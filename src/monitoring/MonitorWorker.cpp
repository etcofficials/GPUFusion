#include "monitoring/MonitorWorker.h"

#include "core/Logging.h"
#include "platform/windows/KmtStatsProvider.h"
#include "platform/windows/NvmlProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"

#include <QThread>
#include <QTimer>

#include <windows.h>

namespace gf::mon {

namespace {

quint64 processCpu100ns()
{
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0;
    const auto toU64 = [](const FILETIME& f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return toU64(kernel) + toU64(user);
}

constexpr qint64 kCpuWindowMs = 10'000;
constexpr qint64 kStatsLogMs = 60'000;

} // namespace

MonitorWorker::MonitorWorker() = default;
MonitorWorker::~MonitorWorker() = default;

void MonitorWorker::start(int intervalMs)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::CoarseTimer);
        connect(m_timer, &QTimer::timeout, this, &MonitorWorker::collect);
        m_clock.start();
    }
    if (!m_kmt) {
        m_kmt = std::make_unique<win::KmtStatsProvider>();
        m_kmt->setTargets(m_gpuTargets);
    }
    if (m_nvmlEnabled && !m_nvml) {
        m_nvml = std::make_unique<win::NvmlProvider>();
        m_nvml->setTargets(m_gpuTargets);
    }
    m_timer->setInterval(intervalMs);
    m_timer->start();
    m_cpuWindowStartCpu100ns = processCpu100ns();
    m_cpuWindowStartMs = m_statWindowStartMs = m_clock.elapsed();
    qCInfo(lcMonitor) << "Monitoring started, interval" << intervalMs << "ms";
    collect(); // first sample right away (rates become known on the second)
}

void MonitorWorker::stop()
{
    if (m_timer && m_timer->isActive()) {
        m_timer->stop();
        // Drop utilization baselines so the first reading after resuming is not an average over the pause.
        m_kmt.reset();
        qCInfo(lcMonitor) << "Monitoring paused";
    }
}

void MonitorWorker::setInterval(int intervalMs)
{
    if (m_timer)
        m_timer->setInterval(intervalMs);
}

void MonitorWorker::setTargets(const QList<MonitorTarget>& targets)
{
    m_targets = targets;
    m_gpuTargets.clear();
    QList<win::KmtProcessProvider::Adapter> legacy;
    for (const MonitorTarget& t : targets) {
        if (!t.software)
            m_gpuTargets << t;
        // Per-process data: WDDM 2.0+ adapters (including WARP) come from performance counters;
        // older drivers (or unknown) need D3DKMT.
        if (t.wddmVersion < 2000)
            legacy << win::KmtProcessProvider::Adapter{ t.luid, t.name };
    }
    m_kmtProcesses.setAdapters(legacy);
    if (m_kmt)
        m_kmt->setTargets(m_gpuTargets);
    if (m_nvml)
        m_nvml->setTargets(m_gpuTargets);
    publishProviders();
}

void MonitorWorker::setProcessTracking(bool enabled)
{
    if (enabled == m_processTracking)
        return;
    m_processTracking = enabled;
    if (enabled) {
        m_lastProcessSampleMs = -1;
        collectProcesses(); // prime counters so the next sample has rates
    } else {
        m_pdhProcesses.reset(); // closes the PDH query; re-opened on demand
    }
}

void MonitorWorker::sampleProcessesNow()
{
    collectProcesses();
}

void MonitorWorker::collectProcesses()
{
    QElapsedTimer cost;
    cost.start();
    ProcessSnapshot snap;
    const QList<win::ProcessInfo> processes = m_processList.snapshot();

    QList<quint64> pdhLuids;
    for (const MonitorTarget& t : m_targets) {
        if (t.wddmVersion >= 2000) {
            pdhLuids << t.luid;
            snap.pdhAdapterNames << t.name;
        } else {
            snap.legacyAdapterNames << t.name;
        }
    }

    // Rates are valid only if every source used here has a previous sample.
    bool ratesReady = !m_targets.isEmpty();
    if (!snap.legacyAdapterNames.isEmpty())
        ratesReady = ratesReady && m_kmtProcesses.hasSampled();
    if (!pdhLuids.isEmpty())
        ratesReady = ratesReady && m_pdhProcesses && m_pdhProcesses->isPrimed();
    snap.ratesReady = ratesReady;

    QHash<quint32, QList<ProcessAdapterUsage>> usage;
    if (!pdhLuids.isEmpty()) {
        if (!m_pdhProcesses)
            m_pdhProcesses = std::make_unique<win::PdhGpuProcessProvider>();
        QList<PdhEngineReading> engines;
        QList<PdhMemoryReading> memory;
        if (m_pdhProcesses->collect(&engines, &memory))
            usage = aggregatePdh(engines, memory, pdhLuids);
        snap.pdhError = m_pdhProcesses->error();
    }
    m_kmtProcesses.sample(processes, usage, &snap.legacyInspectable, &snap.legacyNotInspectable);

    snap.processCount = int(processes.size());
    for (const win::ProcessInfo& p : processes) {
        ProcessSample s;
        s.pid = p.pid;
        s.name = p.name;
        s.path = p.path;
        s.pathError = p.pathError;
        s.adapters = usage.value(p.pid);
        snap.processes << s;
    }
    snap.wallTime = QDateTime::currentDateTime();
    snap.steadyMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    snap.collectionMs = double(cost.nsecsElapsed()) / 1e6;
    m_lastProcessSampleMs = snap.steadyMs;
    Q_EMIT processSnapshotReady(snap);
}

void MonitorWorker::setNvmlEnabled(bool enabled)
{
    if (enabled == m_nvmlEnabled)
        return;
    m_nvmlEnabled = enabled;
    if (!enabled) {
        m_nvml.reset();
    } else if (m_timer) {
        m_nvml = std::make_unique<win::NvmlProvider>();
        m_nvml->setTargets(m_gpuTargets);
    }
    publishProviders();
}

void MonitorWorker::shutdown()
{
    if (m_timer)
        m_timer->stop();
    m_nvml.reset();
    m_kmt.reset();
    m_pdhProcesses.reset();
}

void MonitorWorker::publishProviders()
{
    QList<ProviderStatus> list;
    if (m_kmt)
        list << m_kmt->status();
    if (m_nvml)
        list << m_nvml->status();
    else
        list << ProviderStatus{ QStringLiteral("NVIDIA NVML"), false,
                                m_nvmlEnabled ? QStringLiteral("Not started yet.") : QStringLiteral("Disabled in Settings."), {} };
    Q_EMIT providersChanged(list);
}

void MonitorWorker::updateCpuWindow()
{
    const qint64 now = m_clock.elapsed();
    if (now - m_cpuWindowStartMs < kCpuWindowMs)
        return;
    const quint64 cpu = processCpu100ns();
    const double cpuMs = double(cpu - m_cpuWindowStartCpu100ns) / 1e4;
    // Share of the whole machine (all logical CPUs), the same scale as Task Manager's CPU column.
    const double percentOfMachine = 100.0 * cpuMs / double(now - m_cpuWindowStartMs) / double(QThread::idealThreadCount());
    m_processCpu = MetricValue::ok(percentOfMachine, QStringLiteral("GetProcessTimes over the last 10 s, all GPU Fusion threads, share of all CPU cores"));
    m_cpuWindowStartCpu100ns = cpu;
    m_cpuWindowStartMs = now;
}

void MonitorWorker::collect()
{
    QElapsedTimer cost;
    cost.start();

    QList<AdapterSample> samples;
    samples.reserve(m_gpuTargets.size());
    for (const MonitorTarget& t : m_gpuTargets) {
        AdapterSample s;
        s.luid = t.luid;
        s.name = t.name;
        s.dedicatedTotal = t.dedicatedTotal;
        s.sharedLimit = t.sharedLimit;
        // Sensors default to an explicit state; providers overwrite what they can read.
        MetricValue sensor;
        if (t.vendorId != 0x10DE)
            sensor = MetricValue::unsupported(QStringLiteral("No sensor interface is available for this adapter"));
        else if (!m_nvmlEnabled)
            sensor = MetricValue::unavailable(QStringLiteral("NVML is disabled in Settings"));
        else
            sensor = MetricValue::unavailable(QStringLiteral("NVML is not available: %1").arg(m_nvml ? m_nvml->status().detail : QString()));
        s.temperature = s.fanPercent = s.powerWatts = s.coreClockMHz = s.memoryClockMHz = s.pstate = sensor;
        samples << s;
    }

    bool lost = false;
    if (m_kmt)
        lost = !m_kmt->sample(samples).isEmpty();
    if (m_nvml && m_nvml->isActive())
        m_nvml->sample(samples);

    updateCpuWindow();

    Snapshot snap;
    snap.sequence = ++m_sequence;
    snap.wallTime = QDateTime::currentDateTime();
    snap.steadyMs = m_clock.elapsed();
    snap.adapters = std::move(samples);
    snap.collectionMs = double(cost.nsecsElapsed()) / 1e6;
    snap.processCpuPercent = m_processCpu;

    // Overhead statistics, logged once a minute.
    ++m_statSamples;
    m_statTotalMs += snap.collectionMs;
    m_statMaxMs = std::max(m_statMaxMs, snap.collectionMs);
    if (snap.steadyMs - m_statWindowStartMs >= kStatsLogMs) {
        qCInfo(lcMonitor).noquote() << QStringLiteral("Overhead: %1 samples in %2 s, collection avg %3 ms / max %4 ms; process CPU %5")
                                           .arg(m_statSamples).arg((snap.steadyMs - m_statWindowStartMs) / 1000)
                                           .arg(m_statTotalMs / m_statSamples, 0, 'f', 2).arg(m_statMaxMs, 0, 'f', 2)
                                           .arg(formatMetric(m_processCpu, Unit::Percent));
        m_statSamples = 0;
        m_statTotalMs = m_statMaxMs = 0;
        m_statWindowStartMs = snap.steadyMs;
    }

    Q_EMIT snapshotReady(snap);
    // Per-process sampling (only while someone is looking): at most every 2 s, since a full pass
    // opens every process and reads the per-process counters.
    if (m_processTracking && (m_lastProcessSampleMs < 0 || snap.steadyMs - m_lastProcessSampleMs >= std::max(2000, m_timer->interval()) - 50))
        collectProcesses();
    // Provider status changes after the first samples (NVML learns which metrics are unsupported).
    if (lost || snap.sequence <= 2)
        publishProviders();
    if (lost)
        Q_EMIT adaptersLost();
}

} // namespace gf::mon
