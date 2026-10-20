// Drives the real GPU Preferences page against the real registry, using only GPUFusionProbe.exe as
// the "application": select it, choose Power saving, Apply (confirm), check Windows' value, Undo
// from the history, check the original is back. Confirmation dialogs are answered by the test.
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "platform/windows/RegistryPreferenceStore.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/pages/PreferencesPage.h"

#include "ProbePreferenceGuard.h"

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QJsonDocument>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace gf;

namespace {

// Answers the next modal message box with `button` once it appears.
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

template <typename T>
T* findByText(QWidget* root, const QString& text)
{
    for (T* w : root->findChildren<T*>()) {
        if (w->text() == text)
            return w;
    }
    return nullptr;
}

} // namespace

class TestPreferencesUi : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    pref::win::RegistryPreferenceStore m_store;
    std::optional<QString> m_original;

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        QVERIFY(m_dir.isValid());
        m_original = probeguard::take(m_store);
        if (m_original)
            QSKIP("The probe already has a preference set; this test only runs from a clean state");
    }

    void applyAndUndoThroughThePage()
    {
        DataLocation data = resolveDataLocation(m_dir.path(), m_dir.filePath(QStringLiteral("data")), m_dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        hw::HardwareService hardware;
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), probeguard::probePath());

        // Give the probe an initial preference so it appears in the page's list (journalled -> undoable too).
        QVERIFY(prefs.manager().apply(probeguard::probePath(), pref::GpuPreference::HighPerformance).ok);

        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs);
        ui::PreferencesPage page(context);
        page.resize(1000, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));

        auto* list = page.findChild<QListWidget*>();
        QVERIFY(list);
        int row = -1;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString().compare(probeguard::probePath(), Qt::CaseInsensitive) == 0)
                row = i;
        }
        QVERIFY2(row >= 0, "probe not listed");
        list->setCurrentRow(row);
        QVERIFY(findByText<QRadioButton>(&page, QStringLiteral("High performance"))->isChecked()); // editor shows the stored value

        findByText<QRadioButton>(&page, QStringLiteral("Power saving"))->click();
        answerNextDialog(QMessageBox::Yes);
        findByText<QPushButton>(&page, QStringLiteral("Apply…"))->click();
        QTRY_COMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::PowerSaving);
        QCOMPARE(m_store.read(*pref::findValueName(m_store, probeguard::probePath())).value(), QStringLiteral("GpuPreference=1;"));
        const auto notes = page.findChildren<QLabel*>();
        QVERIFY(std::any_of(notes.begin(), notes.end(), [](QLabel* l) { return l->text().startsWith(QStringLiteral("Saved and read back: GpuPreference=1;")); }));

        // Cancelling a change changes nothing.
        findByText<QRadioButton>(&page, QStringLiteral("Let Windows decide"))->click();
        answerNextDialog(QMessageBox::No);
        findByText<QPushButton>(&page, QStringLiteral("Apply…"))->click();
        QTest::qWait(300);
        QCOMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::PowerSaving);

        // Undo the page's change from the history (newest row first), then the initial one.
        auto* history = page.findChildren<QTableWidget*>().last();
        auto* undo = qobject_cast<QPushButton*>(history->cellWidget(0, 3));
        QVERIFY(undo);
        undo->click();
        QTRY_COMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::HighPerformance);
        QCOMPARE(history->item(1, 3)->text(), QStringLiteral("Undone")); // the newest row is now the undo record
        auto* undoFirst = qobject_cast<QPushButton*>(history->cellWidget(history->rowCount() - 1, 3));
        QVERIFY(undoFirst);
        undoFirst->click();
        QTRY_VERIFY(!pref::findValueName(m_store, probeguard::probePath()).has_value()); // back to "not set"
    }

    // Adapter-named choices follow the verification: with a verification in which every preference put
    // the probe on the discrete GPU (as measured on the reference PC), "Integrated" must not write
    // anything, "Discrete" writes High performance and reads it back, "Both" writes nothing.
    void adapterChoicesFollowTheVerification()
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

        pref::VerificationResult v; // "the preference makes no difference": always the discrete GPU
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
        QVERIFY(prefs.lastVerification().has_value());
        QVERIFY(prefs.manager().apply(probeguard::probePath(), pref::GpuPreference::LetWindowsDecide).ok);
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs);
        ui::PreferencesPage page(context);
        page.resize(1100, 1000);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto* list = page.findChild<QListWidget*>();
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString().compare(probeguard::probePath(), Qt::CaseInsensitive) == 0)
                list->setCurrentRow(i);
        }
        auto byPrefix = [&](const QString& prefix) -> QRadioButton* {
            for (QRadioButton* r : page.findChildren<QRadioButton*>()) {
                if (r->text().startsWith(prefix))
                    return r;
            }
            return nullptr;
        };
        QPushButton* apply = findByText<QPushButton>(&page, QStringLiteral("Apply…"));
        QVERIFY(apply);

        QRadioButton* intel = byPrefix(QStringLiteral("Integrated GPU: "));
        QVERIFY(intel && intel->text().contains(QStringLiteral("cannot enforce")));
        intel->click();
        QVERIFY(!apply->isEnabled()); // nothing that would not do what it says is ever written

        QRadioButton* both = byPrefix(QStringLiteral("Both GPUs"));
        QVERIFY(both);
        both->click();
        QVERIFY(!apply->isEnabled());
        const auto labels = page.findChildren<QLabel*>();
        QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel* l) { return l->text().contains(QStringLiteral("One rendering workload split")); }));

        QRadioButton* nvidia = byPrefix(QStringLiteral("Discrete GPU: "));
        QVERIFY(nvidia && nvidia->text().contains(QStringLiteral("Verified")));
        nvidia->click();
        QVERIFY(apply->isEnabled());
        answerNextDialog(QMessageBox::Yes);
        apply->click();
        QTRY_COMPARE(prefs.manager().current(probeguard::probePath()), pref::GpuPreference::HighPerformance);
        const auto after = page.findChildren<QLabel*>();
        QVERIFY(std::any_of(after.begin(), after.end(), [](QLabel* l) { return l->text().startsWith(QStringLiteral("Saved and read back: GpuPreference=2;")); }));
        QVERIFY(nvidia->isChecked()); // the stored value is shown as the adapter it means here

        QString error;
        QVERIFY2(m_store.remove(*pref::findValueName(m_store, probeguard::probePath()), &error), qPrintable(error));
    }

    void cleanupTestCase()
    {
        QString error;
        QVERIFY2(probeguard::restore(m_store, &error), qPrintable(error));
    }
};

QTEST_MAIN(TestPreferencesUi)
#include "tst_preferences_ui.moc"
