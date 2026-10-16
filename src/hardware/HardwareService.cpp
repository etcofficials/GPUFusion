#include "hardware/HardwareService.h"

#include "core/Logging.h"
#include "hardware/AdapterDetector.h"

namespace gf::hw {

HardwareService::HardwareService(QObject* parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1500);
    connect(&m_debounce, &QTimer::timeout, this, [this] { refresh(false); });
}

HardwareService::~HardwareService()
{
    // Probes cannot be interrupted; wait for the current one (normally well under a second).
    if (m_thread)
        m_thread->wait();
}

void HardwareService::refresh(bool forceProbe)
{
    if (m_running) {
        m_pending = true;
        m_pendingForce = m_pendingForce || forceProbe;
        return;
    }
    start(forceProbe);
}

void HardwareService::deviceConfigurationChanged()
{
    qCDebug(lcHardware) << "Device configuration changed; re-detecting shortly";
    m_debounce.start();
}

void HardwareService::start(bool forceProbe)
{
    m_running = true;
    Q_EMIT detectionStarted();

    std::optional<DetectionResult> previous = forceProbe ? std::nullopt : m_latest;
    QThread* thread = QThread::create([this, previous = std::move(previous)] {
        DetectionResult result = detectAdapters(previous ? &*previous : nullptr);
        // Delivered on the service's thread; dropped automatically if the service is gone.
        QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable { finished(std::move(result)); },
                                  Qt::QueuedConnection);
    });
    thread->setObjectName(QStringLiteral("gpufusion-detect"));
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    m_thread = thread;
    thread->start(QThread::LowPriority);
}

void HardwareService::finished(DetectionResult result)
{
    m_running = false;
    m_latest = std::move(result);
    Q_EMIT detectionFinished(*m_latest);

    if (m_pending) {
        const bool force = m_pendingForce;
        m_pending = m_pendingForce = false;
        start(force);
    }
}

} // namespace gf::hw
