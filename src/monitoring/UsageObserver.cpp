#include "monitoring/UsageObserver.h"

#include "core/Logging.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/ProcessList.h"

#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#include <windows.h>

namespace gf::mon {

namespace {

quint64 nowFiletime()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (quint64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool samePath(const QString& a, const QString& b)
{
    return !a.isEmpty() && QDir::toNativeSeparators(a).compare(QDir::toNativeSeparators(b), Qt::CaseInsensitive) == 0;
}

} // namespace

UsageEvidence observeUsage(const QList<MonitorTarget>& targets, const ObserveOptions& options, const std::atomic<bool>* cancel,
                           const ObserveProgress& progress)
{
    UsageEvidence e;
    e.targetExe = options.exe;

    QList<quint64> pdhLuids;
    QList<win::KmtProcessProvider::Adapter> legacy;
    for (const MonitorTarget& t : targets) {
        if (t.software)
            continue;
        e.adapters << ObservedAdapter{ t.luid, t.name };
        if (t.wddmVersion >= 2000)
            pdhLuids << t.luid;
        else
            legacy << win::KmtProcessProvider::Adapter{ t.luid, t.name };
    }

    win::ProcessList list;
    const QList<win::ProcessInfo> before = list.snapshot();
    e.alreadyRunning = std::any_of(before.begin(), before.end(), [&](const win::ProcessInfo& p) { return samePath(p.path, options.exe); });

    // Allow for clock granularity: the launched process is created a moment after this.
    const quint64 launchTime = nowFiletime() - 20'000'000ull; // 2 s earlier, in 100 ns units
    QString error;
    const std::optional<quint32> root = pref::win::launchDetachedPid(options.exe, options.arguments, &error, options.environment);
    if (!root) {
        e.launchError = error.isEmpty() ? QStringLiteral("The program could not be started.") : error;
        return e;
    }
    e.launched = true;
    qCInfo(lcPrefs).noquote() << "Usage verification: started" << QDir::toNativeSeparators(options.exe) << "pid" << *root;

    win::PdhGpuProcessProvider pdh;
    win::KmtProcessProvider kmt;
    kmt.setAdapters(legacy);

    // The tree: pid -> creation time. A child must have been created after its parent (pids are reused).
    QHash<quint32, quint64> tree;
    const int passes = std::max(2, options.seconds * 1000 / std::max(100, options.intervalMs));
    QElapsedTimer clock;
    clock.start();
    for (int pass = 0; pass < passes; ++pass) {
        if (cancel && cancel->load())
            break;
        const QList<win::ProcessInfo> processes = list.snapshot();
        QHash<quint32, const win::ProcessInfo*> byPid;
        for (const win::ProcessInfo& p : processes)
            byPid.insert(p.pid, &p);
        if (const auto it = byPid.constFind(*root); it != byPid.cend() && !tree.contains(*root) && (*it)->creationTime >= launchTime)
            tree.insert(*root, (*it)->creationTime);
        for (bool grew = true; grew;) { // add descendants until nothing new
            grew = false;
            for (const win::ProcessInfo& p : processes) {
                if (tree.contains(p.pid) || !tree.contains(p.parentPid))
                    continue;
                if (p.creationTime != 0 && p.creationTime >= tree.value(p.parentPid)) {
                    tree.insert(p.pid, p.creationTime);
                    grew = true;
                }
            }
        }
        QList<win::ProcessInfo> members;
        for (const win::ProcessInfo& p : processes) {
            if (tree.value(p.pid, 0) == p.creationTime && tree.contains(p.pid))
                members << p;
        }

        QHash<quint32, QList<ProcessAdapterUsage>> usage;
        if (!pdhLuids.isEmpty()) {
            QList<PdhEngineReading> engines;
            QList<PdhMemoryReading> memory;
            if (pdh.collect(&engines, &memory))
                usage = aggregatePdh(engines, memory, pdhLuids);
        }
        int inspectable = 0, notInspectable = 0;
        kmt.sample(members, usage, &inspectable, &notInspectable);

        for (const win::ProcessInfo& p : members) {
            for (const ProcessAdapterUsage& u : usage.value(p.pid))
                e.observations << UsageObservation{ pass, p.pid, p.path.isEmpty() ? p.name : p.path, u.luid, u.engines };
        }
        e.passes = pass + 1;
        if (progress)
            progress(pass + 1, passes, int(members.size()));
        // Keep a steady rhythm regardless of how long sampling took.
        const qint64 next = qint64(pass + 1) * options.intervalMs;
        while (clock.elapsed() < next && !(cancel && cancel->load()))
            QThread::msleep(50);
    }
    qCInfo(lcPrefs).noquote() << QStringLiteral("Usage verification: %1 passes, %2 processes in the tree, %3 observations")
                                     .arg(e.passes).arg(tree.size()).arg(e.observations.size());
    return e;
}

} // namespace gf::mon
