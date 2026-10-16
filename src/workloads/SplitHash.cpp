#include "workloads/SplitHash.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <mutex>
#include <thread>

namespace gf::work {

namespace {

using Clock = std::chrono::steady_clock;

double msBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

SplitResult runSplit(const HashParams& p, const std::vector<SplitWorker>& workers, const SplitSpec& spec)
{
    SplitResult result;
    result.workers.resize(workers.size());
    if (workers.empty() || p.width == 0 || p.height == 0) {
        result.error = QStringLiteral("Nothing to run.");
        return result;
    }
    result.image.assign(std::size_t(p.width) * p.height, 0u); // allocated before timing starts

    std::vector<RowRange> ranges;
    if (spec.mode == SplitSpec::Mode::Static) {
        if (spec.shares.size() != workers.size()) {
            result.error = QStringLiteral("One share per worker is required.");
            return result;
        }
        ranges = staticRanges(p.height, spec.shares, spec.align);
    }

    // One thread per GPU; a CPU worker gets one thread per CPU thread in dynamic mode (each takes
    // items on its own) and a single coordinating thread in static mode (its range is split inside).
    struct Task { std::size_t worker; unsigned cpuThreads; };
    std::vector<Task> tasks;
    for (std::size_t w = 0; w < workers.size(); ++w) {
        if (workers[w].gpu || spec.mode == SplitSpec::Mode::Static)
            tasks.push_back({ w, std::max(1u, workers[w].cpuThreads) });
        else
            for (unsigned t = 0; t < std::max(1u, workers[w].cpuThreads); ++t)
                tasks.push_back({ w, 1 });
    }

    std::latch ready(std::ptrdiff_t(tasks.size()));
    std::latch go(1);
    Clock::time_point start;
    std::atomic<std::uint32_t> next{ 0 };
    std::atomic<bool> failed{ false };
    std::mutex mutex; // guards reports and error
    QString error;

    auto body = [&](const Task& task) {
        const SplitWorker& worker = workers[task.worker];
        WorkerReport local;
        QString localError;
        auto doItem = [&](std::uint32_t first, std::uint32_t rows) {
            std::uint32_t* dest = result.image.data() + std::size_t(first) * p.width;
            if (worker.gpu) {
                double c = 0, r = 0;
                localError = worker.gpu->computeInto(p, first, rows, dest, &c, &r);
                local.computeMs += c;
                local.readbackMs += r;
            } else {
                const auto t = Clock::now();
                hashRowsCpuInto(p, first, rows, task.cpuThreads, dest);
                local.computeMs += msBetween(t, Clock::now());
            }
            local.rows += rows;
            ++local.items;
            return localError.isEmpty();
        };
        ready.count_down();
        go.wait();
        if (spec.mode == SplitSpec::Mode::Static) {
            const RowRange range = ranges[task.worker];
            if (range.rows)
                doItem(range.first, range.rows);
        } else {
            const std::uint32_t chunk = std::max(1u, spec.chunkRows);
            while (!failed.load()) {
                const std::uint32_t first = next.fetch_add(chunk);
                if (first >= p.height)
                    break;
                if (!doItem(first, std::min(chunk, p.height - first))) {
                    failed = true;
                    break;
                }
            }
        }
        local.busyMs = msBetween(start, Clock::now());
        std::lock_guard lock(mutex);
        WorkerReport& report = result.workers[task.worker];
        report.rows += local.rows;
        report.items += local.items;
        report.computeMs += local.computeMs;
        report.readbackMs += local.readbackMs;
        report.busyMs = std::max(report.busyMs, local.busyMs);
        if (!localError.isEmpty() && error.isEmpty())
            error = QStringLiteral("%1: %2").arg(worker.label, localError);
    };

    std::vector<std::thread> threads;
    threads.reserve(tasks.size());
    for (const Task& t : tasks)
        threads.emplace_back(body, t);
    ready.wait();
    start = Clock::now(); // published to the workers by `go`
    go.count_down();
    for (std::thread& t : threads)
        t.join();
    result.wallMs = msBetween(start, Clock::now());

    std::uint32_t covered = 0;
    for (const WorkerReport& r : result.workers)
        covered += r.rows;
    if (!error.isEmpty())
        result.error = error;
    else if (covered != p.height)
        result.error = QStringLiteral("Only %1 of %2 rows were computed.").arg(covered).arg(p.height);
    result.ok = result.error.isEmpty();
    return result;
}

} // namespace gf::work
