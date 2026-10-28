#include "benchmarks/LabRunner.h"

#include "core/Logging.h"
#include "platform/windows/KmtStatsProvider.h"
#include "platform/windows/NvmlProvider.h"
#include "workloads/SplitHash.h"
#include "workloads/SplitPlan.h"

#include <gpufusion/scheduler.h>

#include <QDir>
#include <QJsonArray>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

namespace gf::bench {

namespace {

double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

quint64 processCpu100ns()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    auto v = [](const FILETIME& f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return v(k) + v(u);
}

struct Gpu {
    const hw::Adapter* adapter;
    Target target;
};

QList<Gpu> usableGpus(const hw::DetectionResult& d)
{
    QList<Gpu> out;
    for (const hw::Adapter& a : d.adapters) {
        if (a.dxgi && !a.isSoftware() && a.dxgi->d3d11.device.ok())
            out << Gpu{ &a, gpuTarget(a) };
    }
    return out;
}

Target combined(const QList<Target>& parts)
{
    Target t;
    QStringList names, drivers;
    for (const Target& p : parts) {
        names << p.name;
        drivers << (p.driverVersion.isEmpty() ? QStringLiteral("-") : p.driverVersion);
    }
    t.name = names.join(QStringLiteral(" + "));
    t.driverVersion = drivers.join(QStringLiteral(" + "));
    return t;
}

// Warm-up + measured runs of one mode, with CPU accounting and D3DKMT engine evidence.
class Loop {
public:
    Loop(const LabOptions& o, const QList<mon::MonitorTarget>& watch, const Progress& progress, const std::atomic<bool>* cancel, int modes)
        : m_o(o), m_watch(watch), m_progress(progress), m_cancel(cancel), m_total(modes * (o.warmupRuns + o.measuredRuns))
    {
        m_nvml.setTargets(watch); // performance-state evidence where NVML is available
    }

    bool cancelled() const { return m_cancel && m_cancel->load(); }
    // A mode that cannot run: recorded as failed with `error` (or as cancelled, when the reason is
    // that the session was cancelled), its runs counted as done.
    void skip(BenchmarkResult& r, const QString& error)
    {
        r.status = cancelled() ? Status::Cancelled : Status::Failed;
        r.error = cancelled() ? QString() : error;
        r.started = QDateTime::currentDateTime();
        m_done += m_o.warmupRuns + m_o.measuredRuns;
        report(r.name);
    }
    void report(const QString& step) { if (m_progress) m_progress(m_done, m_total, step); }

    void run(BenchmarkResult& r, const std::function<bool(RunSample&, QString*)>& iteration)
    {
        r.warmupRuns = m_o.warmupRuns;
        r.measuredRuns = m_o.measuredRuns;
        r.started = QDateTime::currentDateTime();
        r.cpuPercent = r.gpuBusyPercent = r.gpuDedicatedMB = r.peakWorkingSetMB = mon::MetricValue::unsupported(QStringLiteral("Not measured for this test"));
        const double t0 = nowMs();
        const int perMode = m_o.warmupRuns + m_o.measuredRuns;
        if (cancelled()) {
            r.status = Status::Cancelled;
            m_done += perMode;
            return;
        }
        mon::win::KmtStatsProvider kmt;
        kmt.setTargets(m_watch);
        QList<mon::AdapterSample> samples;
        for (const mon::MonitorTarget& t : m_watch)
            samples << mon::AdapterSample{ t.luid, t.name };
        bool failed = false;
        int doneHere = 0;
        auto once = [&](bool measured, const QString& step) {
            report(step);
            RunSample sample;
            QString error;
            if (!iteration(sample, &error)) {
                r.status = Status::Failed;
                r.error = error;
                failed = true;
            } else if (measured) {
                r.samples << sample;
            }
        };

        // Warm-up: at least the configured number of runs AND at least kMinWarmupMs, so GPUs that
        // were idle (e.g. during a CPU-only mode) have left their low-power clock states. Found
        // necessary: with a single warm-up run the GT 730 measured up to 1.7x slower right after
        // an idle period.
        constexpr double kMinWarmupMs = 1000;
        const double warm0 = nowMs();
        int warmups = 0;
        while (!failed && !cancelled() && (warmups < m_o.warmupRuns || (nowMs() - warm0 < kMinWarmupMs && warmups < 200))) {
            once(false, QStringLiteral("%1 — warm-up %2").arg(r.name).arg(warmups + 1));
            ++warmups;
            if (warmups <= m_o.warmupRuns) {
                ++m_done;
                ++doneHere;
            }
        }
        r.warmupRuns = warmups;

        QList<mon::AdapterSample> pstates;
        if (m_nvml.isActive()) {
            pstates = samples;
            m_nvml.sample(pstates);
        }
        kmt.sample(samples);
        const quint64 cpu0 = processCpu100ns();
        const double wall0 = nowMs();
        for (int k = 0; k < m_o.measuredRuns && !failed && !cancelled(); ++k) {
            once(true, QStringLiteral("%1 — run %2 of %3").arg(r.name).arg(k + 1).arg(m_o.measuredRuns));
            ++m_done;
            ++doneHere;
        }
        m_done += perMode - doneHere; // skipped after failure/cancel
        if (!failed && r.samples.size() == m_o.measuredRuns) {
            const double wall = nowMs() - wall0;
            kmt.sample(samples);
            if (wall > 0)
                r.cpuPercent = mon::MetricValue::ok(100.0 * double(processCpu100ns() - cpu0) / 1e4 / wall / std::max(1u, std::thread::hardware_concurrency()),
                                                    QStringLiteral("GPU Fusion's CPU time during the measured runs, share of all cores"));
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
                r.peakWorkingSetMB = mon::MetricValue::ok(double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0),
                                                          QStringLiteral("GPU Fusion's peak working set so far (whole process, window included)"));
                r.notes << QStringLiteral("GPU Fusion's memory after the measured runs: working set %1 MB, private %2 MB (peak working set so far %3 MB).")
                               .arg(double(pmc.WorkingSetSize) / 1048576.0, 0, 'f', 0).arg(double(pmc.PrivateUsage) / 1048576.0, 0, 'f', 0)
                               .arg(double(pmc.PeakWorkingSetSize) / 1048576.0, 0, 'f', 0);
            }
            if (!pstates.isEmpty()) {
                QList<mon::AdapterSample> after = samples;
                m_nvml.sample(after);
                for (qsizetype i = 0; i < pstates.size() && i < after.size(); ++i) {
                    if (pstates[i].pstate.isOk() && after[i].pstate.isOk())
                        r.notes << QStringLiteral("%1: performance state %2 at the start of the measured runs, %3 at the end")
                                       .arg(pstates[i].name, mon::formatMetric(pstates[i].pstate, mon::Unit::PState),
                                            mon::formatMetric(after[i].pstate, mon::Unit::PState));
                }
            }
            for (const mon::AdapterSample& s : samples) {
                QList<mon::EngineSample> engines = s.engines;
                auto busyOf = [](const mon::EngineSample& e) { return e.utilization.isOk() ? e.utilization.value : -1.0; };
                std::sort(engines.begin(), engines.end(), [&](const mon::EngineSample& a, const mon::EngineSample& b) { return busyOf(a) > busyOf(b); });
                QStringList busy;
                for (const mon::EngineSample& e : engines) {
                    if (e.utilization.isOk() && e.utilization.value >= 1.0 && busy.size() < 2)
                        busy << QStringLiteral("%1 %2 %").arg(e.name).arg(e.utilization.value, 0, 'f', 0);
                }
                r.notes << (busy.isEmpty() ? QStringLiteral("%1: no engine busier than 1 % during the measured runs").arg(s.name)
                                           : QStringLiteral("%1: busiest engines during the measured runs — %2").arg(s.name, busy.join(QStringLiteral(", "))));
            }
        }
        if (!failed && r.samples.size() < m_o.measuredRuns)
            r.status = Status::Cancelled;
        summarize(r);
        if (r.correct.has_value() && !*r.correct)
            r.warnings << QStringLiteral("Output did not match the reference — the timings of this result are not trustworthy.");
        if (r.duration.n >= 3 && r.duration.cv() > 0.10)
            r.warnings << QStringLiteral("Run-to-run variation is high (%1 %); other activity may have interfered.").arg(100 * r.duration.cv(), 0, 'f', 1);
        r.totalMs = nowMs() - t0;
        qCInfo(lcBench).noquote() << QStringLiteral("Lab: %1: %2, mean %3 ms ± %4").arg(r.name, statusName(r.status)).arg(r.duration.mean, 0, 'f', 3)
                                         .arg(r.duration.stddev, 0, 'f', 3);
    }

private:
    const LabOptions& m_o;
    QList<mon::MonitorTarget> m_watch;
    Progress m_progress;
    const std::atomic<bool>* m_cancel;
    int m_done = 0, m_total = 0;
    mon::win::NvmlProvider m_nvml;
};

QList<mon::MonitorTarget> gpuWatch(const hw::DetectionResult& d)
{
    QList<mon::MonitorTarget> out;
    for (const mon::MonitorTarget& t : mon::targetsFrom(d)) {
        if (!t.software)
            out << t;
    }
    return out;
}

BenchmarkResult failedResult(const QString& id, const QString& name, const QString& category, const QString& error)
{
    BenchmarkResult r;
    r.testId = id;
    r.name = name;
    r.category = category;
    r.status = Status::Failed;
    r.error = error;
    r.started = QDateTime::currentDateTime();
    return r;
}

} // namespace

QString labDir(const QString& benchmarksDir)
{
    return QDir(benchmarksDir).filePath(QStringLiteral("lab"));
}

LabOptions labOptions(int warmupRuns, int measuredRuns, const QString& preset, const QString& labDirectory)
{
    LabOptions o;
    o.warmupRuns = warmupRuns;
    o.measuredRuns = measuredRuns;
    o.preset = preset;
    o.hash = hashPreset(preset);
    o.mediaDir = QDir(labDirectory).filePath(QStringLiteral("media"));
    return o;
}

// ---- Experiment 1: split compute --------------------------------------------------------------

Session runSplitExperiment(const hw::DetectionResult& detection, const LabOptions& o, const Progress& progress, const std::atomic<bool>* cancel)
{
    Session session = beginSession(detection);
    const QString category = QStringLiteral("Workload Lab: split compute");
    const QList<Gpu> gpus = usableGpus(detection);
    if (gpus.isEmpty()) {
        session.results << failedResult(QStringLiteral("lab.split.single"), QStringLiteral("Split compute"), category,
                                        QStringLiteral("No physical GPU with Direct3D 11 is available."));
        return session;
    }
    const unsigned cpuThreads = o.cpuThreads ? o.cpuThreads : std::max(1u, std::thread::hardware_concurrency() - 1);
    const Target cpuTarget{ QStringLiteral("CPU: %1").arg(cpuName()), 0, 0, 0, {} };
    const QString cpuLabel = QStringLiteral("CPU, %1 thread%2").arg(cpuThreads).arg(cpuThreads == 1 ? QString() : QStringLiteral("s"));

    const int modes = int(gpus.size()) + 1 + (gpus.size() >= 2 ? 3 : 0) + 1;
    Loop loop(o, gpuWatch(detection), progress, cancel, modes);

    loop.report(QStringLiteral("Computing the CPU reference image…"));
    const std::vector<std::uint32_t> reference = work::hashRowsCpu(o.hash, 0, o.hash.height, 0);

    // Devices are created once, before any timing.
    std::vector<std::unique_ptr<work::D3D11HashDevice>> devices;
    QStringList deviceErrors;
    for (const Gpu& g : gpus) {
        QString error;
        devices.push_back(work::D3D11HashDevice::create(g.adapter->dxgi->luid, &error));
        deviceErrors << error;
    }
    auto gpuWorker = [&](qsizetype i) { return work::SplitWorker{ gpus[i].adapter->name(), devices[std::size_t(i)].get(), 0 }; };
    const work::SplitWorker cpuWorker{ cpuLabel, nullptr, cpuThreads };

    auto runMode = [&](const QString& id, const QString& name, const std::vector<work::SplitWorker>& workers, const work::SplitSpec& spec,
                       const Target& target, const QString& measures) {
        BenchmarkResult r;
        r.testId = id;
        r.name = name;
        r.category = category;
        r.target = target;
        r.measures = measures;
        QStringList labels;
        for (const work::SplitWorker& w : workers)
            labels << w.label;
        r.api = QStringLiteral("Direct3D 11 pixel shader per GPU, own thread each; results copied to one image in CPU memory");
        r.params = hashParamsJson(o.hash);
        r.params.insert(QStringLiteral("workers"), QJsonArray::fromStringList(labels));
        if (spec.mode == work::SplitSpec::Mode::Dynamic)
            r.params.insert(QStringLiteral("chunkRows"), qint64(spec.chunkRows));
        r.correctness = QStringLiteral("Bit-exact match of the assembled image with the CPU reference");
        if (std::any_of(workers.begin(), workers.end(), [](const work::SplitWorker& w) { return !w.gpu && w.cpuThreads == 0; })) {
            loop.skip(r, QStringLiteral("A Direct3D device could not be created for one of the GPUs: %1").arg(deviceErrors.join(QStringLiteral(" ")).trimmed()));
            session.results << r;
            return;
        }
        loop.run(r, [&](RunSample& s, QString* error) {
            const work::SplitResult res = work::runSplit(o.hash, workers, spec);
            if (!res.ok) {
                *error = res.error;
                return false;
            }
            s.durationMs = res.wallMs;
            s.correct = work::firstMismatch(res.image, reference) < 0;
            for (std::size_t w = 0; w < res.workers.size(); ++w) {
                const QString k = QStringLiteral("w%1.").arg(w);
                s.metrics.insert(k + QStringLiteral("rowsPercent"), 100.0 * res.workers[w].rows / o.hash.height);
                s.metrics.insert(k + QStringLiteral("busyMs"), res.workers[w].busyMs);
                s.metrics.insert(k + QStringLiteral("computeMs"), res.workers[w].computeMs);
                if (workers[w].gpu)
                    s.metrics.insert(k + QStringLiteral("readbackMs"), res.workers[w].readbackMs);
            }
            return true;
        });
        session.results << r;
    };

    const QString wholeImage = QStringLiteral("time from the common start until the whole image is in CPU memory");
    work::SplitSpec all{ work::SplitSpec::Mode::Static, { 1.0 }, 16, o.chunkRows };
    QList<double> singleMs;
    for (qsizetype i = 0; i < gpus.size(); ++i) {
        runMode(QStringLiteral("lab.split.single"), QStringLiteral("%1 alone").arg(gpus[i].adapter->name()), { gpuWorker(i) }, all, gpus[i].target, wholeImage);
        const BenchmarkResult& r = session.results.last();
        singleMs << (r.status == Status::Completed && r.correct.value_or(false) ? r.duration.mean : 0.0);
    }
    runMode(QStringLiteral("lab.split.single"), QStringLiteral("%1 alone").arg(cpuLabel), { cpuWorker }, all, cpuTarget, wholeImage);

    if (gpus.size() >= 2) {
        std::vector<work::SplitWorker> both;
        QList<Target> targets;
        std::vector<double> times;
        for (qsizetype i = 0; i < gpus.size(); ++i) {
            both.push_back(gpuWorker(i));
            targets << gpus[i].target;
            times.push_back(singleMs[i]);
        }
        const Target pair = combined(targets);
        runMode(QStringLiteral("lab.split.equal"), QStringLiteral("Even split between the GPUs"), both,
                { work::SplitSpec::Mode::Static, std::vector<double>(both.size(), 1.0), 16, o.chunkRows }, pair, wholeImage);
        const bool haveTimes = std::all_of(times.begin(), times.end(), [](double t) { return t > 0; });
        if (haveTimes) {
            const std::vector<double> shares = work::balancedShares(times);
            runMode(QStringLiteral("lab.split.balanced"), QStringLiteral("Split by measured speed"), both,
                    { work::SplitSpec::Mode::Static, shares, 16, o.chunkRows }, pair, wholeImage);
            QStringList planned;
            for (std::size_t i = 0; i < shares.size(); ++i)
                planned << QStringLiteral("%1 %2 %").arg(both[i].label).arg(100 * shares[i], 0, 'f', 1);
            session.results.last().notes.prepend(QStringLiteral("Rows planned from this session's single-GPU times: %1").arg(planned.join(QStringLiteral(", "))));
        } else {
            BenchmarkResult r = failedResult(QStringLiteral("lab.split.balanced"), QStringLiteral("Split by measured speed"), category, {});
            loop.skip(r, QStringLiteral("Needs a correct single-GPU time for every GPU."));
            session.results << r;
        }
        runMode(QStringLiteral("lab.split.dynamic"), QStringLiteral("Shared queue, %1-row items").arg(o.chunkRows), both,
                { work::SplitSpec::Mode::Dynamic, {}, 16, o.chunkRows }, pair, wholeImage);
    }
    std::vector<work::SplitWorker> everyone;
    QList<Target> everyoneTargets;
    for (qsizetype i = 0; i < gpus.size(); ++i) {
        everyone.push_back(gpuWorker(i));
        everyoneTargets << gpus[i].target;
    }
    everyone.push_back(cpuWorker);
    everyoneTargets << cpuTarget;
    runMode(QStringLiteral("lab.split.dynamic-cpu"), QStringLiteral("Shared queue, GPUs + %1").arg(cpuLabel), everyone,
            { work::SplitSpec::Mode::Dynamic, {}, 16, o.chunkRows }, combined(everyoneTargets), wholeImage);
    loop.report(QStringLiteral("Done"));
    return session;
}

// ---- Experiment 3: adaptive multi-GPU scheduler ------------------------------------------------

Session runAdaptiveExperiment(const hw::DetectionResult& detection, const LabOptions& o, const Progress& progress, const std::atomic<bool>* cancel)
{
    Session session = beginSession(detection);
    const QString category = QStringLiteral("Workload Lab: adaptive multi-GPU scheduler");
    const QList<Gpu> gpus = usableGpus(detection);
    if (gpus.isEmpty()) {
        session.results << failedResult(QStringLiteral("lab.split.single"), QStringLiteral("Adaptive split"), category,
                                        QStringLiteral("No physical GPU with Direct3D 11 is available."));
        return session;
    }
    QList<gpufusion::Policy> policies;
    for (const QString& name : o.policies) {
        gpufusion::Policy p;
        if (gpufusion::parsePolicy(name.toStdString(), &p))
            policies << p;
    }
    const int modes = int(gpus.size()) + (gpus.size() >= 2 ? int(policies.size()) + 1 : 0);
    Loop loop(o, gpuWatch(detection), progress, cancel, modes);
    loop.report(QStringLiteral("Computing the CPU reference image…"));
    const std::vector<std::uint32_t> reference = work::hashRowsCpu(o.hash, 0, o.hash.height, 0);

    // Devices, the image buffer and the reference are created once, before any timing.
    std::vector<std::unique_ptr<work::D3D11HashDevice>> devices;
    QStringList deviceErrors;
    for (const Gpu& g : gpus) {
        QString error;
        devices.push_back(work::D3D11HashDevice::create(g.adapter->dxgi->luid, &error));
        deviceErrors << error;
    }
    std::vector<std::uint32_t> image(reference.size(), 0u);
    constexpr std::uint32_t kGranularity = 8; // rows
    const QString wholeImage = QStringLiteral("time from the scheduler's start until the whole image is in CPU memory (threads, transfers, "
                                              "waiting and any re-runs included)");

    // order: GPU indexes, the primary first. failHelper: every helper batch fails (fallback check).
    auto runMode = [&](const QString& id, const QString& name, const QList<qsizetype>& order, std::optional<gpufusion::Policy> policy,
                       bool failHelper) {
        BenchmarkResult r;
        r.testId = id;
        r.name = name;
        r.category = category;
        r.measures = wholeImage;
        QStringList labels;
        QList<Target> targets;
        for (qsizetype i : order) {
            labels << gpus[i].adapter->name();
            targets << gpus[i].target;
        }
        r.target = order.size() == 1 ? targets.first() : combined(targets);
        r.api = QStringLiteral("Direct3D 11 pixel shader per GPU, own thread each; GPU Fusion SDK scheduler v%1: the primary takes batches from "
                               "the start of the image, a helper small batches from the end; rows copied to one image in CPU memory")
                    .arg(gpufusion::kSchedulerVersion);
        r.params = hashParamsJson(o.hash);
        r.params.insert(QStringLiteral("workers"), QJsonArray::fromStringList(labels));
        r.params.insert(QStringLiteral("granularityRows"), qint64(kGranularity));
        if (policy)
            r.params.insert(QStringLiteral("policy"), QString::fromLatin1(gpufusion::policyName(*policy)));
        r.correctness = QStringLiteral("Bit-exact match of the assembled image with the CPU reference; every row computed exactly once");
        for (qsizetype i : order) {
            if (!devices[std::size_t(i)]) {
                loop.skip(r, QStringLiteral("A Direct3D device could not be created for one of the GPUs: %1").arg(deviceErrors.join(QStringLiteral(" ")).trimmed()));
                session.results << r;
                return;
            }
        }
        const std::size_t n = std::size_t(order.size());
        gpufusion::Balancer balancer(n, policy.value_or(gpufusion::Policy::Conservative), kGranularity);
        int splitRuns = 0, iterations = 0;
        gpufusion::Plan fixed; // single-GPU and fallback modes: the same plan every run
        fixed.granularity = kGranularity;
        fixed.enabled.assign(n, char(1));
        fixed.share.assign(n, 0.0);
        fixed.batch.assign(n, 0u);
        fixed.share[0] = n == 1 ? 1.0 : 0.85;
        fixed.batch[0] = gpufusion::detail::roundUp(o.hash.height * fixed.share[0] / 8, kGranularity);
        for (std::size_t d = 1; d < n; ++d) {
            fixed.share[d] = 0.15 / double(n - 1);
            fixed.batch[d] = gpufusion::detail::roundUp(o.hash.height * fixed.share[d] / 12, kGranularity);
        }
        loop.run(r, [&](RunSample& s, QString* error) {
            const gpufusion::Plan plan = policy ? balancer.plan(o.hash.height) : fixed;
            std::fill(image.begin(), image.end(), 0xDEADBEEFu); // missing rows cannot pass the check
            QString workError;
            const gpufusion::RunReport rep = gpufusion::run(o.hash.height, plan, [&](gpufusion::Batch& b) {
                if (failHelper && b.device != 0)
                    return false;
                double computeMs = 0, readbackMs = 0;
                const QString e = devices[std::size_t(order[qsizetype(b.device)])]->computeInto(
                    o.hash, b.first, b.count, image.data() + std::size_t(b.first) * o.hash.width, &computeMs, &readbackMs);
                b.transferMs = readbackMs;
                if (!e.isEmpty() && workError.isEmpty() && b.device == 0)
                    workError = e;
                return e.isEmpty();
            }, balancer.unitsPerMs());
            if (policy)
                balancer.record(plan, rep);
            if (!rep.ok) {
                *error = QString::fromStdString(rep.error) + (workError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(workError));
                return false;
            }
            s.durationMs = rep.wallMs;
            s.correct = rep.exactCoverage && work::firstMismatch(image, reference) < 0;
            s.note = QString::fromStdString(plan.reason);
            double scheduleMs = 0;
            for (std::size_t d = 0; d < n; ++d) {
                const gpufusion::DeviceReport& dr = rep.devices[d];
                const QString k = QStringLiteral("w%1.").arg(d);
                s.metrics.insert(k + QStringLiteral("rowsPercent"), 100.0 * dr.units / o.hash.height);
                s.metrics.insert(k + QStringLiteral("busyMs"), dr.busyMs);
                s.metrics.insert(k + QStringLiteral("computeMs"), dr.workMs - dr.transferMs);
                s.metrics.insert(k + QStringLiteral("readbackMs"), dr.transferMs);
                s.metrics.insert(k + QStringLiteral("batches"), dr.batches);
                if (d > 0)
                    s.metrics.insert(k + QStringLiteral("rerunRows"), rep.devices[0].rerunUnits);
                scheduleMs += dr.scheduleMs;
            }
            s.metrics.insert(QStringLiteral("split"), plan.multi() ? 1.0 : 0.0);
            s.metrics.insert(QStringLiteral("overheadMs"), rep.overheadMs());
            s.metrics.insert(QStringLiteral("scheduleMs"), scheduleMs);
            ++iterations;
            splitRuns += plan.multi() ? 1 : 0;
            return true;
        });
        if (policy && r.status == Status::Completed) {
            // iterations include the warm-up runs; the samples are the measured ones.
            int measuredSplits = 0;
            for (const RunSample& s : r.samples)
                measuredSplits += s.metrics.value(QStringLiteral("split")) > 0 ? 1 : 0;
            r.notes.prepend(QStringLiteral("Runs where the helper took part: %1 of %2 measured (%3 of %4 including the warm-up).")
                                .arg(measuredSplits).arg(r.samples.size()).arg(splitRuns).arg(iterations));
            for (std::size_t d = 1; d < n; ++d) {
                const gpufusion::Balancer::Helper& h = balancer.helper(d);
                QString line = QStringLiteral("%1 after the last run: %2, next planned share %3")
                                   .arg(labels[qsizetype(d)], h.enabled ? QStringLiteral("in use") : QStringLiteral("not used"))
                                   .arg(100.0 * h.share, 0, 'f', 1);
                line += QStringLiteral(" %");
                if (!h.note.empty())
                    line += QStringLiteral(" — ") + QString::fromStdString(h.note);
                r.notes.insert(1, line + u'.');
                r.params.insert(QStringLiteral("helperEnabledAtEnd"), h.enabled);
            }
            if (balancer.baselineMsPerUnit() > 0)
                r.notes.insert(1, QStringLiteral("Primary alone, as learned by the Balancer: %1 ms per row; splits: %2 ms per row.")
                                      .arg(balancer.baselineMsPerUnit(), 0, 'f', 4).arg(balancer.splitMsPerUnit(), 0, 'f', 4));
        }
        session.results << r;
    };

    QList<double> singleMs;
    for (qsizetype i = 0; i < gpus.size(); ++i) {
        runMode(QStringLiteral("lab.split.single"), QStringLiteral("%1 alone").arg(gpus[i].adapter->name()), { i }, std::nullopt, false);
        const BenchmarkResult& r = session.results.last();
        singleMs << (r.status == Status::Completed && r.correct.value_or(false) ? r.duration.mean : 0.0);
    }
    if (gpus.size() >= 2) {
        // The primary: the fastest GPU alone in this session (normally the discrete GPU); if none
        // completed, the discrete GPU.
        qsizetype primary = -1;
        for (qsizetype i = 0; i < gpus.size(); ++i)
            if (singleMs[i] > 0 && (primary < 0 || singleMs[i] < singleMs[primary]))
                primary = i;
        if (primary < 0) {
            primary = 0;
            for (qsizetype i = 0; i < gpus.size(); ++i)
                if (gpus[i].adapter->classification.value == hw::AdapterClass::Discrete)
                    primary = i;
        }
        QList<qsizetype> order{ primary };
        for (qsizetype i = 0; i < gpus.size(); ++i)
            if (i != primary)
                order << i;
        const QString primaryName = gpus[primary].adapter->name();
        for (gpufusion::Policy p : policies) {
            const QString policyText = QString::fromLatin1(gpufusion::policyName(p));
            runMode(QStringLiteral("lab.adaptive.%1").arg(policyText),
                    QStringLiteral("Adaptive, %1 policy (%2 first)").arg(policyText, primaryName), order, p, false);
        }
        runMode(QStringLiteral("lab.adaptive.fallback"), QStringLiteral("Fallback: helper fails, %1 re-runs its rows").arg(primaryName), order,
                std::nullopt, true);
    }
    loop.report(QStringLiteral("Done"));
    return session;
}

// ---- Experiment 2: video decoding ---------------------------------------------------------------

Session runDecodeExperiment(const hw::DetectionResult& detection, const LabOptions& o, const Progress& progress, const std::atomic<bool>* cancel)
{
    work::ComScope com;
    Session session = beginSession(detection);
    const QString category = QStringLiteral("Workload Lab: video decoding");
    const QList<Gpu> gpus = usableGpus(detection);

    struct Decoder {
        QString label;
        Target target;
        std::optional<quint64> luid;
        std::unique_ptr<work::VideoDecoder> a, b; // two instances for two-stream modes
        QString error;
        std::optional<bool> identical;           // frames identical to the software decoder's
        bool gpuOutput = false;
    };
    std::vector<Decoder> decoders;
    decoders.push_back({ QStringLiteral("Software (CPU)"), Target{ QStringLiteral("CPU: %1").arg(cpuName()), 0, 0, 0, {} }, std::nullopt, {}, {}, {}, {}, false });
    for (const Gpu& g : gpus)
        decoders.push_back({ g.adapter->name(), g.target, g.adapter->dxgi->luid, {}, {}, {}, {}, false });

    const int twoSplit = gpus.size() >= 2 ? 1 : 0;
    const int modes = int(decoders.size()) * 2 + twoSplit;
    Loop loop(o, gpuWatch(detection), progress, cancel, modes);

    QString clipError;
    loop.report(QStringLiteral("Preparing the test video (created once, then reused)…"));
    const QString clip = work::ensureTestClip(o.mediaDir, o.clip, &clipError, [&](int percent) {
        loop.report(QStringLiteral("Creating the test video with the software H.264 encoder: %1 %").arg(percent));
    });
    QJsonObject params{ { "clip", work::testClipFileName(o.clip) }, { "frames", qint64(o.clip.frames) },
                        { "width", qint64(o.clip.width) }, { "height", qint64(o.clip.height) } };

    // Open every decoder and check its frames against the software decoder (untimed).
    std::vector<std::uint64_t> referenceHashes;
    for (Decoder& d : decoders) {
        if (clip.isEmpty()) {
            d.error = clipError;
            continue;
        }
        if (cancel && cancel->load())
            break;
        loop.report(QStringLiteral("Checking decoded frames: %1…").arg(d.label));
        d.a = work::VideoDecoder::open(clip, d.luid, &d.error);
        if (d.a)
            d.b = work::VideoDecoder::open(clip, d.luid, &d.error);
        if (!d.a || !d.b) {
            d.a.reset();
            continue;
        }
        const auto check = d.a->decodeAll(true);
        if (!check.ok) {
            d.error = check.error;
            d.a.reset();
            d.b.reset();
            continue;
        }
        d.gpuOutput = check.gpuOutput;
        if (!d.luid)
            referenceHashes = check.frameHashes;
        if (!referenceHashes.empty())
            d.identical = check.frameHashes == referenceHashes;
    }

    auto annotate = [&](BenchmarkResult& r, const std::vector<const Decoder*>& used) {
        r.correctness = QStringLiteral("Every frame identical to the software decoder's (checked before timing)");
        for (const Decoder* d : used) {
            if (d->luid && !d->gpuOutput)
                r.warnings << QStringLiteral("%1: frames did not arrive as textures on this GPU, so decoding may have fallen back to software.").arg(d->label);
            if (d->luid && d->gpuOutput)
                r.notes << QStringLiteral("%1: frames delivered as Direct3D 11 textures on this GPU (hardware decoding).").arg(d->label);
        }
    };
    auto setCorrect = [](RunSample& s, const std::vector<const Decoder*>& used) {
        s.correct = std::all_of(used.begin(), used.end(), [](const Decoder* d) { return d->identical.value_or(false); });
    };
    auto unavailable = [&](BenchmarkResult& r, const std::vector<const Decoder*>& used) {
        QStringList errors;
        for (const Decoder* d : used) {
            if (!d->a)
                errors << QStringLiteral("%1: %2").arg(d->label, d->error.isEmpty() ? QStringLiteral("not available") : d->error);
        }
        if (errors.isEmpty())
            return false;
        loop.skip(r, errors.join(QStringLiteral("; ")));
        return true;
    };
    // Decodes with each decoder instance on its own thread; wall time until all are done.
    auto decodeTogether = [&](const std::vector<work::VideoDecoder*>& instances, RunSample& s, QString* error) {
        std::vector<work::VideoDecoder::Result> results(instances.size());
        std::vector<std::thread> threads;
        const double t0 = nowMs();
        for (std::size_t i = 0; i < instances.size(); ++i)
            threads.emplace_back([&, i] {
                work::ComScope threadCom;
                results[i] = instances[i]->decodeAll(false);
            });
        for (std::thread& t : threads)
            t.join();
        s.durationMs = nowMs() - t0;
        int frames = 0;
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (!results[i].ok) {
                *error = results[i].error;
                return false;
            }
            frames += results[i].frames;
            s.metrics.insert(QStringLiteral("s%1.ms").arg(i), results[i].wallMs);
        }
        s.metrics.insert(QStringLiteral("fps"), frames / (s.durationMs / 1000.0));
        return true;
    };

    for (const Decoder& d : decoders) {
        BenchmarkResult r;
        r.testId = QStringLiteral("lab.decode.one");
        r.name = QStringLiteral("One stream — %1").arg(d.label);
        r.category = category;
        r.target = d.target;
        r.api = d.luid ? QStringLiteral("Media Foundation H.264 decoder, Direct3D 11 (DXVA) on this GPU") : QStringLiteral("Media Foundation H.264 decoder, software");
        r.params = params;
        r.params.insert(QStringLiteral("streams"), 1);
        r.measures = QStringLiteral("time to decode every frame of the stream");
        const std::vector<const Decoder*> used{ &d };
        if (!unavailable(r, used)) {
            annotate(r, used);
            loop.run(r, [&](RunSample& s, QString* e) {
                if (!decodeTogether({ d.a.get() }, s, e))
                    return false;
                setCorrect(s, used);
                return true;
            });
        }
        session.results << r;
    }
    for (const Decoder& d : decoders) {
        BenchmarkResult r;
        r.testId = QStringLiteral("lab.decode.two-same");
        r.name = QStringLiteral("Two streams — both on %1").arg(d.label);
        r.category = category;
        r.target = d.target;
        r.api = QStringLiteral("Two decoder instances at once on the same device");
        r.params = params;
        r.params.insert(QStringLiteral("streams"), 2);
        r.measures = QStringLiteral("time until both streams are completely decoded");
        const std::vector<const Decoder*> used{ &d };
        if (!unavailable(r, used)) {
            annotate(r, used);
            loop.run(r, [&](RunSample& s, QString* e) {
                if (!decodeTogether({ d.a.get(), d.b.get() }, s, e))
                    return false;
                setCorrect(s, used);
                return true;
            });
        }
        session.results << r;
    }
    if (twoSplit) {
        const Decoder& x = decoders[1];
        const Decoder& y = decoders[2];
        BenchmarkResult r;
        r.testId = QStringLiteral("lab.decode.two-split");
        r.name = QStringLiteral("Two streams — one on each GPU");
        r.category = category;
        r.target = combined({ x.target, y.target });
        r.api = QStringLiteral("One decoder per GPU, at the same time");
        r.params = params;
        r.params.insert(QStringLiteral("streams"), 2);
        r.measures = QStringLiteral("time until both streams are completely decoded");
        const std::vector<const Decoder*> used{ &x, &y };
        if (!unavailable(r, used)) {
            annotate(r, used);
            loop.run(r, [&](RunSample& s, QString* e) {
                if (!decodeTogether({ x.a.get(), y.a.get() }, s, e))
                    return false;
                setCorrect(s, used);
                return true;
            });
        }
        session.results << r;
    }
    loop.report(QStringLiteral("Done"));
    return session;
}

} // namespace gf::bench
