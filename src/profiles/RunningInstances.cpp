#include "profiles/RunningInstances.h"

#include "core/Logging.h"
#include "platform/windows/ProcessList.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QThread>

#include <windows.h>

namespace gf::profiles {

namespace {

bool samePath(const QString& a, const QString& b)
{
    return !a.isEmpty() && QDir::toNativeSeparators(QDir::cleanPath(a)).compare(QDir::toNativeSeparators(QDir::cleanPath(b)), Qt::CaseInsensitive) == 0;
}

QList<RunningInstance> instancesIn(const QList<mon::win::ProcessInfo>& processes, const QString& exePath)
{
    QList<RunningInstance> out;
    for (const mon::win::ProcessInfo& p : processes)
        if (samePath(p.path, exePath))
            out << RunningInstance{ p.pid, p.creationTime };
    return out;
}

struct EnumState {
    QSet<quint32> pids;
    QList<HWND> windows;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM lp)
{
    auto* st = reinterpret_cast<EnumState*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Top-level windows the user could close: visible (minimised windows are visible too), unowned.
    if (st->pids.contains(quint32(pid)) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER))
        st->windows << hwnd;
    return TRUE;
}

} // namespace

QList<RunningInstance> runningInstances(const QString& exePath)
{
    mon::win::ProcessList list;
    return instancesIn(list.snapshot(), exePath);
}

CloseOutcome closeGracefully(const QString& exePath, int timeoutMs, const std::atomic<bool>* cancel)
{
    CloseOutcome out;
    mon::win::ProcessList list;
    const QList<RunningInstance> before = instancesIn(list.snapshot(), exePath);
    out.instances = int(before.size());
    const QString name = QFileInfo(exePath).fileName();
    if (before.isEmpty()) {
        out.message = QStringLiteral("%1 is not running.").arg(name);
        return out;
    }
    EnumState st;
    for (const RunningInstance& r : before)
        st.pids.insert(r.pid);
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&st));
    for (HWND w : st.windows)
        if (PostMessageW(w, WM_CLOSE, 0, 0))
            ++out.windowsAsked;

    auto stillRunning = [&] {
        QList<quint32> alive;
        const QList<RunningInstance> now = instancesIn(list.snapshot(), exePath);
        for (const RunningInstance& r : before)
            for (const RunningInstance& n : now)
                if (n.pid == r.pid && n.creationTime == r.creationTime)
                    alive << r.pid;
        return alive;
    };
    QElapsedTimer t;
    t.start();
    out.remaining = stillRunning();
    while (!out.remaining.isEmpty() && t.elapsed() < timeoutMs && !(cancel && cancel->load())) {
        QThread::msleep(200);
        out.remaining = stillRunning();
    }

    if (out.remaining.isEmpty())
        out.message = QStringLiteral("Closed %1 (%2 process(es)).").arg(name).arg(out.instances);
    else if (out.windowsAsked == 0)
        out.message = QStringLiteral("%1 is running without a window (%2 process(es), e.g. in the background or the notification area), so "
                                     "GPU Fusion cannot ask it to close. Close it yourself (or exit it from its tray icon), then try again.")
                          .arg(name).arg(out.remaining.size());
    else
        out.message = QStringLiteral("%1 did not close within %2 s (%3 process(es) still running; it may be asking to save something, or "
                                     "keep running in the background). GPU Fusion does not end programs by force: close it yourself, "
                                     "then try again.").arg(name).arg(timeoutMs / 1000).arg(out.remaining.size());
    qCInfo(lcPrefs).noquote() << "Close request for" << QDir::toNativeSeparators(exePath) << ":" << out.message;
    return out;
}

} // namespace gf::profiles
