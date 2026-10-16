// Drives the real Benchmarks page: choose two quick benchmarks, Run, and check that GPU Fusion's own
// monitoring is paused during the run and resumed afterwards, that the session is saved in the data
// folder and shown in the results table, and that a second run is compared with the first.
#include "benchmarks/BenchmarkService.h"
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/pages/BenchmarksPage.h"
#include "ui/pages/WorkloadLabPage.h"
#include "benchmarks/LabRunner.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace gf;

class TestBenchmarksUi : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void runFromThePage()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        loaded.settings.benchmarkWarmupRuns = 0;
        loaded.settings.benchmarkMeasuredRuns = 3;
        hw::HardwareService hardware;
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), QString());
        bench::BenchmarkService benchmarks(data.benchmarksDir());
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, &benchmarks);
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));

        ui::BenchmarksPage page(context);
        page.resize(1100, 1000);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));

        // Keep only two fast benchmarks.
        auto* tree = page.findChild<QTreeWidget*>();
        QVERIFY(tree);
        int kept = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const QString uid = (*it)->data(0, Qt::UserRole).toString();
            if (uid.isEmpty())
                continue;
            const bool keep = uid == QLatin1StringView("monitor.sample") || uid == QLatin1StringView("detect.reused");
            (*it)->setCheckState(0, keep ? Qt::Checked : Qt::Unchecked);
            kept += keep;
        }
        QCOMPARE(kept, 2);

        QPushButton* run = nullptr;
        for (QPushButton* b : page.findChildren<QPushButton*>()) {
            if (b->text() == QLatin1StringView("Run selected"))
                run = b;
        }
        QVERIFY(run && run->isEnabled());
        auto* table = page.findChild<QTableWidget*>();
        QVERIFY(table);

        for (int round = 1; round <= 2; ++round) {
            QSignalSpy finished(&benchmarks, &bench::BenchmarkService::finished);
            QVERIFY(!monitor.isPaused());
            run->click();
            QVERIFY(monitor.isPaused()); // our own sampling is stopped while measuring
            QVERIFY(!run->isEnabled());
            QVERIFY(finished.wait(60000));
            QVERIFY(!monitor.isPaused());
            QTRY_VERIFY(run->isEnabled());
            QCOMPARE(benchmarks.history().size(), round);
            QCOMPARE(table->rowCount(), 2);
            for (int r = 0; r < 2; ++r)
                QCOMPARE(table->item(r, 4)->text(), QStringLiteral("3"));
        }
        // The second session is shown and compared with the first (same tests, sizes and hardware).
        QVERIFY(table->item(0, 6)->text().contains(QStringLiteral("than")) || table->item(0, 6)->text().startsWith(QStringLiteral("No significant change vs")));
        QVERIFY(QFile::exists(QDir(data.benchmarksDir()).filePath(QStringLiteral("results.csv"))));
    }

    void workloadLabRunsTheSplitExperiment()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        loaded.settings.benchmarkWarmupRuns = 0;
        loaded.settings.benchmarkMeasuredRuns = 2;
        hw::HardwareService hardware;
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), QString());
        bench::BenchmarkService lab(bench::labDir(data.benchmarksDir()));
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, nullptr, &lab);
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));

        ui::WorkloadLabPage page(context);
        page.resize(1100, 1000);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto* size = page.findChild<QComboBox*>();
        QVERIFY(size);
        size->setCurrentIndex(size->findData(QStringLiteral("quick")));
        QList<QPushButton*> runs;
        for (QPushButton* b : page.findChildren<QPushButton*>()) {
            if (b->text() == QLatin1StringView("Run experiment"))
                runs << b;
        }
        QCOMPARE(runs.size(), 2);
        QVERIFY(runs[0]->isEnabled());

        QSignalSpy finished(&lab, &bench::BenchmarkService::finished);
        runs[0]->click(); // split compute (first panel)
        QVERIFY(monitor.isPaused());
        QVERIFY(!runs[1]->isEnabled()); // one experiment at a time
        QVERIFY(finished.wait(120000));
        QVERIFY(!monitor.isPaused());
        QTRY_VERIFY(runs[0]->isEnabled() && runs[1]->isEnabled());
        QCOMPARE(lab.history().size(), 1);
        auto* table = page.findChildren<QTableWidget*>().first();
        QCOMPARE(table->rowCount(), lab.history().first().results.size());
        for (int r = 0; r < table->rowCount(); ++r)
            QCOMPARE(table->item(r, 5)->text(), QStringLiteral("Correct"));
        const auto labels = page.findChildren<QLabel*>();
        QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel* l) { return l->text().contains(QStringLiteral("Combining")); }));
    }
};

QTEST_MAIN(TestBenchmarksUi)
#include "tst_benchmarks_ui.moc"
