#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using gf::Settings;
using gf::SettingsStore;

class TestSettings : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString path(const char* name = "settings.json") const { return m_dir.filePath(QLatin1StringView(name)); }

    static void writeFile(const QString& file, const QByteArray& bytes)
    {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
    }

private Q_SLOTS:
    void init() { QVERIFY(m_dir.isValid()); QFile::remove(path()); }

    void defaultsAreValid()
    {
        QVERIFY(gf::validate(Settings{}).isEmpty());
    }

    void jsonRoundTripPreservesEveryField()
    {
        Settings s;
        s.refreshIntervalMs = 500;
        s.historySeconds = 900;
        s.startMinimized = true;
        s.startMonitoringOnLaunch = false;
        s.useNvml = false;
        s.theme = QStringLiteral("light");
        s.logLevel = QStringLiteral("debug");
        s.logMaxFileKB = 2048;
        s.logMaxFiles = 3;
        s.benchmarkWarmupRuns = 2;
        s.benchmarkMeasuredRuns = 10;
        s.benchmarkRetentionDays = 0;
        s.profileBackupFolder = QStringLiteral("D:\\Backups\\GPU Fusion");
        s.profileBackupsKept = 25;
        s.windowGeometry = QByteArray("\x01\x02geometry", 10);

        QStringList warnings;
        const Settings back = gf::fromJson(gf::toJson(s), &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCOMPARE(back, s);
    }

    // Automatic profile backup: off by default; only a full path is accepted.
    void profileBackupSettings()
    {
        QCOMPARE(Settings{}.profileBackupFolder, QString());
        QStringList warnings;
        Settings s = gf::fromJson(QJsonDocument::fromJson(R"({"schemaVersion":3,"profiles":{"backupFolder":"relative\\dir","backupsKept":500}})")
                                      .object(), &warnings);
        QCOMPARE(s.profileBackupFolder, QString());            // relative path refused, backup stays off
        QCOMPARE(s.profileBackupsKept, 10);                     // out of range -> default
        QCOMPARE(warnings.size(), 2);
        s.profileBackupFolder = QStringLiteral("backups");
        QVERIFY(gf::validate(s).join(u' ').contains(QStringLiteral("full path")));
        // Older settings files (schema 2) have no "profiles" section: defaults, no warning.
        warnings.clear();
        gf::fromJson(QJsonDocument::fromJson(R"({"schemaVersion":2,"monitoring":{"useNvml":true}})").object(), &warnings);
        QVERIFY(warnings.isEmpty());
    }

    void missingFileGivesDefaults()
    {
        SettingsStore store(path());
        const auto r = store.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::Missing);
        QCOMPARE(r.settings, Settings{});
        QVERIFY(!store.isReadOnly());
    }

    void settingsSurviveRestart()
    {
        Settings s;
        s.refreshIntervalMs = 2000;
        s.theme = QStringLiteral("light");
        {
            SettingsStore first(path());
            first.load();
            QString error;
            QVERIFY2(first.save(s, &error), qPrintable(error));
        }
        SettingsStore second(path()); // a new instance models an application restart
        const auto r = second.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::Loaded);
        QCOMPARE(r.settings, s);
    }

    void outOfRangeAndWrongTypesFallBackPerField()
    {
        writeFile(path(), R"({"schemaVersion":1,
            "monitoring":{"refreshIntervalMs":5,"historySeconds":"long"},
            "appearance":{"theme":"neon"},
            "logging":{"level":"INFO","maxFiles":3.5},
            "benchmarks":{"measuredRuns":1e300,"warmupRuns":3}})");
        SettingsStore store(path());
        const auto r = store.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::LoadedWithFixes);
        const Settings d;
        QCOMPARE(r.settings.refreshIntervalMs, d.refreshIntervalMs); // below minimum
        QCOMPARE(r.settings.historySeconds, d.historySeconds);       // wrong type
        QCOMPARE(r.settings.theme, d.theme);                         // unknown choice
        QCOMPARE(r.settings.logLevel, QStringLiteral("info"));       // case-insensitive accepted
        QCOMPARE(r.settings.logMaxFiles, d.logMaxFiles);             // not a whole number
        QCOMPARE(r.settings.benchmarkMeasuredRuns, d.benchmarkMeasuredRuns); // huge value
        QCOMPARE(r.settings.benchmarkWarmupRuns, 3);                 // valid value kept
        QCOMPARE(r.warnings.size(), 5);
        QVERIFY(gf::validate(r.settings).isEmpty());
    }

    void corruptFileIsMovedAsideNotOverwritten()
    {
        writeFile(path(), "{ this is not json");
        SettingsStore store(path());
        const auto r = store.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::Corrupt);
        QCOMPARE(r.settings, Settings{});
        QVERIFY(!r.backupPath.isEmpty());
        QVERIFY(QFile::exists(r.backupPath));
        QVERIFY(!QFile::exists(path()));
        QFile backup(r.backupPath);
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), QByteArray("{ this is not json")); // original content preserved
    }

    void nonObjectJsonIsCorrupt()
    {
        writeFile(path(), "[1,2,3]");
        SettingsStore store(path());
        QCOMPARE(store.load().status, SettingsStore::LoadStatus::Corrupt);
    }

    void newerSchemaIsReadOnly()
    {
        writeFile(path(), R"({"schemaVersion":99,"monitoring":{"refreshIntervalMs":750},"futureSection":{}})");
        SettingsStore store(path());
        const auto r = store.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::NewerVersion);
        QCOMPARE(r.settings.refreshIntervalMs, 750);
        QVERIFY(store.isReadOnly());
        QString error;
        QVERIFY(!store.save(Settings{}, &error));
        QVERIFY(!error.isEmpty());
        QFile f(path());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("futureSection")); // newer file untouched
    }

    void saveRejectsInvalidSettings()
    {
        SettingsStore store(path());
        store.load();
        Settings s;
        s.refreshIntervalMs = 10;
        s.theme = QStringLiteral("neon");
        QString error;
        QVERIFY(!store.save(s, &error));
        QVERIFY(error.contains(QStringLiteral("Refresh interval")));
        QVERIFY(error.contains(QStringLiteral("Theme")));
        QVERIFY(!QFile::exists(path()));
    }

    void schemaOneFileLoadsWithNewDefaults()
    {
        writeFile(path(), R"({"schemaVersion":1,"monitoring":{"refreshIntervalMs":500,"historySeconds":600}})");
        SettingsStore store(path());
        const auto r = store.load();
        QCOMPARE(r.status, SettingsStore::LoadStatus::Loaded);
        QVERIFY(r.warnings.isEmpty());
        QCOMPARE(r.settings.refreshIntervalMs, 500);
        QCOMPARE(r.settings.useNvml, true); // field added in schema 2
        QVERIFY(!store.isReadOnly());
        QString error;
        QVERIFY(store.save(r.settings, &error)); // upgrades the file to the current schema
        QCOMPARE(store.load().settings.refreshIntervalMs, 500);
    }

    void savedFileIsVersionedJson()
    {
        SettingsStore store(path());
        store.load();
        QString error;
        QVERIFY(store.save(Settings{}, &error));
        QFile f(path());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value(QStringLiteral("schemaVersion")).toInt(), Settings::kSchemaVersion);
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
