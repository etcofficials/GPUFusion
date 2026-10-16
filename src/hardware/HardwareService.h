#pragma once

#include "hardware/AdapterModel.h"

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <optional>

namespace gf::hw {

// Owns the latest adapter list and refreshes it on a worker thread.
// - refresh(true): user-requested; probes every adapter again.
// - deviceConfigurationChanged(): debounced; reuses probes of unchanged adapters, so ordinary
//   device-change notifications (USB etc.) do not recreate Direct3D devices.
// Only one detection runs at a time; requests that arrive meanwhile are coalesced.
class HardwareService : public QObject {
    Q_OBJECT
public:
    explicit HardwareService(QObject* parent = nullptr);
    ~HardwareService() override;

    void refresh(bool forceProbe);
    void deviceConfigurationChanged();

    bool isRunning() const { return m_running; }
    const std::optional<DetectionResult>& latest() const { return m_latest; }

Q_SIGNALS:
    void detectionStarted();
    void detectionFinished(const gf::hw::DetectionResult& result);

private:
    void start(bool forceProbe);
    void finished(DetectionResult result);

    std::optional<DetectionResult> m_latest;
    QPointer<QThread> m_thread;
    QTimer m_debounce;
    bool m_running = false;
    bool m_pending = false;
    bool m_pendingForce = false;
};

} // namespace gf::hw
