#pragma once

#include "monitoring/History.h"
#include "monitoring/MetricModel.h"
#include "monitoring/ProcessModel.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <optional>

class QThread;

namespace gf { struct Settings; }
namespace gf::hw { class HardwareService; struct DetectionResult; }

namespace gf::mon {

class MonitorWorker;

// UI-thread facade over the monitor thread. Receives snapshots, keeps the bounded history and
// tracks freshness. Adapters to monitor come from hardware detection; when a provider loses an
// adapter, hardware detection is asked to run again.
class MonitorService : public QObject {
    Q_OBJECT
public:
    MonitorService(hw::HardwareService& hardware, const Settings& settings, QObject* parent = nullptr);
    ~MonitorService() override;

    void applySettings(const Settings& settings);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    int intervalMs() const { return m_intervalMs; }
    int historySeconds() const { return m_historySeconds; }

    const History& history() const { return m_history; }
    const std::optional<Snapshot>& latest() const { return m_latest; }

    // Per-process sampling costs a pass over every process, so it runs only while requested
    // (reference-counted: each caller that enables it must disable it).
    void setProcessTracking(bool enabled);
    void refreshProcesses();
    const std::optional<ProcessSnapshot>& latestProcesses() const { return m_latestProcesses; }
    const QList<ProviderStatus>& providers() const { return m_providers; }
    bool isStale() const;

Q_SIGNALS:
    void updated();        // new snapshot appended
    void processesUpdated();
    void stateChanged();   // paused/resumed, became stale/fresh, interval or providers changed

private:
    void onDetection(const hw::DetectionResult& result);
    void onSnapshot(const Snapshot& snapshot);

    hw::HardwareService& m_hardware;
    QThread* m_thread;
    MonitorWorker* m_worker;
    History m_history;
    std::optional<Snapshot> m_latest;
    std::optional<ProcessSnapshot> m_latestProcesses;
    int m_processTrackingRefs = 0;
    QList<ProviderStatus> m_providers;
    QTimer m_staleCheck;
    QElapsedTimer m_sinceLastSnapshot; // UI-side clock: time since the last snapshot arrived
    bool m_paused = false;
    bool m_wasStale = false;
    bool m_haveTargets = false;
    int m_intervalMs = 1000;
    int m_historySeconds = 300;
};

} // namespace gf::mon
