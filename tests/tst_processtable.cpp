// The Applications table model: rows are updated in place by (pid, adapter), processes that exit
// disappear, a selected row keeps its identity across refreshes, and filters behave.
#include "profiles/ProfileStore.h"
#include "ui/models/ProcessTableModel.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QTest>

using namespace gf::mon;
using gf::ui::ProcessFilterModel;
using gf::ui::ProcessTableModel;

namespace {

ProcessSample proc(quint32 pid, const QString& name, QList<ProcessAdapterUsage> usage = {})
{
    ProcessSample p;
    p.pid = pid;
    p.name = name;
    p.path = QStringLiteral("C:\\Apps\\%1").arg(name);
    p.adapters = std::move(usage);
    return p;
}

ProcessAdapterUsage use(quint64 luid, double util, double dedicated = 0)
{
    ProcessAdapterUsage u;
    u.luid = luid;
    u.utilization = MetricValue::ok(util);
    u.engines << EngineSample{ 0, QStringLiteral("3D"), MetricValue::ok(util) };
    u.dedicated = MetricValue::ok(dedicated);
    u.shared = MetricValue::ok(0);
    u.source = QStringLiteral("test");
    return u;
}

ProcessSnapshot snapshot(QList<ProcessSample> processes)
{
    ProcessSnapshot s;
    s.processes = std::move(processes);
    s.processCount = int(s.processes.size());
    return s;
}

} // namespace

class TestProcessTable : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void modelIsConsistent()
    {
        ProcessTableModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.update(snapshot({ proc(1, QStringLiteral("a.exe"), { use(10, 5) }), proc(2, QStringLiteral("b.exe")) }));
        model.update(snapshot({ proc(2, QStringLiteral("b.exe"), { use(10, 1), use(20, 2) }), proc(3, QStringLiteral("c.exe")) }));
        model.update(snapshot({}));
        QCOMPARE(model.rowCount(), 0);
    }

    void rowsUpdateInPlaceAndExitedProcessesDisappear()
    {
        ProcessTableModel model;
        model.update(snapshot({ proc(1, QStringLiteral("game.exe"), { use(10, 50) }), proc(2, QStringLiteral("idle.exe")) }));
        QCOMPARE(model.rowCount(), 2);
        const QPersistentModelIndex game = model.index(0, ProcessTableModel::Usage);
        QCOMPARE(game.data().toString(), QStringLiteral("50 %"));

        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        // idle.exe exits; game.exe's usage changes.
        model.update(snapshot({ proc(1, QStringLiteral("game.exe"), { use(10, 75) }) }));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(reset.count(), 0);              // no reset: selection and scroll survive
        QVERIFY(game.isValid());                 // the same row object, updated
        QCOMPARE(game.data().toString(), QStringLiteral("75 %"));
        QVERIFY(model.process(QStringLiteral("1:10")));
        QVERIFY(!model.process(QStringLiteral("2:0"))); // exited
    }

    // Views read data() inside the removal notification; at that moment every remaining row must
    // still describe its own process (regression: rows briefly pointed into the new process list).
    void rowsStayConsistentDuringRemovalNotifications()
    {
        ProcessTableModel model;
        model.update(snapshot({ proc(1, QStringLiteral("a.exe"), { use(10, 5) }), proc(2, QStringLiteral("b.exe")) }));
        int checks = 0;
        auto verifyRows = [&] {
            for (int r = 0; r < model.rowCount(); ++r) {
                const QString key = model.index(r, 0).data(ProcessTableModel::KeyRole).toString();
                const QString pid = model.index(r, ProcessTableModel::Pid).data().toString();
                QCOMPARE(key.section(u':', 0, 0), pid);
                ++checks;
            }
        };
        connect(&model, &QAbstractItemModel::rowsRemoved, this, verifyRows);
        connect(&model, &QAbstractItemModel::rowsInserted, this, verifyRows);
        // pid 1 exits, pid 3 starts: remaining row (pid 2) must not read pid 3's data mid-update.
        model.update(snapshot({ proc(2, QStringLiteral("b.exe")), proc(3, QStringLiteral("c.exe")) }));
        QVERIFY(checks >= 2);
        verifyRows();
    }

    void onlyChangedRowsAreSignalled()
    {
        ProcessTableModel model;
        model.update(snapshot({ proc(1, QStringLiteral("idle1.exe"), { use(10, 0, 1024) }), proc(2, QStringLiteral("busy.exe"), { use(10, 20) }),
                                proc(3, QStringLiteral("idle2.exe"), { use(10, 0, 2048) }) }));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.update(snapshot({ proc(1, QStringLiteral("idle1.exe"), { use(10, 0, 1024) }), proc(2, QStringLiteral("busy.exe"), { use(10, 35) }),
                                proc(3, QStringLiteral("idle2.exe"), { use(10, 0, 2048) }) }));
        QCOMPARE(changed.count(), 1);
        const QModelIndex top = changed.first().at(0).value<QModelIndex>();
        const QModelIndex bottom = changed.first().at(1).value<QModelIndex>();
        QCOMPARE(top.row(), bottom.row());
        QCOMPARE(top.data(ProcessTableModel::KeyRole).toString(), QStringLiteral("2:10"));
        QCOMPARE(model.index(top.row(), ProcessTableModel::Usage).data().toString(), QStringLiteral("35 %"));
    }

    void oneRowPerAdapterAndNoneForUnusedProcesses()
    {
        ProcessTableModel model;
        model.setAdapters({ { 10, { QStringLiteral("GT 730"), false } }, { 30, { QStringLiteral("Basic Render"), true } } });
        model.update(snapshot({ proc(1, QStringLiteral("both.exe"), { use(10, 3), use(30, 20) }), proc(2, QStringLiteral("none.exe")) }));
        QCOMPARE(model.rowCount(), 3);
        QStringList gpus;
        for (int r = 0; r < model.rowCount(); ++r)
            gpus << model.index(r, ProcessTableModel::Gpu).data().toString();
        QVERIFY(gpus.contains(QStringLiteral("GT 730")));
        QVERIFY(gpus.contains(QStringLiteral("Basic Render (software, CPU)")));
        QVERIFY(gpus.contains(QStringLiteral("—")));
    }

    void filterHidesProcessesWithoutGpuFootprint()
    {
        ProcessTableModel model;
        ProcessFilterModel filter;
        filter.setSourceModel(&model);
        ProcessAdapterUsage memoryOnly = use(10, 0, 4096);
        model.update(snapshot({ proc(1, QStringLiteral("active.exe"), { use(10, 12) }), proc(2, QStringLiteral("holder.exe"), { memoryOnly }),
                                proc(3, QStringLiteral("plain.exe")) }));
        QCOMPARE(filter.rowCount(), 2); // active + memory-only; plain.exe hidden by default
        filter.setShowAll(true);
        QCOMPARE(filter.rowCount(), 3);
        filter.setSearch(QStringLiteral("HOLDER"));
        QCOMPARE(filter.rowCount(), 1);
        filter.setSearch(QStringLiteral("3")); // PID
        QCOMPARE(filter.rowCount(), 1);
        filter.setSearch(QString());
        filter.setAdapter(10);
        QCOMPARE(filter.rowCount(), 2);
    }

    void preferencesShowOnlyWhenSet()
    {
        ProcessTableModel model;
        model.update(snapshot({ proc(1, QStringLiteral("a.exe")), proc(2, QStringLiteral("b.exe")) }));
        model.setPreferences({ { QStringLiteral("c:\\apps\\b.exe"), GpuPreference::HighPerformance } });
        QStringList prefs;
        for (int r = 0; r < model.rowCount(); ++r)
            prefs << model.index(r, ProcessTableModel::Preference).data().toString();
        QVERIFY(prefs.contains(QStringLiteral("High performance")));
        QVERIFY(prefs.contains(QString())); // not set -> blank, not a guessed value
    }

    // The live GPU-profile column: matches / on another GPU / no GPU work / no profile, and every row of
    // a profiled process is refreshed when its activity changes on any adapter.
    void profileColumnComparesLiveUseWithTheProfile()
    {
        using namespace gf::profiles;
        const gf::pref::SelectableGpu nv{ QStringLiteral("GT 730"), 0x10DE, gf::hw::formatLuid(10), false, 0 };
        const gf::pref::SelectableGpu intel{ QStringLiteral("HD Graphics"), 0x8086, gf::hw::formatLuid(20), true, 1 };
        ProcessTableModel model;
        model.setAdapters({ { 10, { nv.name, false } }, { 20, { intel.name, false } } });
        AppProfile onIntel = newProfile(QStringLiteral("C:\\Apps\\video.exe"));
        onIntel.mode = GpuMode::Integrated;
        AppProfile onNv = newProfile(QStringLiteral("C:\\Apps\\game.exe"));
        onNv.mode = GpuMode::Discrete;
        AppProfile automatic = newProfile(QStringLiteral("C:\\Apps\\auto.exe"));
        model.setProfiles({ onIntel, onNv, automatic }, { nv, intel });

        model.update(snapshot({ proc(1, QStringLiteral("video.exe"), { use(10, 30) }), proc(2, QStringLiteral("game.exe"), { use(10, 40) }),
                                proc(3, QStringLiteral("auto.exe"), { use(10, 5) }), proc(4, QStringLiteral("other.exe"), { use(10, 9) }) }));
        auto profileText = [&](const QString& exe) {
            for (int r = 0; r < model.rowCount(); ++r)
                if (model.index(r, ProcessTableModel::Application).data().toString() == exe)
                    return model.index(r, ProcessTableModel::Profile).data().toString();
            return QStringLiteral("<missing>");
        };
        QVERIFY(profileText(QStringLiteral("video.exe")).startsWith(QStringLiteral("✗")));      // asks for Intel, works on the GT 730
        for (int r = 0; r < model.rowCount(); ++r)                                                // where instead: in the tooltip
            if (model.index(r, ProcessTableModel::Application).data().toString() == QStringLiteral("video.exe"))
                QVERIFY(model.index(r, ProcessTableModel::Profile).data(Qt::ToolTipRole).toString().contains(QStringLiteral("Working on GT 730")));
        QVERIFY(profileText(QStringLiteral("game.exe")).startsWith(QStringLiteral("✓")));
        QCOMPARE(profileText(QStringLiteral("auto.exe")), QStringLiteral("Automatic"));
        QCOMPARE(profileText(QStringLiteral("other.exe")), QString());                          // no profile: blank

        // video.exe moves to the Intel GPU (a new row appears, the GT 730 row goes idle): its check follows.
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.update(snapshot({ proc(1, QStringLiteral("video.exe"), { use(10, 0, 1024), use(20, 50) }), proc(2, QStringLiteral("game.exe"), { use(10, 40) }),
                                proc(3, QStringLiteral("auto.exe"), { use(10, 5) }), proc(4, QStringLiteral("other.exe"), { use(10, 9) }) }));
        for (int r = 0; r < model.rowCount(); ++r)
            if (model.index(r, ProcessTableModel::Application).data().toString() == QStringLiteral("video.exe"))
                QVERIFY2(model.index(r, ProcessTableModel::Profile).data().toString().startsWith(QStringLiteral("✓")),
                         qPrintable(model.index(r, ProcessTableModel::Profile).data().toString()));
        QVERIFY(changed.count() >= 1);

        model.update(snapshot({ proc(1, QStringLiteral("video.exe"), { use(20, 0, 1024) }) }));                  // memory only
        QVERIFY(profileText(QStringLiteral("video.exe")).endsWith(QStringLiteral("idle")));

        // An Automatic variant accepts any GPU: on the GT 730 the program is consistent with it, not "✗".
        AppProfile anyGpu = *newVariant(onIntel, QStringLiteral("any"));
        anyGpu.mode = GpuMode::Automatic; // a variant starts as a copy of the main profile
        model.setProfiles({ onIntel, anyGpu, onNv, automatic }, { nv, intel });
        model.update(snapshot({ proc(1, QStringLiteral("video.exe"), { use(10, 30) }) }));
        QVERIFY2(profileText(QStringLiteral("video.exe")).startsWith(QStringLiteral("✓")), qPrintable(profileText(QStringLiteral("video.exe"))));
        QVERIFY(model.index(0, ProcessTableModel::Profile).data(Qt::ToolTipRole).toString().contains(QStringLiteral("consistent with")));
    }

    // Cost of the live profile column on a busy PC: a refresh plus reading every row's profile cell
    // (as a full repaint does) with 250 processes, 40 of them profiled, stays small next to the
    // 1-second refresh interval and close to the cost without profiles.
    void profileColumnCostIsSmall()
    {
        using namespace gf::profiles;
        const gf::pref::SelectableGpu nv{ QStringLiteral("GT 730"), 0x10DE, gf::hw::formatLuid(10), false, 0 };
        const gf::pref::SelectableGpu intel{ QStringLiteral("HD Graphics"), 0x8086, gf::hw::formatLuid(20), true, 1 };
        QList<ProcessSample> procs;
        QList<AppProfile> profiles;
        for (int i = 0; i < 250; ++i) {
            const QString exe = QStringLiteral("app%1.exe").arg(i % 120);
            procs << proc(quint32(1000 + i), exe, { use(10, i % 7), use(20, i % 3) });
            if (i < 40) {
                AppProfile p = newProfile(QStringLiteral("C:\\Apps\\%1").arg(exe));
                p.mode = i % 2 ? GpuMode::Integrated : GpuMode::Discrete;
                profiles << p;
            }
        }
        auto measure = [&](bool withProfiles) {
            ProcessTableModel model;
            model.setAdapters({ { 10, { nv.name, false } }, { 20, { intel.name, false } } });
            if (withProfiles)
                model.setProfiles(profiles, { nv, intel });
            model.update(snapshot(procs));
            QElapsedTimer t;
            t.start();
            for (int round = 0; round < 10; ++round) {
                for (ProcessSample& p : procs)               // every process changes a little each refresh
                    p.adapters[0].utilization = MetricValue::ok(double((round + p.pid) % 9));
                model.update(snapshot(procs));
                for (int r = 0; r < model.rowCount(); ++r)
                    for (int role : { int(Qt::DisplayRole), int(Qt::ToolTipRole), int(Qt::ForegroundRole) })
                        model.index(r, ProcessTableModel::Profile).data(role);
            }
            return double(t.nsecsElapsed()) / 1e6 / 10;      // ms per refresh + repaint
        };
        const double without = measure(false), with = measure(true);
        qInfo().noquote() << QStringLiteral("per refresh + full repaint of the column: %1 ms without profiles, %2 ms with 40 profiles (500 rows)")
                                 .arg(without, 0, 'f', 2).arg(with, 0, 'f', 2);
        // Measured 4.4 ms with the per-process cache (14.1 ms when every cell recomputed the check), in the
        // optimised build users run; an unoptimised debug build is several times slower, so only report there.
#ifdef NDEBUG
        QVERIFY2(with < 20.0, qPrintable(QStringLiteral("%1 ms").arg(with)));
#endif
    }

    void nonOkValuesShowTheirState()
    {
        ProcessTableModel model;
        ProcessAdapterUsage u = use(10, 0);
        u.utilization = MetricValue::unknown(QStringLiteral("Needs two samples"));
        u.dedicated = MetricValue::unavailable(QStringLiteral("no counter"));
        model.update(snapshot({ proc(1, QStringLiteral("new.exe"), { u }) }));
        QCOMPARE(model.index(0, ProcessTableModel::Usage).data().toString(), QStringLiteral("Unknown"));
        QCOMPARE(model.index(0, ProcessTableModel::Dedicated).data().toString(), QStringLiteral("Unavailable"));
        QCOMPARE(model.index(0, ProcessTableModel::Usage).data(ProcessTableModel::SortRole).toDouble(), -1.0);
    }
};

QTEST_MAIN(TestProcessTable)
#include "tst_processtable.moc"
