// The game check's analysis from SIMULATED samples (deterministic; the sampling itself is exercised by
// tst_gamecheck_live): competitors on the game's GPU and whether they could move, indications labelled
// as indications, before/after, storage and the report's wording.
#include "games/GameCheck.h"

#include <QTemporaryDir>
#include <QTest>

using namespace gf;
using namespace gf::games;

namespace {
constexpr quint64 kGt = 0x808D, kIntel = 0x89C0;

ProcessGpu use(quint32 pid, const QString& name, quint64 luid, double percent, const QString& engine = QStringLiteral("3D"))
{
    ProcessGpu g;
    g.pid = pid;
    g.name = name;
    g.path = QStringLiteral("C:/apps/") + name;
    g.luid = luid;
    g.adapter = luid == kGt ? QStringLiteral("NVIDIA GeForce GT 730") : QStringLiteral("Intel(R) HD Graphics");
    g.meanPercent = percent;
    g.busiestEngine = engine;
    return g;
}

QList<Sample> samples(int n, const QList<ProcessGpu>& each, const QList<double>& cores, double gameCpu)
{
    QList<Sample> out;
    for (int i = 0; i < n; ++i) {
        Sample s;
        s.gpu = each;
        s.corePercent = cores;
        s.gameCpuPercentOfOneCore = gameCpu;
        out << s;
    }
    return out;
}

compat::Assessment fakeAssess(const QString& exe)
{
    compat::Assessment a;
    for (compat::Capability c : compat::allCapabilities()) {
        compat::Answer x;
        x.capability = c;
        x.status = exe.endsWith(QLatin1StringView("msedge.exe")) ? compat::Status::Supported : compat::Status::Unsupported;
        x.summary = QStringLiteral("test");
        if (exe.endsWith(QLatin1StringView("msedge.exe")))
            x.reasons << QStringLiteral("How: --use-adapter-luid={luid_pair} in a GPU Fusion profile.");
        a.answers << x;
    }
    return a;
}
} // namespace

class TestGameCheck : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void gpuBoundGameWithACompetitor()
    {
        const QList<ProcessGpu> each{ use(100, QStringLiteral("game.exe"), kGt, 92), use(200, QStringLiteral("msedge.exe"), kGt, 22),
                                      use(300, QStringLiteral("dwm.exe"), kGt, 10), use(400, QStringLiteral("System"), kGt, 0.1),
                                      use(500, QStringLiteral("video.exe"), kIntel, 30, QStringLiteral("Engine 0")) };
        const GameCheck c = analyze(100, QStringLiteral("game.exe"), QStringLiteral("C:/apps/game.exe"), samples(10, each, { 40, 35, 30, 25 }, 60), fakeAssess);
        QCOMPARE(c.gameLuid, kGt);
        QCOMPARE(c.gameGpuMean, 92.0);
        QCOMPARE(c.competitors.size(), 2);                 // the trace (System, 0.1 %) is not competition; Intel's user is not either
        QCOMPARE(c.competitors[0].use.name, QStringLiteral("msedge.exe"));
        QVERIFY(c.competitors[0].movable());
        QCOMPARE(c.competitors[0].mechanism, QStringLiteral("--use-adapter-luid={luid_pair} in a GPU Fusion profile"));
        QVERIFY(!c.competitors[1].movable());
        QCOMPARE(c.othersOnGameGpuMean, 32.0);
        QCOMPARE(c.indications.size(), 2);
        QCOMPARE(c.indications[0].first, Indication::GpuBound);
        QCOMPARE(c.indications[1].first, Indication::Contention);
        QVERIFY(c.indications[1].second.contains(QStringLiteral("77 %")));
        QCOMPARE(c.processes.first().name, QStringLiteral("game.exe"));
        QVERIFY(indicationText(Indication::GpuBound).contains(QStringLiteral("indication")));
    }

    void cpuBoundAndNoneClear()
    {
        const GameCheck cpu = analyze(1, QStringLiteral("g.exe"), {}, samples(5, { use(1, QStringLiteral("g.exe"), kGt, 45) }, { 98, 40, 35, 30 }, 95), fakeAssess);
        QCOMPARE(cpu.indications.size(), 1);
        QCOMPARE(cpu.indications[0].first, Indication::CpuBound);
        QVERIFY(cpu.indications[0].second.contains(QStringLiteral("95 % of one core")));
        QCOMPARE(cpu.cpu.meanBusiestCorePercent, 98.0);

        const GameCheck calm = analyze(1, QStringLiteral("g.exe"), {}, samples(5, { use(1, QStringLiteral("g.exe"), kGt, 40) }, { 20, 20, 20, 20 }, 30), fakeAssess);
        QCOMPARE(calm.indications[0].first, Indication::NoneClear);

        const GameCheck idle = analyze(1, QStringLiteral("g.exe"), {}, samples(5, {}, { 5, 5, 5, 5 }, 1), fakeAssess);
        QCOMPARE(idle.gameLuid, quint64(0));
        QVERIFY(idle.notes.join(u' ').contains(QStringLiteral("did no GPU work")));
        QVERIFY(!analyze(1, QStringLiteral("g.exe"), {}, {}, fakeAssess).error.isEmpty());
    }

    void storageComparisonAndReport()
    {
        const QList<ProcessGpu> busy{ use(100, QStringLiteral("game.exe"), kGt, 75), use(200, QStringLiteral("msedge.exe"), kGt, 22) };
        GameCheck before = analyze(100, QStringLiteral("game.exe"), QStringLiteral("C:/apps/game.exe"), samples(10, busy, { 30, 30, 30, 30 }, 40), fakeAssess);
        before.id = QStringLiteral("game-20261028-120000-000");
        before.created = QDateTime(QDate(2026, 10, 28), QTime(12, 0));
        before.label = QStringLiteral("before");
        GameCheck after = analyze(100, QStringLiteral("game.exe"), QStringLiteral("C:/apps/game.exe"),
                                  samples(10, { use(100, QStringLiteral("game.exe"), kGt, 84) }, { 30, 30, 30, 30 }, 40), fakeAssess);
        after.id = QStringLiteral("game-20261028-121000-000");
        after.created = before.created.addSecs(600);
        QTemporaryDir dir;
        QString error;
        QVERIFY2(saveCheck(dir.path(), before, &error) && saveCheck(dir.path(), after, &error), qPrintable(error));
        const QList<GameCheck> loaded = loadChecks(dir.path());
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[1].label, QStringLiteral("before"));
        QCOMPARE(loaded[1].competitors.size(), 1);
        QCOMPARE(loaded[1].competitors[0].move.status, compat::Status::Supported);
        QCOMPARE(loaded[1].indications.size(), before.indications.size());

        const QStringList cmp = compareChecks(loaded[1], loaded[0]);
        QVERIFY(cmp[0].startsWith(QStringLiteral("Frame times: not compared"))); // no frame capture in either
        QVERIFY(cmp[1].contains(QStringLiteral("22 % before, 0 % after")));
        const QString html = reportHtml(after, &loaded[1]);
        QVERIFY(html.contains(QStringLiteral("<h2>Before / after</h2>")));
        QVERIFY(html.contains(QStringLiteral("Indications</b> are not proof")));
        QVERIFY(html.contains(QStringLiteral("Nothing was started, closed or changed by this check")));
        QVERIFY(summaryLines(before).join(u'\n').contains(QStringLiteral("Competitor: msedge.exe used 22 %")));
    }
};

QTEST_GUILESS_MAIN(TestGameCheck)
#include "tst_gamecheck.moc"
