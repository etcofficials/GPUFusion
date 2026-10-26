// Drives the real Applications page with live monitoring: GPUFusionProbe has an Integrated GPU
// profile; started normally it renders on the default (discrete) GPU and the page's "GPU profile"
// column must flag it (✗); started with its own --adapter option on the integrated GPU it must
// match (✓). Nothing but the probe is started, and no Windows setting is changed.
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/models/ProcessTableModel.h"
#include "ui/pages/ApplicationsPage.h"

#include "ProbePreferenceGuard.h"

#include <QAbstractProxyModel>
#include <QProcess>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

using namespace gf;

class TestApplicationsUi : public QObject {
    Q_OBJECT

    // The "GPU profile" text of the probe's row on `luid`, polled until `predicate` holds.
    static QString waitForProfileText(QTableView* table, quint64 luid, const std::function<bool(const QString&)>& predicate, int ms)
    {
        QString last;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QAbstractItemModel* m = table->model();
            for (int r = 0; r < m->rowCount(); ++r) {
                if (m->index(r, ui::ProcessTableModel::Application).data().toString().compare(QStringLiteral("GPUFusionProbe.exe"), Qt::CaseInsensitive) == 0 &&
                    m->index(r, 0).data(ui::ProcessTableModel::LuidRole).value<quint64>() == luid) {
                    last = m->index(r, ui::ProcessTableModel::Profile).data().toString();
                    if (predicate(last))
                        return last;
                }
            }
            QTest::qWait(250);
        }
        return last;
    }

private Q_SLOTS:
    void liveProfileCheckOfARunningProgram()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        std::optional<quint64> discrete, integrated;
        for (const hw::Adapter& a : hardware.latest()->adapters) {
            if (!a.dxgi || a.isSoftware())
                continue;
            (a.classification.value == hw::AdapterClass::Integrated ? integrated : discrete) = a.dxgi->luid;
        }
        if (!discrete || !integrated)
            QSKIP("Needs an integrated and a discrete GPU");

        profiles::ProfileStore store(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
        profiles::AppProfile p = profiles::newProfile(probeguard::probePath(), QStringLiteral("Probe"));
        p.mode = profiles::GpuMode::Integrated;
        QString error;
        QVERIFY(store.save(p, &error));

        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), probeguard::probePath());
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, nullptr, nullptr, &store);
        ui::ApplicationsPage page(context);
        page.resize(1100, 800);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto* table = page.findChild<QTableView*>();
        QVERIFY(table);

        // Started normally: renders on the default (discrete) GPU although its profile asks for the integrated one.
        QProcess probe;
        probe.start(probeguard::probePath(), { QStringLiteral("--render"), QStringLiteral("20") });
        QVERIFY(probe.waitForStarted(10000));
        const QString flagged = waitForProfileText(table, *discrete, [](const QString& s) { return s.startsWith(QStringLiteral("✗")); }, 15000);
        qInfo().noquote() << "default start:" << flagged;
        QVERIFY2(flagged.startsWith(QStringLiteral("✗")), qPrintable(flagged));
        probe.kill();
        probe.waitForFinished(5000);

        // Started with its own GPU option on the integrated GPU: matches its profile.
        probe.start(probeguard::probePath(), { QStringLiteral("--render"), QStringLiteral("20"), QStringLiteral("--adapter"), hw::formatLuid(*integrated) });
        QVERIFY(probe.waitForStarted(10000));
        const QString matched = waitForProfileText(table, *integrated, [](const QString& s) { return s.startsWith(QStringLiteral("✓")); }, 15000);
        qInfo().noquote() << "started on the integrated GPU:" << matched;
        QVERIFY2(matched.startsWith(QStringLiteral("✓")), qPrintable(matched));
        probe.kill();
        probe.waitForFinished(5000);
    }
};

QTEST_MAIN(TestApplicationsUi)
#include "tst_applications_ui.moc"
