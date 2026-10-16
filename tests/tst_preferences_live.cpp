// Live preference tests against the real per-user registry key — restricted to the value for this
// build's GPUFusionProbe.exe, which is restored afterwards (and checked). They answer the Phase 5
// question with evidence: what does each Windows GPU preference actually do on this PC?
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/RegistryPreferenceStore.h"
#include "preferences/PreferenceModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace gf::pref;

class TestPreferencesLive : public QObject {
    Q_OBJECT

    QString m_probe;
    QTemporaryDir m_dir;
    std::optional<QString> m_originalBefore;
    win::RegistryPreferenceStore m_store;

private Q_SLOTS:
    // The probe's original value is saved to a file before anything changes. If this process dies
    // mid-test, the separate "restore" fixture (restoreFromSnapshot, run by CTest afterwards) puts it back.
    static QString snapshotPath()
    {
        return QDir(QDir::tempPath()).filePath(QStringLiteral("gpufusion-preferences-live-snapshot.json"));
    }
    static QString probePath()
    {
        return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionProbe.exe")));
    }

    void initTestCase()
    {
        if (QCoreApplication::arguments().contains(QStringLiteral("restoreFromSnapshot")))
            return; // cleanup-only run
        m_probe = probePath();
        if (!QFileInfo::exists(m_probe))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        QVERIFY(m_dir.isValid());
        if (QFileInfo::exists(snapshotPath()))
            restoreFromSnapshot(); // a previous run died: put its value back first
        const auto name = findValueName(m_store, m_probe);
        m_originalBefore = name ? m_store.read(*name) : std::nullopt;
        QFile f(snapshotPath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonObject{ { "name", name.value_or(m_probe) },
                                           { "original", m_originalBefore ? QJsonValue(*m_originalBefore) : QJsonValue(QJsonValue::Null) } }).toJson());
    }

    void probeRunsAndReportsAnAdapter()
    {
        QString error;
        const auto out = win::runProbe(m_probe, &error);
        QVERIFY2(out, qPrintable(error));
        const auto report = parseProbeReport(*out, &error);
        QVERIFY2(report, qPrintable(error));
        QVERIFY2(report->error.isEmpty(), qPrintable(report->error));
        QVERIFY(report->d3d11Default.has_value());
        QVERIFY(!report->enumOrder.isEmpty());
        qInfo().noquote() << "probe default device:" << report->d3d11Default->name;
    }

    void applyAndRollbackOnTheRealRegistry()
    {
        ChangeJournal journal(m_dir.filePath(QStringLiteral("journal.json")));
        PreferenceManager manager(m_store, journal);
        const ChangeResult r = manager.apply(m_probe, GpuPreference::HighPerformance);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(manager.current(m_probe), GpuPreference::HighPerformance);
        const ChangeResult undo = manager.rollback(r.record->id);
        QVERIFY2(undo.ok, qPrintable(undo.error));
        const auto name = findValueName(m_store, m_probe);
        QCOMPARE(name ? m_store.read(*name) : std::nullopt, m_originalBefore);
    }

    void verificationOnThisPc()
    {
        ChangeJournal journal(m_dir.filePath(QStringLiteral("journal2.json")));
        const VerificationResult v = verifyPreferences(m_store, journal, m_probe,
                                                       [](const QString& p, QString* e) { return win::runProbe(p, e); });
        for (const VerificationStep& s : v.steps) {
            qInfo().noquote() << gpuPreferenceText(s.preference).leftJustified(20) << "->"
                              << (s.ok ? QStringLiteral("default device on %1; enumeration starts with %2; minimum-power query: %3")
                                             .arg(s.report.d3d11Default->name, s.report.enumOrder.value(0).name,
                                                  s.report.minimumPower ? s.report.minimumPower->name : QStringLiteral("n/a"))
                                       : s.error);
            QVERIFY2(s.ok, qPrintable(s.error));
        }
        QVERIFY2(v.restored, qPrintable(v.restoreError));
        QVERIFY(!journal.pending().has_value());
        const auto name = findValueName(m_store, m_probe);
        QCOMPARE(name ? m_store.read(*name) : std::nullopt, m_originalBefore); // left exactly as found
        qInfo().noquote() << "Preference has an effect on this PC:" << (v.preferenceHasNoEffect() ? "NO" : "YES");

        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1) {
            // Phase 0 predicted it from EnumAdapterByGpuPreference; this checks it with a launched program.
            QVERIFY(v.preferenceHasNoEffect());
            QCOMPARE(v.steps[2].report.d3d11Default->vendorId, 0x10DEu); // "Power saving" still lands on the GT 730
        }
    }

    // Also run on its own by CTest after this suite (FIXTURES_CLEANUP), so a crash cannot leave the
    // probe's preference changed.
    void restoreFromSnapshot()
    {
        QFile f(snapshotPath());
        if (!f.open(QIODevice::ReadOnly))
            return; // nothing pending
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        const QString name = o.value(QStringLiteral("name")).toString();
        QVERIFY(name.endsWith(QStringLiteral("GPUFusionProbe.exe"), Qt::CaseInsensitive)); // never touch anything else
        const QJsonValue original = o.value(QStringLiteral("original"));
        QString error;
        QVERIFY2(original.isString() ? m_store.write(name, original.toString(), &error) : m_store.remove(name, &error), qPrintable(error));
        const auto now = m_store.read(name);
        QCOMPARE(now.has_value(), original.isString());
        if (original.isString())
            QCOMPARE(*now, original.toString());
        QFile::remove(snapshotPath());
    }
};

QTEST_GUILESS_MAIN(TestPreferencesLive)
#include "tst_preferences_live.moc"
