// GPU Fusion SDK — multi-GPU work scheduler. Header-only, C++17, standard library only; MIT License.
//
// Divides `total` independent work units (rows, tiles, items) between devices the program created
// itself, normally one per adapter from gpufusion::adapters() (see d3d11.h). Device 0 is the primary
// (the fastest GPU — the discrete one in "Both" mode); it takes batches from the front of the range.
// Every other device is a helper and takes small batches from the back, but only
//   - up to its planned share of the units, and
//   - when it would finish its next batch before the primary could finish everything that is left
//     (the batch is shrunk to fit, or the helper stops),
// so a slow helper cannot hold up the end of the job by more than the error of one estimate. This is
// the same rule as GPU Fusion's job dispatcher uses for whole jobs. Units of a batch that a helper
// fails are re-run on the primary. Every unit is computed exactly once; run() checks the accounting.
//
// Balancer: across repeated runs of one workload, it measures the primary alone first (that run does
// useful work — it is the baseline, not an extra benchmark), then plans splits with a small helper
// share, compares the splits' time per unit with the primary alone (the fastest of the last few runs
// on each side, so one run disturbed by other activity decides nothing), and raises a helper's
// share after gains, halves it after a split that was not faster, and switches the helper off when
// that keeps happening. The policy sets the limits. Nothing here promises a speed-up: on mismatched
// GPUs a split is often slower, and then the Balancer ends up using the primary alone.
//
//     gpufusion::Balancer balancer(devices.size(), gpufusion::Policy::Conservative);
//     for (each job of the same kind) {
//         const gpufusion::Plan plan = balancer.plan(rows);
//         const gpufusion::RunReport report = gpufusion::run(rows, plan, [&](gpufusion::Batch& b) {
//             return computeRows(devices[b.device], b.first, b.count, &b.transferMs); // your work
//         }, balancer.unitsPerMs());
//         balancer.record(plan, report);   // report: per-device units, times, transfers
//     }
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gpufusion {

inline constexpr int kSchedulerVersion = 1;

// How readily helpers are used. Shares are of the work units; gains are of the time per unit compared
// with the primary alone.
enum class Policy { Conservative, Balanced, Performance };

struct PolicyLimits {
    double initialShare;     // a helper's share in its first split, before its speed is known
    double maxShare;         // a helper's share never exceeds this
    double minGain;          // a split must be this much faster than the primary alone (0.05 = 5 %)
    std::uint32_t helperBatches; // a helper's planned share is taken in about this many batches
    int strikes;             // splits in a row without that gain before a helper is switched off
};

inline PolicyLimits limits(Policy p)
{
    switch (p) {
    case Policy::Balanced:    return { 0.15, 0.35, 0.02, 16, 3 };
    case Policy::Performance: return { 0.20, 0.50, 0.00, 24, 3 };
    case Policy::Conservative:
    default:                  return { 0.10, 0.20, 0.05, 12, 2 };
    }
}

inline const char* policyName(Policy p)
{
    switch (p) {
    case Policy::Balanced:    return "balanced";
    case Policy::Performance: return "performance";
    case Policy::Conservative:
    default:                  return "conservative";
    }
}

inline bool parsePolicy(const std::string& text, Policy* out)
{
    for (Policy p : { Policy::Conservative, Policy::Balanced, Policy::Performance }) {
        if (text == policyName(p)) {
            if (out)
                *out = p;
            return true;
        }
    }
    return false;
}

// What run() does: per device whether it takes part, its share of the units and its batch size.
struct Plan {
    std::vector<double> share;          // per device, sums to 1 over enabled devices
    std::vector<std::uint32_t> batch;   // per device, units per batch
    std::vector<char> enabled;          // per device; device 0 (the primary) must be enabled
    std::uint32_t granularity = 1;      // batch sizes are multiples of this
    std::string reason;                 // why the plan looks like this, in words
    std::size_t activeDevices() const { return std::size_t(std::count(enabled.begin(), enabled.end(), char(1))); }
    bool multi() const { return activeDevices() > 1; }
};

// One batch handed to the program's work function.
struct Batch {
    std::size_t device = 0;
    std::uint32_t first = 0, count = 0; // units [first, first + count)
    double transferMs = 0;              // the work function may report its copy time here
};
// Computes the batch on `batch.device` (that device is only ever used by one thread at a time).
// Returns false on failure; a helper's failed units are then re-run on the primary.
using Execute = std::function<bool(Batch&)>;

enum class Stop { None, ShareReached, WouldDelay, NoUnitsLeft, Failed, PrimaryFailed };

inline const char* stopText(Stop s)
{
    switch (s) {
    case Stop::ShareReached:  return "its planned share was done";
    case Stop::WouldDelay:    return "it would have finished after the primary";
    case Stop::NoUnitsLeft:   return "no units were left";
    case Stop::Failed:        return "a batch failed; its units were re-run on the primary";
    case Stop::PrimaryFailed: return "the primary failed";
    case Stop::None:
    default:                  return "";
    }
}

struct DeviceReport {
    std::uint32_t units = 0;       // units this device completed (including re-runs)
    std::uint32_t batches = 0;
    std::uint32_t failedUnits = 0; // units of failed batches (re-run elsewhere)
    std::uint32_t rerunUnits = 0;  // units this device re-ran for a helper that failed
    double busyMs = 0;             // from the start of run() until this device's last batch ended
    double workMs = 0;             // time inside the work function
    double transferMs = 0;         // as reported by the work function
    double scheduleMs = 0;         // the scheduler's own time on this device's thread (taking batches)
    Stop stop = Stop::None;
    double unitsPerMs() const { return workMs > 0 ? units / workMs : 0; }
};

struct RunReport {
    bool ok = false;
    std::string error;
    std::uint32_t total = 0, completed = 0;
    double wallMs = 0;                  // run() start -> every unit done (threads, waiting, re-runs included)
    std::vector<DeviceReport> devices;
    bool exactCoverage = false;         // every unit completed exactly once
    // Time on the critical path that was not work: thread start, waiting for the slowest device,
    // scheduling. Transfers are inside workMs (and reported separately as transferMs).
    double overheadMs() const
    {
        double longest = 0;
        for (const DeviceReport& d : devices)
            longest = std::max(longest, d.workMs);
        return std::max(0.0, wallMs - longest);
    }
};

namespace detail {

using Clock = std::chrono::steady_clock;

inline double msBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

inline std::uint32_t roundUp(double v, std::uint32_t g)
{
    g = std::max(1u, g);
    const double units = std::max(1.0, std::ceil(v / g));
    return std::uint32_t(std::min(units * g, 4294967295.0));
}

// The units not yet taken, [front, back), in one atomic word: lock-free, no mutex.
class RangeQueue {
public:
    explicit RangeQueue(std::uint32_t total) : m_state(std::uint64_t(total)) {}

    bool takeFront(std::uint32_t n, std::uint32_t* first, std::uint32_t* count)
    {
        std::uint64_t s = m_state.load();
        for (;;) {
            const std::uint32_t f = front(s), b = back(s);
            if (f >= b || n == 0)
                return false;
            const std::uint32_t nf = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(f) + n, b));
            if (m_state.compare_exchange_weak(s, pack(nf, b))) {
                *first = f;
                *count = nf - f;
                return true;
            }
        }
    }
    bool takeBack(std::uint32_t n, std::uint32_t* first, std::uint32_t* count)
    {
        std::uint64_t s = m_state.load();
        for (;;) {
            const std::uint32_t f = front(s), b = back(s);
            if (f >= b || n == 0)
                return false;
            const std::uint32_t nb = b - f > n ? b - n : f;
            if (m_state.compare_exchange_weak(s, pack(f, nb))) {
                *first = nb;
                *count = b - nb;
                return true;
            }
        }
    }
    std::uint32_t remaining() const
    {
        const std::uint64_t s = m_state.load();
        return back(s) > front(s) ? back(s) - front(s) : 0;
    }

private:
    static std::uint32_t front(std::uint64_t s) { return std::uint32_t(s >> 32); }
    static std::uint32_t back(std::uint64_t s) { return std::uint32_t(s & 0xFFFFFFFFu); }
    static std::uint64_t pack(std::uint32_t f, std::uint32_t b) { return (std::uint64_t(f) << 32) | b; }
    std::atomic<std::uint64_t> m_state;
};

} // namespace detail

// Runs `total` units according to `plan`. The primary works on the calling thread, each enabled
// helper on its own thread (started here, joined before returning). `expectedUnitsPerMs` (per
// device, 0 = unknown) seeds the helpers' "would it delay the end" rule until this run has measured
// its own rates.
inline RunReport run(std::uint32_t total, const Plan& plan, const Execute& execute,
                     const std::vector<double>& expectedUnitsPerMs = {})
{
    using detail::Clock;
    RunReport r;
    r.total = total;
    const std::size_t n = plan.enabled.size();
    r.devices.resize(n);
    if (n == 0 || !plan.enabled[0] || plan.share.size() != n || plan.batch.size() != n) {
        r.error = "Invalid plan: one entry per device is needed, and device 0 (the primary) must be enabled.";
        return r;
    }
    if (total == 0) {
        r.ok = r.exactCoverage = true;
        return r;
    }

    struct Live {
        std::atomic<std::uint64_t> units{ 0 }, workNs{ 0 };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> failed; // written by its own thread only
    };
    std::vector<Live> live(n);
    detail::RangeQueue queue(total);
    std::atomic<bool> primaryFailed{ false };
    const std::uint32_t g = std::max(1u, plan.granularity);
    const Clock::time_point t0 = Clock::now();

    auto rateOf = [&](std::size_t d) {
        const std::uint64_t ns = live[d].workNs.load();
        if (ns > 0)
            return double(live[d].units.load()) / (double(ns) / 1e6);
        return d < expectedUnitsPerMs.size() ? expectedUnitsPerMs[d] : 0.0;
    };
    auto doBatch = [&](std::size_t d, std::uint32_t first, std::uint32_t count, DeviceReport& rep) {
        Batch b;
        b.device = d;
        b.first = first;
        b.count = count;
        const Clock::time_point s = Clock::now();
        bool ok = false;
        try {
            ok = execute(b);
        } catch (...) {
            ok = false;
        }
        const Clock::time_point e = Clock::now();
        const double w = detail::msBetween(s, e);
        rep.workMs += w;
        rep.transferMs += b.transferMs;
        rep.busyMs = detail::msBetween(t0, e);
        if (ok) {
            rep.units += count;
            ++rep.batches;
            live[d].units += count;
            live[d].workNs += std::uint64_t(w * 1e6);
        }
        return ok;
    };

    auto helper = [&](std::size_t d) {
        DeviceReport& rep = r.devices[d];
        const std::uint32_t budget = std::uint32_t(std::min<double>(total, std::ceil(plan.share[d] * total)));
        std::uint32_t taken = 0;
        for (;;) {
            const Clock::time_point s = Clock::now();
            if (primaryFailed.load()) {
                rep.stop = Stop::PrimaryFailed;
                break;
            }
            if (taken >= budget) {
                rep.stop = Stop::ShareReached;
                break;
            }
            std::uint32_t want = std::max(1u, std::min(plan.batch[d], budget - taken));
            const std::uint32_t left = queue.remaining();
            if (left == 0) {
                rep.stop = Stop::NoUnitsLeft;
                break;
            }
            const double mine = rateOf(d), primary = rateOf(0);
            if (mine > 0 && primary > 0) {
                // The primary alone would finish what is left in left/primary ms; take only what this
                // device finishes well within that (10 % margin for estimation error).
                const double fits = mine * (double(left) / primary) * 0.9;
                if (fits < want) {
                    const std::uint32_t shrunk = std::uint32_t(fits) / g * g;
                    if (shrunk == 0) {
                        rep.stop = Stop::WouldDelay;
                        rep.scheduleMs += detail::msBetween(s, Clock::now());
                        break;
                    }
                    want = shrunk;
                }
            }
            std::uint32_t first = 0, count = 0;
            const bool got = queue.takeBack(want, &first, &count);
            rep.scheduleMs += detail::msBetween(s, Clock::now());
            if (!got) {
                rep.stop = Stop::NoUnitsLeft;
                break;
            }
            taken += count;
            if (!doBatch(d, first, count, rep)) {
                rep.failedUnits += count;
                live[d].failed.emplace_back(first, count);
                rep.stop = Stop::Failed;
                break;
            }
        }
    };

    std::vector<std::thread> threads;
    for (std::size_t d = 1; d < n; ++d) {
        if (!plan.enabled[d])
            continue;
        try {
            threads.emplace_back(helper, d);
        } catch (...) {
            r.devices[d].stop = Stop::Failed; // could not start a thread: the primary does its share
        }
    }

    DeviceReport& primary = r.devices[0];
    for (;;) {
        const Clock::time_point s = Clock::now();
        std::uint32_t first = 0, count = 0;
        const bool got = queue.takeFront(std::max(1u, plan.batch[0]), &first, &count);
        primary.scheduleMs += detail::msBetween(s, Clock::now());
        if (!got)
            break;
        if (!doBatch(0, first, count, primary)) {
            primary.failedUnits += count;
            primary.stop = Stop::Failed;
            primaryFailed = true;
            r.error = "The primary device failed a batch.";
            break;
        }
    }
    for (std::thread& t : threads)
        t.join();
    if (!primaryFailed) {
        // Fallback: units of failed helper batches run on the primary.
        for (std::size_t d = 1; d < n && r.error.empty(); ++d) {
            for (const auto& range : live[d].failed) {
                for (std::uint32_t at = range.first; at < range.first + range.second;) {
                    const std::uint32_t count = std::min(std::max(1u, plan.batch[0]), range.first + range.second - at);
                    if (!doBatch(0, at, count, primary)) {
                        r.error = "The primary device failed while re-running a helper's units.";
                        break;
                    }
                    primary.rerunUnits += count;
                    at += count;
                }
                if (!r.error.empty())
                    break;
            }
        }
    }
    r.wallMs = detail::msBetween(t0, Clock::now());
    for (const DeviceReport& d : r.devices)
        r.completed += d.units;
    r.exactCoverage = r.error.empty() && r.completed == total && queue.remaining() == 0;
    if (r.error.empty() && !r.exactCoverage)
        r.error = "Not every unit was completed exactly once.";
    r.ok = r.exactCoverage;
    return r;
}

// Learns, across runs of one kind of workload, how much each helper should do (see the top of this
// file). Keep one Balancer per workload kind and size class; it is cheap (a few numbers).
class Balancer {
public:
    struct Helper {
        double share = 0;        // share planned for its next split
        int strikes = 0;         // splits in a row that were not fast enough
        bool enabled = true;
        std::string note;        // what happened to it, in words
    };
    static constexpr int kBaselineRuns = 2;     // runs of the primary alone before the first split
    static constexpr int kRefreshEvery = 8;     // re-measure the primary alone after this many splits
    static constexpr double kMinShare = 0.02;   // below this a helper is not worth its overhead
    static constexpr std::size_t kWindow = 3;   // recent runs compared on each side

    explicit Balancer(std::size_t devices = 1, Policy policy = Policy::Conservative, std::uint32_t granularity = 1)
        : m_policy(policy), m_granularity(std::max(1u, granularity)), m_devices(std::max<std::size_t>(1, devices))
    {
        reset();
    }

    void reset()
    {
        m_rate.assign(m_devices, 0.0);
        m_helpers.assign(m_devices, Helper{ limits(m_policy).initialShare, 0, true, {} });
        m_helpers[0] = Helper{ 1.0, 0, true, "primary" };
        m_baseline = m_split = m_lastGain = 0;
        m_baseHistory.clear();
        m_splitHistory.clear();
        m_hasGain = false;
        m_sinceBaseline = 0;
        m_baselineRuns = 0;
        m_splitRuns = 0;
    }

    Policy policy() const { return m_policy; }
    std::size_t devices() const { return m_devices; }
    double baselineMsPerUnit() const { return m_baseline; } // the primary alone (0 = not measured yet)
    double splitMsPerUnit() const { return m_split; }       // splits (0 = none yet)
    bool hasGain() const { return m_hasGain; }
    // baseline / splits - 1 after the last split (> 0: splits were faster). Each side is the fastest of
    // its last kWindow runs: other activity on the PC can only make a run slower, so one disturbed run
    // can neither switch a helper off nor make a split look better than it is; a real slowdown shows
    // once it persists for kWindow runs.
    double lastGain() const { return m_lastGain; }
    const std::vector<double>& unitsPerMs() const { return m_rate; }
    const Helper& helper(std::size_t d) const { return m_helpers.at(d); }
    bool anyHelperEnabled() const
    {
        for (std::size_t d = 1; d < m_devices; ++d)
            if (m_helpers[d].enabled)
                return true;
        return false;
    }

    Plan plan(std::uint32_t total) const
    {
        Plan p;
        p.granularity = m_granularity;
        p.share.assign(m_devices, 0.0);
        p.batch.assign(m_devices, 0u);
        p.enabled.assign(m_devices, char(0));
        p.enabled[0] = 1;
        auto single = [&](const std::string& why) {
            p.share[0] = 1.0;
            p.batch[0] = detail::roundUp(double(total) / 8, m_granularity);
            p.reason = why;
            return p;
        };
        if (m_baseline <= 0 || m_baselineRuns < kBaselineRuns)
            return single("The primary alone first, as the baseline that splits are compared with.");
        if (!anyHelperEnabled()) {
            std::string why = "Primary alone:";
            for (std::size_t d = 1; d < m_devices; ++d)
                why += " helper " + std::to_string(d) + " " + (m_helpers[d].note.empty() ? "off" : m_helpers[d].note) + ".";
            return single(why);
        }
        if (m_sinceBaseline >= kRefreshEvery)
            return single("The primary alone again, to refresh the baseline.");

        const PolicyLimits l = limits(m_policy);
        double helpersTotal = 0, rateSum = m_rate[0];
        for (std::size_t d = 1; d < m_devices; ++d)
            if (m_helpers[d].enabled)
                rateSum += m_rate[d];
        for (std::size_t d = 1; d < m_devices; ++d) {
            if (!m_helpers[d].enabled)
                continue;
            double s = std::min(m_helpers[d].share, l.maxShare);
            if (m_rate[d] > 0 && m_rate[0] > 0 && rateSum > 0)
                s = std::min(s, 1.1 * m_rate[d] / rateSum); // never plan more than its speed justifies
            p.share[d] = s;
            p.enabled[d] = 1;
            helpersTotal += s;
        }
        if (helpersTotal > 0.9) { // the primary always keeps most of the work
            for (std::size_t d = 1; d < m_devices; ++d)
                p.share[d] *= 0.9 / helpersTotal;
            helpersTotal = 0.9;
        }
        p.share[0] = 1.0 - helpersTotal;
        p.batch[0] = detail::roundUp(total * p.share[0] / 8, m_granularity);
        p.reason = "Primary " + percent(p.share[0]);
        for (std::size_t d = 1; d < m_devices; ++d) {
            if (!p.enabled[d])
                continue;
            p.batch[d] = detail::roundUp(total * p.share[d] / l.helperBatches, m_granularity);
            p.reason += ", helper " + std::to_string(d) + " up to " + percent(p.share[d]);
        }
        p.reason += std::string(" (policy ") + policyName(m_policy) + ").";
        return p;
    }

    void record(const Plan& plan, const RunReport& report)
    {
        for (std::size_t d = 1; d < m_devices && d < report.devices.size(); ++d) {
            if (d < plan.enabled.size() && plan.enabled[d] && report.devices[d].failedUnits > 0) {
                m_helpers[d].enabled = false;
                m_helpers[d].note = "switched off: it failed a batch (its units were re-run on the primary)";
            }
        }
        if (!report.ok || report.total == 0 || report.devices.size() != m_devices)
            return;
        for (std::size_t d = 0; d < m_devices; ++d) {
            const double rate = report.devices[d].unitsPerMs();
            if (rate > 0)
                m_rate[d] = ewma(m_rate[d], rate);
        }
        const double perUnit = report.wallMs / report.total;
        if (!plan.multi()) {
            // The first run after a pause may be slow (the GPU leaving its idle clocks): the second
            // baseline run replaces it rather than averaging with it.
            if (m_baselineRuns == 1)
                m_baseHistory.clear();
            push(m_baseHistory, perUnit);
            m_baseline = fastest(m_baseHistory);
            ++m_baselineRuns;
            m_sinceBaseline = 0;
            return;
        }
        ++m_sinceBaseline;
        // The first split is a warm-up: the helper's first batches include one-time costs (its textures,
        // the driver's first use of the shader), so it is neither compared nor counted.
        if (m_splitRuns++ == 0) {
            for (std::size_t d = 1; d < m_devices; ++d)
                if (plan.enabled[d] && m_helpers[d].enabled)
                    m_helpers[d].note = "first split (a warm-up, not judged)";
            return;
        }
        push(m_splitHistory, perUnit);
        m_split = fastest(m_splitHistory);
        if (m_baseline <= 0 || m_split <= 0)
            return;
        m_lastGain = m_baseline / m_split - 1.0;
        m_hasGain = true;
        const PolicyLimits l = limits(m_policy);
        for (std::size_t d = 1; d < m_devices; ++d) {
            if (!plan.enabled[d] || !m_helpers[d].enabled)
                continue;
            Helper& h = m_helpers[d];
            const DeviceReport& rep = report.devices[d];
            if (rep.units == 0) {
                ++h.strikes;
                h.share *= 0.5;
                h.note = "did no work (it would have finished after the primary)";
            } else if (m_lastGain < l.minGain) {
                ++h.strikes;
                h.share *= 0.5;
                h.note = "the split was " + percent(-m_lastGain) + " slower than the primary alone; share halved";
                if (m_lastGain >= 0)
                    h.note = "the split was only " + percent(m_lastGain) + " faster than the primary alone; share halved";
            } else {
                h.strikes = 0;
                h.note = "the split was " + percent(m_lastGain) + " faster than the primary alone";
                if (rep.stop == Stop::ShareReached)
                    h.share = std::min(h.share * 1.5, l.maxShare);
            }
            if (h.strikes >= l.strikes || h.share < kMinShare) {
                h.enabled = false;
                h.note = "switched off: splits were not at least " + percent(l.minGain) + " faster than the primary alone ("
                         + std::to_string(h.strikes) + " in a row)";
            }
        }
    }

    // What the Balancer has learned, as one line of text, so a program can keep it between runs (for
    // example in its settings) and start with the shares it found last time instead of measuring the
    // primary alone again. restore() accepts only text from save() for the same number of devices and
    // policy; anything else leaves the Balancer unchanged and returns false. A restored Balancer
    // re-measures the primary alone after kRefreshEvery splits, as usual, so stale data is replaced.
    std::string save() const
    {
        std::string s = "gfbalancer2 " + std::to_string(int(m_policy)) + " " + std::to_string(m_devices) + " " + std::to_string(m_baselineRuns)
                      + " " + std::to_string(m_sinceBaseline) + " " + std::to_string(m_hasGain ? 1 : 0) + " " + num(m_baseline) + " " + num(m_split) + " "
                      + num(m_lastGain);
        for (std::size_t d = 0; d < m_devices; ++d)
            s += " " + num(m_rate[d]) + " " + num(m_helpers[d].share) + " " + std::to_string(m_helpers[d].strikes) + " " + (m_helpers[d].enabled ? "1" : "0");
        for (const std::vector<double>* h : { &m_baseHistory, &m_splitHistory }) {
            s += " " + std::to_string(h->size());
            for (double v : *h)
                s += " " + num(v);
        }
        return s;
    }
    bool restore(const std::string& text)
    {
        const char* p = text.c_str();
        auto word = [&](std::string* out) {
            while (*p == ' ')
                ++p;
            const char* b = p;
            while (*p && *p != ' ')
                ++p;
            out->assign(b, p);
            return !out->empty();
        };
        auto number = [&](double* out) {
            std::string w;
            if (!word(&w))
                return false;
            char* end = nullptr;
            *out = std::strtod(w.c_str(), &end);
            return end && *end == 0 && std::isfinite(*out);
        };
        std::string tag;
        double policy = 0, devices = 0, baselineRuns = 0, since = 0, hasGain = 0, baseline = 0, split = 0, lastGain = 0;
        if (!word(&tag) || tag != "gfbalancer2" || !number(&policy) || !number(&devices) || !number(&baselineRuns) || !number(&since)
            || !number(&hasGain) || !number(&baseline) || !number(&split) || !number(&lastGain))
            return false;
        if (int(policy) != int(m_policy) || std::size_t(devices) != m_devices)
            return false;
        std::vector<double> rate(m_devices);
        std::vector<Helper> helpers(m_helpers);
        for (std::size_t d = 0; d < m_devices; ++d) {
            double share = 0, strikes = 0, enabled = 0;
            if (!number(&rate[d]) || !number(&share) || !number(&strikes) || !number(&enabled) || share < 0 || share > 1)
                return false;
            helpers[d].share = share;
            helpers[d].strikes = int(strikes);
            helpers[d].enabled = enabled != 0;
            helpers[d].note = d == 0 ? "primary" : (helpers[d].enabled ? "restored from an earlier run" : "switched off in an earlier run");
        }
        std::vector<double> histories[2];
        for (auto& h : histories) {
            double n = 0;
            if (!number(&n) || n < 0 || n > double(kWindow))
                return false;
            for (int i = 0; i < int(n); ++i) {
                double v = 0;
                if (!number(&v))
                    return false;
                h.push_back(v);
            }
        }
        m_rate = rate;
        m_helpers = helpers;
        m_baselineRuns = int(baselineRuns);
        m_sinceBaseline = int(since);
        m_splitRuns = std::max(1, int(since)); // restored: the warm-up split happened in an earlier run
        m_hasGain = hasGain != 0;
        m_baseline = baseline;
        m_split = split;
        m_lastGain = lastGain;
        m_baseHistory = histories[0];
        m_splitHistory = histories[1];
        return true;
    }

private:
    static std::string num(double v)
    {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return buf;
    }
    static double ewma(double old, double v) { return old > 0 ? 0.5 * old + 0.5 * v : v; }
    static void push(std::vector<double>& history, double v)
    {
        history.push_back(v);
        if (history.size() > kWindow)
            history.erase(history.begin());
    }
    static double fastest(const std::vector<double>& history)
    {
        return history.empty() ? 0.0 : *std::min_element(history.begin(), history.end());
    }
    static std::string percent(double f)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f %%", 100.0 * f);
        return buf;
    }

    Policy m_policy;
    std::uint32_t m_granularity;
    std::size_t m_devices;
    std::vector<double> m_rate;
    std::vector<Helper> m_helpers;
    double m_baseline = 0, m_split = 0, m_lastGain = 0;
    std::vector<double> m_baseHistory, m_splitHistory; // ms per unit, last kWindow runs
    bool m_hasGain = false;
    int m_sinceBaseline = 0;
    int m_baselineRuns = 0;
    int m_splitRuns = 0;    // splits recorded so far (the first is a warm-up)
};

} // namespace gpufusion
