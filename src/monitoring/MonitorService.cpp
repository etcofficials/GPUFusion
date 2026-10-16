#include "monitoring/MonitorService.h"

#include "core/Logging.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorWorker.h"

#include <QThread>

namespace gf::mon {

MonitorService::MonitorService(hw::HardwareService& hardware, const Settings& settings, QObject* parent)
    : QObject(parent), m_hardware(hardware), m_thread(new QThread), m_worker(new MonitorWorker)
{
    qRegisterMetaType<Snapshot>();
    qRegisterMetaType<QList<MonitorTarget>>();
    qRegisterMetaType<QList<ProviderStatus>>();
    qRegisterMetaType<ProcessSnapshot>();

    m_thread->setObjectName(QStringLiteral("gpufusion-monitor"));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &MonitorWorker::snapshotReady, this, &MonitorService::onSnapshot);
    connect(m_worker, &MonitorWorker::processSnapshotReady, this, [this](const ProcessSnapshot& s) {
        m_latestProcesses = s;
        Q_EMIT processesUpdated();
    });
    connect(m_worker, &MonitorWorker::providersChanged, this, [this](const QList<ProviderStatus>& p) {
        m_providers = p;
        Q_EMIT stateChanged();
    });
    connect(m_worker, &MonitorWorker::adaptersLost, &m_hardware, &hw::HardwareService::deviceConfigurationChanged);
    connect(&m_hardware, &hw::HardwareService::detectionFinished, this, &MonitorService::onDetection);
    m_thread->start(QThread::LowPriority);

    m_staleCheck.setInterval(1000);
    connect(&m_staleCheck, &QTimer::timeout, this, [this] {
        const bool stale = isStale();
        if (stale != m_wasStale) {
            m_wasStale = stale;
            if (stale)
                qCWarning(lcMonitor) << "Monitoring data is stale: no sample for" << m_intervalMs * 3 << "ms or more";
            Q_EMIT stateChanged();
        }
    });
    m_staleCheck.start();

    m_paused = !settings.startMonitoringOnLaunch;
    applySettings(settings);
    if (m_hardware.latest())
        onDetection(*m_hardware.latest());
}

MonitorService::~MonitorService()
{
    QMetaObject::invokeMethod(m_worker, &MonitorWorker::shutdown, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_thread; // the worker was deleted via deleteLater when the thread finished
}

void MonitorService::applySettings(const Settings& s)
{
    const bool intervalChanged = s.refreshIntervalMs != m_intervalMs;
    m_intervalMs = s.refreshIntervalMs;
    m_historySeconds = s.historySeconds;
    m_history.setCapacity(historyCapacity(m_historySeconds, m_intervalMs));
    if (intervalChanged)
        m_history.clear(); // points at different spacing would misrepresent the time axis

    const bool nvml = s.useNvml;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, nvml] { w->setNvmlEnabled(nvml); });
    const int interval = m_intervalMs;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, interval] { w->setInterval(interval); });
    Q_EMIT stateChanged();
}

void MonitorService::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (paused) {
        QMetaObject::invokeMethod(m_worker, &MonitorWorker::stop);
    } else if (m_haveTargets) {
        const int interval = m_intervalMs;
        QMetaObject::invokeMethod(m_worker, [w = m_worker, interval] { w->start(interval); });
    }
    Q_EMIT stateChanged();
}

void MonitorService::setProcessTracking(bool enabled)
{
    const int before = m_processTrackingRefs;
    m_processTrackingRefs = std::max(0, m_processTrackingRefs + (enabled ? 1 : -1));
    if ((before == 0) != (m_processTrackingRefs == 0)) {
        const bool on = m_processTrackingRefs > 0;
        QMetaObject::invokeMethod(m_worker, [w = m_worker, on] { w->setProcessTracking(on); });
    }
}

void MonitorService::refreshProcesses()
{
    QMetaObject::invokeMethod(m_worker, &MonitorWorker::sampleProcessesNow);
}

bool MonitorService::isStale() const
{
    if (m_paused || !m_latest)
        return false;
    return gf::mon::isStale(0, m_sinceLastSnapshot.elapsed(), m_intervalMs);
}

void MonitorService::onDetection(const hw::DetectionResult& result)
{
    const QList<MonitorTarget> targets = targetsFrom(result);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, targets] { w->setTargets(targets); });
    if (!m_haveTargets && !m_paused) {
        const int interval = m_intervalMs;
        QMetaObject::invokeMethod(m_worker, [w = m_worker, interval] { w->start(interval); });
    }
    m_haveTargets = true;
}

void MonitorService::onSnapshot(const Snapshot& snapshot)
{
    if (m_paused)
        return; // a sample that was in flight when pausing
    m_latest = snapshot;
    m_sinceLastSnapshot.restart();
    m_history.append(snapshot);
    Q_EMIT updated();
}

} // namespace gf::mon
