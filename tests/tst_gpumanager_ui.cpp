// Drives the real GPU Manager page with GPUFusionProbe.exe as the program (an SDK-integrated program):
// add a profile, choose Integrated (no Windows value; the probe receives the GPU when launched from
// its profile) and verify on the real GPUs, then Discrete (Windows value written after confirmation)
// and Automatic (value removed). Only the probe's own preference is changed, and it is restored.
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "platform/windows/RegistryPreferenceStore.h"
#include "preferences/PreferenceService.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/pages/GpuManagerPage.h"

#include "ProbePreferenceGuard.h"

#include <QApplication>
#include <QCheckBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace gf;

namespace {

void answerNextDialog(QMessageBox::StandardButton button)
{
    QTimer::singleShot(50, [button] {
        for (int i = 0; i < 100; ++i) {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->button(button)->click();
                return;
            }
            QTest::qWait(20);
        }
    });
}

} // namespace

class TestGpuManagerUi : public QObject {
    Q_OBJECT

    pref::win::RegistryPreferenceStore m_store;

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        if (probeguard::take(m_store))
            QSKIP("The probe already has a preference set; this test only runs from a clean state");
    }

    void profileModesThroughThePage()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        std::optional<pref::ProbeAdapter> discrete, integrated;
        for (const hw::Adapter& a : hardware.latest()->adapters) {
            if (!a.dxgi || a.isSoftware())
                continue;
            const pref::ProbeAdapter p{ a.name(), a.dxgi->vendorId, hw::formatLuid(a.dxgi->luid) };
            (a.classification.value == hw::AdapterClass::Integrated ? integrated : discrete) = p;
        }
        if (!discrete || !integrated)
            QSKIP("Needs an integrated and a discrete GPU");

        // A verification like the reference PC's: every preference puts programs on the discrete GPU.
        pref::VerificationResult v;
        v.time = QDateTime::currentDateTime();
        v.restored = true;
        for (auto p : { pref::GpuPreference::NotSet, pref::GpuPreference::LetWindowsDecide, pref::GpuPreference::PowerSaving,
                        pref::GpuPreference::HighPerformance }) {
            pref::VerificationStep s;
            s.preference = p;
            s.ok = true;
            s.report.d3d11Default = s.report.minimumPower = s.report.highPerformance = *discrete;
            v.steps << s;
        }
        QFile report(QDir(data.reportsDir()).filePath(QStringLiteral("gpu-preference-verification.json")));
        QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(v.toJson()).toJson());
        report.close();

        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), probeguard::probePath());
        profiles::ProfileStore store(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, nullptr, nullptr, &store);
        ui::GpuManagerPage page(context);
        page.resize(1000, 1000);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));

        QVERIFY(page.addProgram(probeguard::probePath(), QStringLiteral("Probe")));
        QCOMPARE(store.list().size(), 1);
        page.findChild<QLineEdit*>()->setText(QStringLiteral("--render 6"));

        // Integrated: Windows cannot place programs on Intel here, so nothing is written; the probe
        // receives the GPU through its profile and must be observed on it.
        page.selectMode(profiles::GpuMode::Integrated);
        QVERIFY(page.capabilityText().contains(QStringLiteral("Only with the program's support")));
        page.applySelected(false);
        QVERIFY(!pref::findValueName(m_store, probeguard::probePath()).has_value());
        QCOMPARE(store.find(probeguard::probePath())->mode, profiles::GpuMode::Integrated);
        QSignalSpy finished(&page, &ui::GpuManagerPage::verificationFinished);
        page.verifySelected(8, false);
        QVERIFY(page.isVerifying());
        QVERIFY(finished.wait(60000));
        qInfo().noquote() << page.resultText();
        const auto saved = store.find(probeguard::probePath());
        QVERIFY(saved->lastVerification.has_value());
        QVERIFY2(saved->lastVerification->confirmed, qPrintable(saved->lastVerification->headline));
        QVERIFY(page.resultText().contains(QStringLiteral("Confirmed")));
        QVERIFY(page.findChild<QListWidget*>()->item(0)->text().contains(QStringLiteral("verified")));

        // Discrete: verified mapping -> Windows value written after confirmation and read back.
        page.selectMode(profiles::GpuMode::Discrete);
        QVERIFY(page.capabilityText().contains(QStringLiteral("Verified")));
        answerNextDialog(QMessageBox::Yes);
        page.applySelected(true);
        QTRY_COMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::HighPerformance);
        QCOMPARE(store.find(probeguard::probePath())->appliedValue, QStringLiteral("GpuPreference=2;"));

        // Declining the confirmation changes nothing.
        page.selectMode(profiles::GpuMode::Automatic);
        answerNextDialog(QMessageBox::No);
        page.applySelected(true);
        QTest::qWait(300);
        QCOMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::HighPerformance);

        // Automatic: the value is removed.
        answerNextDialog(QMessageBox::Yes);
        page.applySelected(true);
        QTRY_VERIFY(!pref::findValueName(m_store, probeguard::probePath()).has_value());
        QCOMPARE(store.find(probeguard::probePath())->mode, profiles::GpuMode::Automatic);

        // Backup and restore through the page: export, lose the profile, import it back.
        const QString backup = dir.filePath(QStringLiteral("profiles-backup.json"));
        QVERIFY(page.exportTo(backup));
        QString error;
        QVERIFY(store.remove(store.find(probeguard::probePath())->id, &error));
        QVERIFY(page.importFrom(backup, false));
        QVERIFY(page.resultText().contains(QStringLiteral("1 profile(s) imported")));
        QCOMPARE(page.findChild<QListWidget*>()->count(), 1);
        QVERIFY(!store.find(probeguard::probePath())->lastVerification.has_value()); // results stay with the PC they were made on
    }

    // A program's own GPU option through the page: a user-defined known option for the probe
    // ("--adapter {luid}"), inserted with the button, SDK sharing switched off — the probe must land on
    // the integrated GPU purely through its launch arguments, with no Windows value written.
    void knownOptionThroughThePage()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        QFile options(QDir(data.configDir()).filePath(QStringLiteral("gpu-options.json")));
        QVERIFY(options.open(QIODevice::WriteOnly));
        options.write(R"([{"executables":["GPUFusionProbe.exe"],"arguments":"--adapter {luid}","program":"GPU Fusion probe","note":"test entry"}])");
        options.close();

        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        bool integrated = false;
        for (const hw::Adapter& a : hardware.latest()->adapters)
            integrated |= a.dxgi && !a.isSoftware() && a.classification.value == hw::AdapterClass::Integrated;
        if (!integrated)
            QSKIP("Needs an integrated GPU");

        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), probeguard::probePath());
        profiles::ProfileStore store(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, nullptr, nullptr, &store);
        ui::GpuManagerPage page(context);
        page.resize(1000, 1000);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));

        QVERIFY(page.addProgram(probeguard::probePath(), QStringLiteral("Probe")));
        QVERIFY(page.knownOptionText().contains(QStringLiteral("--adapter {luid}")));
        page.setArguments(QStringLiteral("--render 6"));
        page.useKnownOption();
        QCOMPARE(page.findChild<QLineEdit*>()->text(), QStringLiteral("--adapter {luid} --render 6"));
        page.findChild<QCheckBox*>()->setChecked(false); // no SDK variables: the option alone must do it
        page.selectMode(profiles::GpuMode::Integrated);
        QVERIFY2(page.capabilityText().contains(QStringLiteral("own GPU option")), qPrintable(page.capabilityText()));
        page.applySelected(false);
        QVERIFY(!pref::findValueName(m_store, probeguard::probePath()).has_value());
        QCOMPARE(store.find(probeguard::probePath())->arguments, QStringLiteral("--adapter {luid} --render 6")); // placeholder kept
        QVERIFY(!store.find(probeguard::probePath())->shareGpuChoiceWithApp);

        QSignalSpy finished(&page, &ui::GpuManagerPage::verificationFinished);
        page.verifySelected(8, false);
        QVERIFY(finished.wait(60000));
        qInfo().noquote() << page.resultText();
        const auto saved = store.find(probeguard::probePath());
        QVERIFY2(saved->lastVerification && saved->lastVerification->confirmed, qPrintable(page.resultText()));
    }

    // The program is already running when "Verify on this PC" is used: the page asks it to close first
    // (policy Close stands in for the user's choice), then verifies a fresh instance.
    void runningProgramIsClosedBeforeVerify()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), probeguard::probePath());
        profiles::ProfileStore store(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, nullptr, nullptr, &store);
        ui::GpuManagerPage page(context);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(page.addProgram(probeguard::probePath(), QStringLiteral("Probe")));
        page.setArguments(QStringLiteral("--render 4"));
        page.selectMode(profiles::GpuMode::Automatic);
        page.applySelected(false);

        QProcess running; // an instance that is already running, with a window
        running.start(probeguard::probePath(), { QStringLiteral("--window"), QStringLiteral("60") });
        QVERIFY(running.waitForStarted(10000));
        QTest::qWait(800);
        page.setRunningPolicy(ui::GpuManagerPage::RunningPolicy::Close);
        QSignalSpy finished(&page, &ui::GpuManagerPage::verificationFinished);
        page.verifySelected(5, false);
        // (the page's event loop may already have processed the exit; waitForFinished is false then)
        QVERIFY(running.state() == QProcess::NotRunning || running.waitForFinished(15000));
        QVERIFY(running.readAllStandardOutput().contains("\"closedByRequest\":true"));
        QVERIFY(finished.wait(60000));
        qInfo().noquote() << page.resultText();
        QVERIFY(store.find(probeguard::probePath())->lastVerification.has_value());
    }

    void cleanupTestCase()
    {
        QString error;
        QVERIFY2(probeguard::restore(m_store, &error), qPrintable(error));
    }
};

QTEST_MAIN(TestGpuManagerUi)
#include "tst_gpumanager_ui.moc"
