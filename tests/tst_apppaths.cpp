#include "core/AppPaths.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using gf::DataLocation;

class TestAppPaths : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString exeDir() const { return m_tmp.filePath(QStringLiteral("app")); }
    QString fallback() const { return m_tmp.filePath(QStringLiteral("fallback")); }

    void writePointer(const QByteArray& content)
    {
        QFile f(QDir(exeDir()).filePath(QLatin1StringView(gf::kDataDirPointerFile)));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content);
    }

private Q_SLOTS:
    void init()
    {
        QVERIFY(m_tmp.isValid());
        QDir(exeDir()).removeRecursively();
        QVERIFY(QDir().mkpath(exeDir()));
    }

    void defaultIsPortableFolderNextToExe()
    {
        const DataLocation loc = gf::resolveDataLocation(exeDir(), {}, fallback());
        QCOMPARE(loc.source, DataLocation::Source::Portable);
        QCOMPARE(loc.root, QDir::cleanPath(exeDir() + QStringLiteral("/data")));
        QVERIFY(loc.problem.isEmpty());
    }

    void pointerFileIsHonouredAndSkipsComments()
    {
        const QString target = m_tmp.filePath(QStringLiteral("devdata"));
        writePointer("# comment\n\n" + target.toUtf8() + "\n");
        const DataLocation loc = gf::resolveDataLocation(exeDir(), {}, fallback());
        QCOMPARE(loc.source, DataLocation::Source::PointerFile);
        QCOMPARE(loc.root, QDir::cleanPath(target));
    }

    void relativePointerResolvesAgainstExeDir()
    {
        writePointer("../shared-data\n");
        const DataLocation loc = gf::resolveDataLocation(exeDir(), {}, fallback());
        QCOMPARE(loc.root, QDir::cleanPath(m_tmp.filePath(QStringLiteral("shared-data"))));
    }

    void commandLineWinsOverPointerFile()
    {
        writePointer(m_tmp.filePath(QStringLiteral("devdata")).toUtf8());
        const QString cli = m_tmp.filePath(QStringLiteral("cli"));
        const DataLocation loc = gf::resolveDataLocation(exeDir(), cli, fallback());
        QCOMPARE(loc.source, DataLocation::Source::CommandLine);
        QCOMPARE(loc.root, QDir::cleanPath(cli));
    }

    void unusableLocationFallsBackWithReason()
    {
        // A regular file where the folder should be makes the location impossible to create.
        const QString blocker = m_tmp.filePath(QStringLiteral("blocker"));
        QFile f(blocker);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const DataLocation loc = gf::resolveDataLocation(exeDir(), blocker + QStringLiteral("/data"), fallback());
        QCOMPARE(loc.source, DataLocation::Source::Fallback);
        QCOMPARE(loc.root, QDir::cleanPath(fallback()));
        QVERIFY(!loc.problem.isEmpty());
    }

    void layoutCreatesStandardFolders()
    {
        const DataLocation loc = gf::resolveDataLocation(exeDir(), {}, fallback());
        QVERIFY(gf::ensureDataLayout(loc).isEmpty());
        for (const QString& d : { loc.logsDir(), loc.benchmarksDir(), loc.reportsDir(), loc.configDir(), loc.tempDir() })
            QVERIFY2(QDir(d).exists(), qPrintable(d));
        QVERIFY(loc.settingsFile().endsWith(QStringLiteral("config/settings.json")));
    }
};

QTEST_GUILESS_MAIN(TestAppPaths)
#include "tst_apppaths.moc"
