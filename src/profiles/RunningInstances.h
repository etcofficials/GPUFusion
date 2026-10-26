#pragma once

// Running instances of a program, and asking them to close — the same as clicking the window's close
// button. Windows applies a per-app GPU preference, and programs read their own GPU option, only when
// they start; a program that is already running (or a browser that hands a new launch to its running
// instance) keeps the GPU it has. GPU Fusion never ends a program by force: if it does not close, the
// caller is told which processes are still running.

#include <QList>
#include <QString>

#include <atomic>

namespace gf::profiles {

struct RunningInstance {
    quint32 pid = 0;
    quint64 creationTime = 0;
};
QList<RunningInstance> runningInstances(const QString& exePath);

struct CloseOutcome {
    int instances = 0;         // running when asked
    int windowsAsked = 0;      // top-level windows sent a close request
    QList<quint32> remaining;  // still running at the end
    bool ok() const { return remaining.isEmpty(); }
    QString message;           // plain-language summary
};
// Sends a close request (WM_CLOSE) to every top-level window of the program's running instances and
// waits up to `timeoutMs` for all of them to exit.
CloseOutcome closeGracefully(const QString& exePath, int timeoutMs = 15000, const std::atomic<bool>* cancel = nullptr);

} // namespace gf::profiles
