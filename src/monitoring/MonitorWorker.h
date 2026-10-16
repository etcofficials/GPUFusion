#pragma once

#include "monitoring/MetricModel.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/ProcessList.h"

#include <QElapsedTimer>
#include <QObject>

#include <memory>

class QTimer;

namespace gf::mon {

namespace win { class KmtStatsProvider; class NvmlProvider; class PdhGpuProcessProvider; }

// Lives on the monitor thread. Owns the providers (created, used and destroyed on that thread)
// and a timer; each tick produces one Snapshot. Collection never overlaps: a tick that arrives
// while a collection is still running is simply skipped by the timer.
class MonitorWorker : public QObject {
    Q_OBJECT
public:
    MonitorWorker();
    ~MonitorWorker() override;

public Q_SLOTS:
    void start(int intervalMs);
    void stop();
    void setInterval(int intervalMs);
    void setTargets(const QList<gf::mon::MonitorTarget>& targets);
    void setNvmlEnabled(bool enabled);
    void setProcessTracking(bool enabled); // per-process sampling, only while the Applications page is open
    void sampleProcessesNow();
    void shutdown(); // stops the timer and releases providers (called before the thread quits)

Q_SIGNALS:
    void snapshotReady(const gf::mon::Snapshot& snapshot);
    void processSnapshotReady(const gf::mon::ProcessSnapshot& snapshot);
    void providersChanged(const QList<gf::mon::ProviderStatus>& providers);
    void adaptersLost(); // a provider could not query an adapter any more

private:
    void collect();
    void collectProcesses();
    void publishProviders();
    void updateCpuWindow();

    QTimer* m_timer = nullptr;
    std::unique_ptr<win::KmtStatsProvider> m_kmt;
    std::unique_ptr<win::NvmlProvider> m_nvml;
    bool m_nvmlEnabled = true;
    QList<MonitorTarget> m_targets;     // all DXGI adapters, software ones flagged
    QList<MonitorTarget> m_gpuTargets;  // physical GPUs (adapter-level monitoring)

    // Per-process
    bool m_processTracking = false;
    qint64 m_lastProcessSampleMs = -1;
    win::ProcessList m_processList;
    std::unique_ptr<win::PdhGpuProcessProvider> m_pdhProcesses;
    win::KmtProcessProvider m_kmtProcesses;
    quint64 m_sequence = 0;
    QElapsedTimer m_clock;

    // Overhead accounting
    quint64 m_cpuWindowStartCpu100ns = 0;
    qint64 m_cpuWindowStartMs = 0;
    MetricValue m_processCpu = MetricValue::unknown(QStringLiteral("Measured over 10 s windows"));
    int m_statSamples = 0;
    double m_statTotalMs = 0, m_statMaxMs = 0;
    qint64 m_statWindowStartMs = 0;
};

} // namespace gf::mon
