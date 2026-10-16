// Hardware-dependent per-process tests. This test process loads a GPU itself, so the right answer
// is known: the process must appear on that adapter as active, through the source GPU Fusion uses
// for that adapter (PDH for WDDM 2.0+, D3DKMT for WDDM 1.x) — and the two sources must agree where
// both can measure the same process.
#include "hardware/AdapterDetector.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"
#include "platform/windows/ProcessList.h"

#include "GpuLoad.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTest>

using namespace gf;
using namespace gf::mon;

namespace {

const ProcessAdapterUsage* findUsage(const QHash<quint32, QList<ProcessAdapterUsage>>& byPid, quint32 pid, quint64 luid)
{
    // Iterate the list stored in the hash: value() would return a temporary copy, and the returned
    // pointer would dangle (caught by the debug heap).
    const auto it = byPid.constFind(pid);
    if (it == byPid.cend())
        return nullptr;
    for (const ProcessAdapterUsage& u : *it) {
        if (u.luid == luid)
            return &u;
    }
    return nullptr;
}

} // namespace

class TestProcessLive : public QObject {
    Q_OBJECT

    QList<MonitorTarget> m_gpus;
    quint32 m_pid = 0;

    // Two samples ~1.5 s apart with this process loading `luid`; returns per-process usage from KMT
    // (if `kmt`) or PDH.
    QHash<quint32, QList<ProcessAdapterUsage>> measure(quint64 luid, bool kmt, QHash<quint32, QList<ProcessAdapterUsage>>* otherSource = nullptr)
    {
        win::ProcessList list;
        win::KmtProcessProvider kmtProvider;
        kmtProvider.setAdapters({ { luid, QStringLiteral("x") } });
        win::PdhGpuProcessProvider pdh;
        QList<PdhEngineReading> e;
        QList<PdhMemoryReading> m;
        GpuLoad load(luid);
        if (!load.ok())
            return {};
        QThread::msleep(300);
        QHash<quint32, QList<ProcessAdapterUsage>> k1, k2;
        int a = 0, b = 0;
        kmtProvider.sample(list.snapshot(), k1, &a, &b);
        pdh.collect(&e, &m);
        QThread::msleep(1500);
        kmtProvider.sample(list.snapshot(), k2, &a, &b);
        pdh.collect(&e, &m);
        const auto pdhResult = aggregatePdh(e, m, { luid });
        if (otherSource)
            *otherSource = kmt ? pdhResult : k2;
        return kmt ? k2 : pdhResult;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_pid = quint32(QCoreApplication::applicationPid());
        for (const MonitorTarget& t : targetsFrom(hw::detectAdapters(nullptr))) {
            if (!t.software)
                m_gpus << t;
        }
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU is available to DirectX on this machine");
    }

    void processListSeesThisProcessWithPath()
    {
        win::ProcessList list;
        const auto processes = list.snapshot();
        QVERIFY(processes.size() > 10);
        const auto self = std::find_if(processes.begin(), processes.end(), [this](const win::ProcessInfo& p) { return p.pid == m_pid; });
        QVERIFY(self != processes.end());
        QVERIFY(self->path.endsWith(QStringLiteral("tst_process_live.exe"), Qt::CaseInsensitive));
        QVERIFY(self->creationTime != 0);
        int withoutPath = 0;
        for (const auto& p : processes)
            withoutPath += p.path.isEmpty() ? 1 : 0;
        qInfo() << processes.size() << "processes," << withoutPath << "without a readable path (protected / other accounts)";
    }

    void exitedProcessesDisappear()
    {
        QProcess child;
        child.start(QStringLiteral("cmd.exe"), { QStringLiteral("/c"), QStringLiteral("ping -n 2 127.0.0.1 >nul") });
        QVERIFY(child.waitForStarted());
        const quint32 pid = quint32(child.processId());
        win::ProcessList list;
        auto has = [&](const QList<win::ProcessInfo>& ps) { return std::any_of(ps.begin(), ps.end(), [pid](const auto& p) { return p.pid == pid; }); };
        QVERIFY(has(list.snapshot()));
        QVERIFY(child.waitForFinished(10000));
        QVERIFY(!has(list.snapshot()));
        // The D3DKMT provider must cope with a list that contains processes that exit mid-pass.
        win::KmtProcessProvider kmt;
        kmt.setAdapters({ { m_gpus.first().luid, m_gpus.first().name } });
        QHash<quint32, QList<ProcessAdapterUsage>> out;
        int a = 0, b = 0;
        kmt.sample(list.snapshot(), out, &a, &b);
        QVERIFY(a > 0);
    }

    // The source GPU Fusion uses for each adapter sees this process when it loads that adapter.
    void eachGpuSeesThisProcessThroughItsSource()
    {
        for (const MonitorTarget& t : m_gpus) {
            const bool legacy = t.wddmVersion < 2000;
            const auto byPid = measure(t.luid, legacy);
            const ProcessAdapterUsage* u = findUsage(byPid, m_pid, t.luid);
            QVERIFY2(u, qPrintable(QStringLiteral("%1: this process not found via %2").arg(t.name, legacy ? QStringLiteral("D3DKMT") : QStringLiteral("PDH"))));
            qInfo().noquote() << t.name << (legacy ? "(D3DKMT)" : "(PDH)") << "this process:" << formatMetric(u->utilization, Unit::Percent)
                              << "busiest" << u->busiestEngine << "dedicated" << formatMetric(u->dedicated, Unit::Bytes)
                              << "shared" << formatMetric(u->shared, Unit::Bytes);
            QVERIFY(u->utilization.isOk());
            QVERIFY2(u->utilization.value >= 30.0, qPrintable(formatMetric(u->utilization, Unit::Percent)));
            QCOMPARE(activityOf(*u), Activity::Active);
        }
    }

    // Where both sources can measure the same process (WDDM 2.0+ adapter, own process), they agree.
    void kmtAndPdhAgreePerProcess()
    {
        const auto modern = std::find_if(m_gpus.begin(), m_gpus.end(), [](const MonitorTarget& t) { return t.wddmVersion >= 2000; });
        if (modern == m_gpus.end())
            QSKIP("No WDDM 2.0+ adapter: Windows GPU performance counters are not available for comparison");
        QHash<quint32, QList<ProcessAdapterUsage>> pdh;
        const auto kmt = measure(modern->luid, true, &pdh);
        const ProcessAdapterUsage* k = findUsage(kmt, m_pid, modern->luid);
        const ProcessAdapterUsage* p = findUsage(pdh, m_pid, modern->luid);
        QVERIFY(k && p);
        qInfo().noquote() << modern->name << "this process: D3DKMT" << formatMetric(k->utilization, Unit::Percent)
                          << "vs PDH" << formatMetric(p->utilization, Unit::Percent) << "| dedicated D3DKMT"
                          << formatMetric(k->dedicated, Unit::Bytes) << "vs PDH" << formatMetric(p->dedicated, Unit::Bytes);
        QVERIFY(std::abs(k->utilization.value - p->utilization.value) < 15.0);
    }

    void legacyCoverageIsReportedNotGuessed()
    {
        const auto legacy = std::find_if(m_gpus.begin(), m_gpus.end(), [](const MonitorTarget& t) { return t.wddmVersion > 0 && t.wddmVersion < 2000; });
        if (legacy == m_gpus.end())
            QSKIP("No WDDM 1.x adapter on this machine");
        win::ProcessList list;
        const auto processes = list.snapshot();
        win::KmtProcessProvider kmt;
        kmt.setAdapters({ { legacy->luid, legacy->name } });
        QHash<quint32, QList<ProcessAdapterUsage>> out;
        int inspectable = 0, notInspectable = 0;
        QElapsedTimer t;
        t.start();
        kmt.sample(processes, out, &inspectable, &notInspectable);
        qInfo() << "D3DKMT per-process pass:" << inspectable << "inspectable," << notInspectable << "not inspectable," << t.elapsed() << "ms";
        QCOMPARE(inspectable + notInspectable, int(processes.size()));
        QVERIFY(inspectable > 0);
        QVERIFY(t.elapsed() < 500);
    }

    void pdhPassIsAffordable()
    {
        win::PdhGpuProcessProvider pdh;
        if (!pdh.isOpen())
            QSKIP(qPrintable(pdh.error()));
        QList<PdhEngineReading> e;
        QList<PdhMemoryReading> m;
        pdh.collect(&e, &m);
        QThread::msleep(500);
        QElapsedTimer t;
        t.start();
        QVERIFY(pdh.collect(&e, &m));
        qInfo() << "PDH per-process pass:" << e.size() << "engine instances," << m.size() << "memory instances," << t.elapsed() << "ms";
        QVERIFY(!e.isEmpty());
        QVERIFY(t.elapsed() < 1000);
    }
};

QTEST_GUILESS_MAIN(TestProcessLive)
#include "tst_process_live.moc"
