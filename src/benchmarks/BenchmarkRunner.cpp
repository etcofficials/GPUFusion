#include "benchmarks/BenchmarkRunner.h"

#include "core/Logging.h"
#include "core/Version.h"
#include "hardware/AdapterDetector.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/KmtStatsProvider.h"
#include "platform/windows/NvmlProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/ProcessList.h"
#include "preferences/PreferenceModel.h"
#include "workloads/D3D11HashDevice.h"

#include <QElapsedTimer>
#include <QSysInfo>
#include <QThread>

#include <chrono>
#include <map>

#include <windows.h>
#include <psapi.h>

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

double peakWorkingSetMB()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? double(pmc.PeakWorkingSetSize) / (1 << 20) : 0;
}

const hw::Adapter* adapterByLuid(const hw::DetectionResult& d, quint64 luid)
{
    for (const hw::Adapter& a : d.adapters) {
        if (a.dxgi && a.dxgi->luid == luid)
            return &a;
    }
    return nullptr;
}

// One measured iteration of a benchmark; returns false (with error) if it cannot run.
using Iteration = std::function<bool(RunSample& sample, QString* error)>;

// Shared state across the benchmarks of one session.
struct Context {
    const RunOptions& options;
    const hw::DetectionResult& detection;
    std::map<quint64, std::unique_ptr<work::D3D11HashDevice>> devices;
    std::optional<std::vector<std::uint32_t>> reference; // CPU reference image for options.hash

    const std::vector<std::uint32_t>& cpuReference()
    {
        if (!reference)
            reference = work::hashRowsCpu(options.hash, 0, options.hash.height, 0);
        return *reference;
    }
};

} // namespace

QJsonObject hashParamsJson(const work::HashParams& p)
{
    return QJsonObject{ { "width", qint64(p.width) }, { "height", qint64(p.height) }, { "iterations", qint64(p.iterations) },
                        { "seed", qint64(p.seed) }, { "bandRows", qint64(p.bandRows) } };
}

Target gpuTarget(const hw::Adapter& a)
{
    const hw::DxgiAdapter& d = *a.dxgi;
    return Target{ a.name(), d.vendorId, d.deviceId, d.subSysId, a.pnp ? a.pnp->driverVersion : d.umdVersion };
}

Session beginSession(const hw::DetectionResult& detection)
{
    Session session;
    session.started = QDateTime::currentDateTime();
    session.id = session.started.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")); // unique even for back-to-back sessions
    session.appVersion = QString::fromLatin1(version::kString);
    session.os = QStringLiteral("%1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    session.cpu = cpuName();
    session.ramGB = installedRamGB();
    for (const hw::Adapter& a : detection.adapters) {
        if (a.dxgi && !a.isSoftware())
            session.adapters << QStringLiteral("%1 (driver %2)").arg(a.name(), a.pnp ? a.pnp->driverVersion : a.dxgi->umdVersion);
    }
    return session;
}

work::HashParams hashPreset(const QString& name)
{
    work::HashParams p;
    if (name == QLatin1StringView("quick")) {
        p.width = p.height = 512;
        p.iterations = 256;
    } else if (name == QLatin1StringView("heavy")) {
        p.width = p.height = 2048;
        p.iterations = 512;
    } else { // standard
        p.width = p.height = 1024;
        p.iterations = 512;
    }
    return p;
}

QString cpuName()
{
    HKEY key = nullptr;
    QString name;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buffer[256] = {};
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS)
            name = QString::fromWCharArray(buffer).simplified();
        RegCloseKey(key);
    }
    return name.isEmpty() ? QSysInfo::currentCpuArchitecture() : name;
}

double installedRamGB()
{
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) ? double(m.ullTotalPhys) / double(1ull << 30) : 0;
}

QList<BenchmarkSpec> availableBenchmarks(const hw::DetectionResult& detection, const QString& probePath)
{
    const Target cpu{ QStringLiteral("CPU: %1").arg(cpuName()), 0, 0, 0, {} };
    const Target host{ QStringLiteral("This PC"), 0, 0, 0, {} };
    QList<BenchmarkSpec> specs;
    specs << BenchmarkSpec{ QStringLiteral("detect.full"), QStringLiteral("Adapter detection (full probe)"), QStringLiteral("Detection"),
                            QStringLiteral("Lists adapters and creates Direct3D 11/12 test devices on each."), 0, host };
    specs << BenchmarkSpec{ QStringLiteral("detect.reused"), QStringLiteral("Adapter re-detection (probes reused)"), QStringLiteral("Detection"),
                            QStringLiteral("What a device-change re-detection costs."), 0, host };
    specs << BenchmarkSpec{ QStringLiteral("monitor.sample"), QStringLiteral("Monitoring: one adapter sample"), QStringLiteral("Monitoring overhead"),
                            QStringLiteral("D3DKMT + NVML readings for every GPU, as taken once per refresh interval."), 0, host };
    specs << BenchmarkSpec{ QStringLiteral("monitor.processes"), QStringLiteral("Monitoring: one per-process pass"), QStringLiteral("Monitoring overhead"),
                            QStringLiteral("Process list + per-process GPU counters, as taken while the Applications page is open."), 0, host };
    specs << BenchmarkSpec{ QStringLiteral("cpu.hash.1t"), QStringLiteral("Hash workload — CPU, 1 thread"), QStringLiteral("Compute"),
                            QStringLiteral("The reference workload on one CPU core."), 0, cpu };
    specs << BenchmarkSpec{ QStringLiteral("cpu.hash.mt"), QStringLiteral("Hash workload — CPU, all threads"), QStringLiteral("Compute"),
                            QStringLiteral("The reference workload on all CPU cores."), 0, cpu };
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi || a.isSoftware() || !a.dxgi->d3d11.device.ok())
            continue;
        const hw::DxgiAdapter& d = *a.dxgi;
        const Target t = gpuTarget(a);
        specs << BenchmarkSpec{ QStringLiteral("gpu.hash"), QStringLiteral("Hash workload — %1").arg(a.name()), QStringLiteral("Compute"),
                                QStringLiteral("The reference workload as a pixel shader, checked bit-exactly against the CPU."), d.luid, t };
        specs << BenchmarkSpec{ QStringLiteral("gpu.transfer"), QStringLiteral("CPU ↔ GPU transfer — %1").arg(a.name()), QStringLiteral("Data transfer"),
                                QStringLiteral("Upload 32 MB to GPU memory and read it back; verifies the data."), d.luid, t };
    }
    if (!probePath.isEmpty())
        specs << BenchmarkSpec{ QStringLiteral("probe.launch"), QStringLiteral("Application launch (probe)"), QStringLiteral("Application"),
                                QStringLiteral("Start GPUFusionProbe.exe, create a Direct3D device, report, exit."), 0, host };
    return specs;
}

Session runBenchmarks(const QList<BenchmarkSpec>& specs, const RunOptions& options, const hw::DetectionResult& detection,
                      const Progress& progress, const std::atomic<bool>* cancel)
{
    Session session = beginSession(detection);

    Context ctx{ options, detection, {}, {} };
    const int perTest = options.warmupRuns + options.measuredRuns;
    const int total = int(specs.size()) * perTest;
    int done = 0;
    const QList<mon::MonitorTarget> targets = mon::targetsFrom(detection);
    QList<mon::MonitorTarget> gpuTargets;
    for (const mon::MonitorTarget& t : targets) {
        if (!t.software)
            gpuTargets << t;
    }

    for (const BenchmarkSpec& spec : specs) {
        BenchmarkResult r;
        r.testId = spec.id;
        r.name = spec.name;
        r.category = spec.category;
        r.target = spec.target;
        r.warmupRuns = options.warmupRuns;
        r.measuredRuns = options.measuredRuns;
        r.started = QDateTime::currentDateTime();
        const double testStart = nowMs();
        r.cpuPercent = r.gpuBusyPercent = r.gpuDedicatedMB = mon::MetricValue::unsupported(QStringLiteral("Not measured for this test"));
        r.peakWorkingSetMB = mon::MetricValue::unknown({});
        r.correctness = QStringLiteral("Not checked");

        if (cancel && cancel->load()) {
            r.status = Status::Cancelled;
            session.results << r;
            continue;
        }

        // ---- build the iteration for this test ----
        Iteration iteration;
        QString setupError;
        if (spec.id == QLatin1StringView("detect.full") || spec.id == QLatin1StringView("detect.reused")) {
            const bool reuse = spec.id == QLatin1StringView("detect.reused");
            r.api = QStringLiteral("DXGI, Direct3D 11/12, D3DKMT, SetupAPI");
            r.measures = reuse ? QStringLiteral("time for one re-detection with probes reused") : QStringLiteral("time for one full detection");
            r.correctness = QStringLiteral("Adapter count matches the app's detection");
            iteration = [&, reuse](RunSample& s, QString*) {
                const double t = nowMs();
                const hw::DetectionResult d = hw::detectAdapters(reuse ? &detection : nullptr);
                s.durationMs = nowMs() - t;
                s.correct = d.hardwareCount() == detection.hardwareCount();
                s.metrics.insert(QStringLiteral("adapters"), d.hardwareCount());
                return true;
            };
        } else if (spec.id == QLatin1StringView("monitor.sample")) {
            r.api = QStringLiteral("D3DKMT + NVML");
            r.measures = QStringLiteral("average time for one adapter-level sample (over 20)");
            auto kmt = std::make_shared<mon::win::KmtStatsProvider>();
            auto nvml = std::make_shared<mon::win::NvmlProvider>();
            kmt->setTargets(gpuTargets);
            nvml->setTargets(gpuTargets);
            iteration = [kmt, nvml, gpuTargets](RunSample& s, QString*) {
                const double t = nowMs();
                for (int k = 0; k < 20; ++k) {
                    QList<mon::AdapterSample> samples;
                    for (const mon::MonitorTarget& g : gpuTargets)
                        samples << mon::AdapterSample{ g.luid, g.name };
                    kmt->sample(samples);
                    if (nvml->isActive())
                        nvml->sample(samples);
                }
                s.durationMs = (nowMs() - t) / 20.0;
                return true;
            };
        } else if (spec.id == QLatin1StringView("monitor.processes")) {
            r.api = QStringLiteral("Toolhelp + PDH + D3DKMT");
            r.measures = QStringLiteral("time for one per-process pass");
            auto list = std::make_shared<mon::win::ProcessList>();
            auto pdh = std::make_shared<mon::win::PdhGpuProcessProvider>();
            auto kmtp = std::make_shared<mon::win::KmtProcessProvider>();
            QList<mon::win::KmtProcessProvider::Adapter> legacy;
            QList<quint64> pdhLuids;
            for (const mon::MonitorTarget& t : targets) {
                if (t.wddmVersion < 2000)
                    legacy << mon::win::KmtProcessProvider::Adapter{ t.luid, t.name };
                else
                    pdhLuids << t.luid;
            }
            kmtp->setAdapters(legacy);
            iteration = [list, pdh, kmtp, pdhLuids](RunSample& s, QString*) {
                const double t = nowMs();
                const auto processes = list->snapshot();
                QList<mon::PdhEngineReading> e;
                QList<mon::PdhMemoryReading> m;
                QHash<quint32, QList<mon::ProcessAdapterUsage>> usage;
                if (pdh->collect(&e, &m))
                    usage = mon::aggregatePdh(e, m, pdhLuids);
                int a = 0, b = 0;
                kmtp->sample(processes, usage, &a, &b);
                s.durationMs = nowMs() - t;
                s.metrics.insert(QStringLiteral("processes"), processes.size());
                return true;
            };
        } else if (spec.id.startsWith(QLatin1StringView("cpu.hash"))) {
            const unsigned threads = spec.id.endsWith(QLatin1StringView("1t")) ? 1u : 0u;
            const unsigned used = threads ? 1u : std::max(1u, std::thread::hardware_concurrency());
            r.api = QStringLiteral("CPU, %1 thread%2").arg(used).arg(used == 1 ? QString() : QStringLiteral("s"));
            r.params = hashParamsJson(options.hash);
            r.params.insert(QStringLiteral("threads"), qint64(used));
            r.measures = QStringLiteral("time to compute the whole image");
            r.correctness = threads ? QStringLiteral("Bit-exact match with the multi-threaded CPU result") : QStringLiteral("Reference result (others are checked against it)");
            iteration = [&ctx, threads](RunSample& s, QString*) {
                const double t = nowMs();
                const auto out = work::hashRowsCpu(ctx.options.hash, 0, ctx.options.hash.height, threads);
                s.durationMs = nowMs() - t;
                s.metrics.insert(QStringLiteral("gigaStepsPerSecond"),
                                 double(ctx.options.hash.width) * ctx.options.hash.height * ctx.options.hash.iterations / (s.durationMs * 1e6));
                s.correct = threads == 0 || work::firstMismatch(out, ctx.cpuReference()) < 0;
                return true;
            };
        } else if (spec.id == QLatin1StringView("gpu.hash") || spec.id == QLatin1StringView("gpu.transfer")) {
            auto it = ctx.devices.find(spec.luid);
            if (it == ctx.devices.end())
                it = ctx.devices.emplace(spec.luid, work::D3D11HashDevice::create(spec.luid, &setupError)).first;
            work::D3D11HashDevice* device = it->second.get();
            if (device) {
                r.api = QStringLiteral("Direct3D 11 (FL %1), pixel shader SM 4.0").arg(device->featureLevel());
                if (spec.id == QLatin1StringView("gpu.hash")) {
                    r.params = hashParamsJson(options.hash);
                    r.measures = QStringLiteral("GPU time to compute the whole image (submit to finish, excluding read-back)");
                    r.correctness = QStringLiteral("Bit-exact match with the CPU reference (%1 values)").arg(qulonglong(options.hash.width) * options.hash.height);
                    ctx.cpuReference(); // computed before timing starts
                    iteration = [&ctx, device](RunSample& s, QString* error) {
                        const auto res = device->run(ctx.options.hash, 0, ctx.options.hash.height, true);
                        if (!res.ok) {
                            *error = res.error;
                            return false;
                        }
                        s.durationMs = res.wallMs;
                        if (res.gpuMs)
                            s.metrics.insert(QStringLiteral("gpuTimestampMs"), *res.gpuMs);
                        s.metrics.insert(QStringLiteral("readbackMs"), res.readbackMs);
                        s.metrics.insert(QStringLiteral("gigaStepsPerSecond"),
                                         double(ctx.options.hash.width) * ctx.options.hash.height * ctx.options.hash.iterations / (res.wallMs * 1e6));
                        const long long bad = work::firstMismatch(res.output, ctx.cpuReference());
                        s.correct = bad < 0;
                        if (!s.correct)
                            s.note = QStringLiteral("first mismatch at value %1").arg(bad);
                        return true;
                    };
                } else {
                    r.params = QJsonObject{ { "bytes", qint64(options.transferBytes) } };
                    r.measures = QStringLiteral("upload + read-back of the buffer");
                    r.correctness = QStringLiteral("Data read back equals data sent");
                    iteration = [&ctx, device](RunSample& s, QString* error) {
                        const auto res = device->transfer(ctx.options.transferBytes);
                        if (!res.ok) {
                            *error = res.error;
                            return false;
                        }
                        s.durationMs = res.uploadMs + res.readbackMs;
                        const double mb = double(res.bytes) / (1 << 20);
                        s.metrics.insert(QStringLiteral("uploadMBps"), mb / (res.uploadMs / 1000.0));
                        s.metrics.insert(QStringLiteral("readbackMBps"), mb / (res.readbackMs / 1000.0));
                        s.correct = res.dataIntact;
                        return true;
                    };
                }
            }
        } else if (spec.id == QLatin1StringView("probe.launch")) {
            r.api = QStringLiteral("CreateProcess + DXGI/D3D11 in the probe");
            r.measures = QStringLiteral("time from start to the probe's report");
            r.correctness = QStringLiteral("The probe reported a Direct3D device");
            const QString probe = options.probePath;
            iteration = [probe](RunSample& s, QString* error) {
                const double t = nowMs();
                const auto out = pref::win::runProbe(probe, error);
                s.durationMs = nowMs() - t;
                if (!out)
                    return false;
                const auto report = pref::parseProbeReport(*out, error);
                s.correct = report && report->d3d11Default.has_value();
                return true;
            };
        } else {
            setupError = QStringLiteral("Unknown benchmark.");
        }

        if (!iteration) {
            r.status = Status::Failed;
            r.error = setupError.isEmpty() ? QStringLiteral("Could not set up the benchmark.") : setupError;
            done += perTest;
            if (progress)
                progress(done, total, spec.name);
            session.results << r;
            continue;
        }

        // ---- run: warm-up, then measured runs with CPU/GPU accounting ----
        mon::win::KmtStatsProvider kmt;
        QList<mon::AdapterSample> gpuSample;
        if (spec.luid) {
            for (const mon::MonitorTarget& t : gpuTargets) {
                if (t.luid == spec.luid) {
                    kmt.setTargets({ t });
                    gpuSample << mon::AdapterSample{ t.luid, t.name };
                }
            }
        }
        bool failed = false;
        quint64 cpu0 = 0;
        double wall0 = 0;
        const double warm0 = nowMs();
        for (int k = 0; k < perTest && !failed; ++k) {
            if (cancel && cancel->load())
                break;
            const bool measured = k >= options.warmupRuns;
            if (k == options.warmupRuns && spec.luid) {
                // GPU tests: keep warming up for at least 1 s so an idle GPU has left its low-power
                // clock state (the GT 730 was measured in P8 after idle periods; see the Workload Lab).
                for (int extra = 0; nowMs() - warm0 < 1000 && extra < 200 && !(cancel && cancel->load()); ++extra) {
                    RunSample ignored;
                    QString error;
                    if (!iteration(ignored, &error)) {
                        r.status = Status::Failed;
                        r.error = error;
                        failed = true;
                        break;
                    }
                    ++r.warmupRuns;
                }
                if (failed || (cancel && cancel->load()))
                    break;
            }
            if (k == options.warmupRuns) { // start accounting at the first measured run
                cpu0 = processCpu100ns();
                wall0 = nowMs();
                if (!gpuSample.isEmpty()) {
                    QList<mon::AdapterSample> s = gpuSample;
                    kmt.sample(s);
                }
            }
            if (progress)
                progress(done, total, QStringLiteral("%1 — %2 %3 of %4").arg(spec.name, measured ? QStringLiteral("run") : QStringLiteral("warm-up"))
                                          .arg(measured ? k - options.warmupRuns + 1 : k + 1).arg(measured ? options.measuredRuns : options.warmupRuns));
            RunSample sample;
            QString error;
            if (!iteration(sample, &error)) {
                r.status = Status::Failed;
                r.error = error;
                failed = true;
            } else if (measured) {
                r.samples << sample;
            }
            ++done;
        }
        const double wall = nowMs() - wall0;
        if (!failed && r.samples.size() == options.measuredRuns && wall > 0) {
            r.cpuPercent = mon::MetricValue::ok(100.0 * double(processCpu100ns() - cpu0) / 1e4 / wall / std::max(1, QThread::idealThreadCount()),
                                                QStringLiteral("GPU Fusion's CPU time during the measured runs, share of all cores"));
            if (!gpuSample.isEmpty()) {
                QList<mon::AdapterSample> s = gpuSample;
                kmt.sample(s);
                r.gpuBusyPercent = s.first().utilization;
                r.gpuDedicatedMB = s.first().dedicatedUsed.isOk() ? mon::MetricValue::ok(s.first().dedicatedUsed.value / (1 << 20))
                                                                  : s.first().dedicatedUsed;
            }
        }
        r.peakWorkingSetMB = mon::MetricValue::ok(peakWorkingSetMB(), QStringLiteral("GPU Fusion's peak working set so far"));
        if (!failed && r.samples.size() < options.measuredRuns)
            r.status = Status::Cancelled;
        summarize(r);
        if (r.correct.has_value() && !*r.correct)
            r.warnings << QStringLiteral("Output did not match the reference — the timings of this result are not trustworthy.");
        if (r.duration.n >= 3 && r.duration.cv() > 0.10)
            r.warnings << QStringLiteral("Run-to-run variation is high (%1 %); other activity on the PC may have interfered.").arg(r.duration.cv() * 100, 0, 'f', 0);
        r.totalMs = nowMs() - testStart;
        qCInfo(lcBench).noquote() << QStringLiteral("%1 [%2]: %3, mean %4 ms ± %5, correct: %6").arg(r.name, r.target.name, statusName(r.status))
                                         .arg(r.duration.mean, 0, 'f', 3).arg(r.duration.stddev, 0, 'f', 3)
                                         .arg(r.correct ? (*r.correct ? QStringLiteral("yes") : QStringLiteral("NO")) : QStringLiteral("n/a"));
        session.results << r;
    }
    if (progress)
        progress(total, total, QStringLiteral("Done"));
    return session;
}

} // namespace gf::bench
