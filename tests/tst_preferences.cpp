// Pure-logic tests for GPU preferences: value editing, journal-before-write, rollback safety,
// verification (set -> probe -> restore) and crash recovery — against an in-memory registry.
#include "preferences/PreferenceModel.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <map>

using namespace gf::pref;

namespace {

class FakeStore : public IPreferenceStore {
public:
    std::map<QString, QString> values;
    bool failWrites = false;
    QString journalPath;          // if set, every write asserts the journal is already on disk
    int writes = 0;

    QStringList names() const override
    {
        QStringList n;
        for (const auto& [k, v] : values)
            n << k;
        return n;
    }
    std::optional<QString> read(const QString& name) const override
    {
        const auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<QString>(it->second);
    }
    bool write(const QString& name, const QString& data, QString* error) override
    {
        if (!checkJournal(error))
            return false;
        if (failWrites) {
            if (error) *error = QStringLiteral("access denied (simulated)");
            return false;
        }
        ++writes;
        values[name] = data;
        return true;
    }
    bool remove(const QString& name, QString* error) override
    {
        if (!checkJournal(error))
            return false;
        if (failWrites) {
            if (error) *error = QStringLiteral("access denied (simulated)");
            return false;
        }
        ++writes;
        values.erase(name);
        return true;
    }

private:
    bool checkJournal(QString* error) const
    {
        if (!journalPath.isEmpty() && !QFile::exists(journalPath)) {
            if (error) *error = QStringLiteral("journal not written before the registry change");
            return false;
        }
        return true;
    }
};

QByteArray probeJson(const QString& luid, const QString& name)
{
    return QStringLiteral(R"({"probeVersion":1,"exe":"C:\\p.exe","enumOrder":[{"name":"%2","vendorId":4318,"luid":"%1"}],)"
                          R"("byPreference":{"unspecified":{"name":"%2","vendorId":4318,"luid":"%1"},"minimumPower":{"name":"%2","vendorId":4318,"luid":"%1"},)"
                          R"("highPerformance":{"name":"%2","vendorId":4318,"luid":"%1"}},"d3d11Default":{"name":"%2","vendorId":4318,"luid":"%1"},"featureLevel":"11_0"})")
        .arg(luid, name).toUtf8();
}

} // namespace

class TestPreferences : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_exe;      // an existing .exe file path
    QString journalPath() const { return m_dir.filePath(QStringLiteral("journal.json")); }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_exe = QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("App.exe")));
        QFile f(m_exe);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    void init() { QFile::remove(journalPath()); }

    void editsOnlyTheGpuPreferenceKey()
    {
        QCOMPARE(withGpuPreference(QString(), GpuPreference::HighPerformance), QStringLiteral("GpuPreference=2;"));
        QCOMPARE(withGpuPreference(QStringLiteral("GpuPreference=2;"), GpuPreference::PowerSaving), QStringLiteral("GpuPreference=1;"));
        QCOMPARE(withGpuPreference(QStringLiteral("SwapEffectUpgradeEnable=1;"), GpuPreference::HighPerformance),
                 QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;"));
        QCOMPARE(withGpuPreference(QStringLiteral("GpuPreference=1;SwapEffectUpgradeEnable=1;"), GpuPreference::LetWindowsDecide),
                 QStringLiteral("GpuPreference=0;SwapEffectUpgradeEnable=1;")); // order kept
        QCOMPARE(withGpuPreference(QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;"), GpuPreference::NotSet),
                 QStringLiteral("SwapEffectUpgradeEnable=1;"));
        QCOMPARE(withGpuPreference(QStringLiteral("GpuPreference=2;"), GpuPreference::NotSet), QString()); // nothing left
        QCOMPARE(withGpuPreference(QStringLiteral("gpupreference=2;;"), GpuPreference::PowerSaving), QStringLiteral("GpuPreference=1;"));
        QCOMPARE(parseGpuPreference(withGpuPreference(QString(), GpuPreference::PowerSaving)), GpuPreference::PowerSaving);
    }

    void onlyExistingAbsoluteExecutablesAreConfigurable()
    {
        QString why;
        QVERIFY(isConfigurableExecutable(m_exe, &why));
        QVERIFY(!isConfigurableExecutable(QStringLiteral("App.exe"), &why));
        QVERIFY(why.contains(QStringLiteral("absolute")));
        QVERIFY(!isConfigurableExecutable(m_dir.filePath(QStringLiteral("missing.exe")), &why));
        QVERIFY(!isConfigurableExecutable(m_dir.filePath(QStringLiteral("journal.json")), &why));
        QVERIFY(!isConfigurableExecutable(QString(), &why));
    }

    void valueNamesMatchCaseInsensitively()
    {
        FakeStore store;
        store.values[m_exe.toUpper()] = QStringLiteral("GpuPreference=1;");
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);
        QCOMPARE(manager.current(m_exe), GpuPreference::PowerSaving);
        QVERIFY(manager.apply(m_exe, GpuPreference::HighPerformance).ok);
        QCOMPARE(store.values.size(), size_t(1)); // existing value updated, not duplicated
        QCOMPARE(store.values.begin()->first, m_exe.toUpper());
    }

    void applyJournalsBeforeWritingAndRollbackRestores()
    {
        FakeStore store;
        store.journalPath = journalPath(); // the fake fails any write made before the journal exists
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);

        const ChangeResult r = manager.apply(m_exe, GpuPreference::HighPerformance);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(store.read(m_exe).value(), QStringLiteral("GpuPreference=2;"));
        QVERIFY(r.record && !r.record->before.has_value());

        ChangeJournal reloaded(journalPath());
        QVERIFY(reloaded.load(nullptr));
        QCOMPARE(reloaded.records().size(), 1);
        QCOMPARE(reloaded.records().first().after.value(), QStringLiteral("GpuPreference=2;"));

        const ChangeResult undo = manager.rollback(r.record->id);
        QVERIFY2(undo.ok, qPrintable(undo.error));
        QVERIFY(!store.read(m_exe).has_value()); // was not set before -> removed again
        QVERIFY(journal.find(r.record->id)->rolledBack);
        QVERIFY(!manager.rollback(r.record->id).ok); // only once
    }

    void otherKeysSurviveApplyAndRollback()
    {
        FakeStore store;
        store.values[m_exe] = QStringLiteral("SwapEffectUpgradeEnable=1;");
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);
        const ChangeResult r = manager.apply(m_exe, GpuPreference::PowerSaving);
        QVERIFY(r.ok);
        QCOMPARE(store.values[m_exe], QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=1;"));
        QVERIFY(manager.apply(m_exe, GpuPreference::NotSet).ok);
        QCOMPARE(store.values[m_exe], QStringLiteral("SwapEffectUpgradeEnable=1;"));
        QVERIFY(manager.rollback(journal.records().last().id).ok);
        QCOMPARE(store.values[m_exe], QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=1;"));
    }

    void noOpChangesAreNotJournalled()
    {
        FakeStore store;
        store.values[m_exe] = QStringLiteral("GpuPreference=2;");
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);
        QVERIFY(manager.apply(m_exe, GpuPreference::HighPerformance).ok);
        QCOMPARE(store.writes, 0);
        QVERIFY(journal.records().isEmpty());
    }

    void rollbackNeverOverwritesANewerExternalChange()
    {
        FakeStore store;
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);
        const auto r = manager.apply(m_exe, GpuPreference::HighPerformance);
        store.values[m_exe] = QStringLiteral("GpuPreference=1;"); // changed in Windows Settings afterwards
        const ChangeResult refused = manager.rollback(r.record->id);
        QVERIFY(!refused.ok);
        QVERIFY(refused.error.contains(QStringLiteral("outside GPU Fusion")));
        QCOMPARE(store.values[m_exe], QStringLiteral("GpuPreference=1;"));
        QVERIFY(manager.rollback(r.record->id, /*force*/ true).ok);
        QVERIFY(!store.read(m_exe).has_value());
    }

    void failedWritesChangeNothingAndAreMarked()
    {
        FakeStore store;
        store.failWrites = true;
        ChangeJournal journal(journalPath());
        PreferenceManager manager(store, journal);
        const ChangeResult r = manager.apply(m_exe, GpuPreference::HighPerformance);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("simulated")));
        QVERIFY(store.values.empty());
        QCOMPARE(journal.records().size(), 1);
        QVERIFY(journal.records().first().rolledBack); // not offered for undo
        QVERIFY(journal.records().first().origin.contains(QStringLiteral("failed")));
    }

    void unwritableJournalBlocksTheChange()
    {
        // A file where the journal's folder should be makes the journal impossible to save.
        const QString blocker = m_dir.filePath(QStringLiteral("blocker"));
        QFile f(blocker);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        FakeStore store;
        ChangeJournal journal(blocker + QStringLiteral("/journal.json"));
        PreferenceManager manager(store, journal);
        const ChangeResult r = manager.apply(m_exe, GpuPreference::HighPerformance);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("nothing was changed")));
        QVERIFY(store.values.empty());
    }

    void journalIsBounded()
    {
        ChangeJournal journal(journalPath(), 3);
        for (int i = 0; i < 5; ++i) {
            ChangeRecord r;
            r.id = QString::number(i);
            r.valueName = m_exe;
            journal.append(r);
        }
        QCOMPARE(journal.records().size(), 3);
        QCOMPARE(journal.records().first().id, QStringLiteral("2"));
    }

    void parsesProbeOutput()
    {
        QString error;
        const auto r = parseProbeReport(probeJson(QStringLiteral("00000000_00007EDA"), QStringLiteral("NVIDIA GeForce GT 730")), &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->d3d11Default->luid, QStringLiteral("00000000_00007EDA"));
        QCOMPARE(r->minimumPower->name, QStringLiteral("NVIDIA GeForce GT 730"));
        QCOMPARE(r->enumOrder.size(), 1);
        QVERIFY(!parseProbeReport("not json", &error));
        const auto failed = parseProbeReport(R"({"probeVersion":1,"d3d11Error":"0x887A0004"})", &error);
        QVERIFY(failed && failed->error.contains(QStringLiteral("0x887A0004")));
    }

    void verificationSetsEachPreferenceThenRestoresTheOriginal()
    {
        for (const std::optional<QString>& original : { std::optional<QString>(), std::optional<QString>(QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;")) }) {
            FakeStore store;
            const QString probe = m_exe;
            if (original)
                store.values[probe] = *original;
            ChangeJournal journal(journalPath());
            QStringList seen; // the value in place each time the probe ran
            const auto run = [&](const QString&, QString*) -> std::optional<QByteArray> {
                seen << store.read(probe).value_or(QStringLiteral("<none>"));
                return probeJson(QStringLiteral("00000000_00007EDA"), QStringLiteral("GT 730"));
            };
            const VerificationResult v = verifyPreferences(store, journal, probe, run);
            QCOMPARE(v.steps.size(), 4);
            for (const VerificationStep& s : v.steps)
                QVERIFY2(s.ok, qPrintable(s.error));
            if (original) {
                QCOMPARE(seen, (QStringList{ QStringLiteral("SwapEffectUpgradeEnable=1;"), QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=0;"),
                                             QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=1;"), QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;") }));
            } else {
                QCOMPARE(seen, (QStringList{ QStringLiteral("<none>"), QStringLiteral("GpuPreference=0;"), QStringLiteral("GpuPreference=1;"),
                                             QStringLiteral("GpuPreference=2;") }));
            }
            QVERIFY(v.restored);
            QCOMPARE(store.read(probe), original);   // exactly as before
            QVERIFY(!journal.pending().has_value()); // marker cleared
            QVERIFY(v.preferenceHasNoEffect());      // every run landed on the same adapter
        }
    }

    void verificationDetectsAnEffect()
    {
        FakeStore store;
        ChangeJournal journal(journalPath());
        const auto run = [&](const QString& path, QString*) -> std::optional<QByteArray> {
            const bool power = store.read(path).value_or(QString()).contains(QStringLiteral("=1"));
            return power ? probeJson(QStringLiteral("00000000_0000896D"), QStringLiteral("Intel")) : probeJson(QStringLiteral("00000000_00007EDA"), QStringLiteral("NVIDIA"));
        };
        const VerificationResult v = verifyPreferences(store, journal, m_exe, run);
        QVERIFY(!v.preferenceHasNoEffect());
        QCOMPARE(v.steps[2].report.d3d11Default->name, QStringLiteral("Intel"));
    }

    void failedProbeRunsAreReportedNotGuessed()
    {
        FakeStore store;
        ChangeJournal journal(journalPath());
        const auto run = [](const QString&, QString* error) -> std::optional<QByteArray> {
            *error = QStringLiteral("timed out");
            return std::nullopt;
        };
        const VerificationResult v = verifyPreferences(store, journal, m_exe, run);
        for (const VerificationStep& s : v.steps) {
            QVERIFY(!s.ok);
            QVERIFY(s.error.contains(QStringLiteral("timed out")));
        }
        QVERIFY(!v.preferenceHasNoEffect()); // no conclusion from failed runs
        QVERIFY(v.restored);
    }

    void interruptedVerificationIsRecovered()
    {
        FakeStore store;
        store.values[m_exe] = QStringLiteral("GpuPreference=1;"); // left behind mid-run
        ChangeJournal journal(journalPath());
        journal.setPending(PendingVerification{ m_exe, std::nullopt }); // original: not set
        QVERIFY(journal.save(nullptr));

        ChangeJournal afterRestart(journalPath());
        QVERIFY(afterRestart.load(nullptr));
        QVERIFY(afterRestart.pending().has_value());
        QString error;
        QVERIFY(recoverPending(store, afterRestart, &error));
        QVERIFY(store.values.empty());
        QVERIFY(!afterRestart.pending().has_value());
        QVERIFY(!recoverPending(store, afterRestart, &error)); // nothing left to do
    }

    void failedRestoreKeepsThePendingMarker()
    {
        FakeStore store;
        ChangeJournal journal(journalPath());
        int runs = 0;
        const auto run = [&](const QString&, QString*) -> std::optional<QByteArray> {
            if (++runs == 4)
                store.failWrites = true; // the restore after the last step will fail
            return probeJson(QStringLiteral("00000000_00007EDA"), QStringLiteral("GT 730"));
        };
        const VerificationResult v = verifyPreferences(store, journal, m_exe, run);
        QVERIFY(!v.restored);
        QVERIFY(!v.restoreError.isEmpty());
        QVERIFY(journal.pending().has_value()); // retried at next start
        store.failWrites = false;
        QVERIFY(recoverPending(store, journal, nullptr));
        QVERIFY(store.values.empty());
    }

    void verificationResultRoundTrips()
    {
        FakeStore store;
        ChangeJournal journal(journalPath());
        const auto run = [](const QString&, QString*) -> std::optional<QByteArray> { return probeJson(QStringLiteral("00000000_00007EDA"), QStringLiteral("GT 730")); };
        const VerificationResult v = verifyPreferences(store, journal, m_exe, run);
        const auto back = VerificationResult::fromJson(QJsonDocument::fromJson(QJsonDocument(v.toJson()).toJson()).object());
        QVERIFY(back);
        QCOMPARE(back->steps.size(), 4);
        QCOMPARE(back->steps[2].preference, GpuPreference::PowerSaving);
        QCOMPARE(back->steps[2].report.d3d11Default->name, QStringLiteral("GT 730"));
        QVERIFY(back->restored);
        QVERIFY(back->preferenceHasNoEffect());
    }
};

QTEST_GUILESS_MAIN(TestPreferences)
#include "tst_preferences.moc"
