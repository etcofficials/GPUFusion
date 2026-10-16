#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using gf::log::Level;
using gf::log::RotatingFileLog;

class TestLogging : public QObject {
    Q_OBJECT

    static QByteArray read(const QString& file)
    {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private Q_SLOTS:
    void levelNamesRoundTrip()
    {
        for (Level l : { Level::Debug, Level::Info, Level::Warning, Level::Error }) {
            Level parsed = Level::Info;
            QVERIFY(gf::log::parseLevel(gf::log::levelName(l).toUpper(), &parsed));
            QCOMPARE(parsed, l);
        }
        Level untouched = Level::Warning;
        QVERIFY(!gf::log::parseLevel(QStringLiteral("verbose"), &untouched));
        QCOMPARE(untouched, Level::Warning);
    }

    void lineHasTimestampSeverityAndSubsystem()
    {
        const QDateTime t(QDate(2026, 10, 16), QTime(12, 34, 56, 789));
        const QString line = RotatingFileLog::formatLine(t, Level::Warning, QStringLiteral("hardware"), 42, QStringLiteral("a\nb"));
        QVERIFY2(line.startsWith(QStringLiteral("2026-10-16T12:34:56.789")), qPrintable(line));
        QVERIFY(line.contains(QStringLiteral(" WARN  [hardware] (t42) a\n    b")));
        QVERIFY(line.endsWith(u'\n'));
    }

    void rotationBoundsFileCountAndSize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RotatingFileLog log;
        QString error;
        QVERIFY2(log.open({ dir.path(), QStringLiteral("t"), 4096, 3 }, &error), qPrintable(error));

        const QString payload(200, u'x');
        for (int i = 0; i < 200; ++i) // ~45 KB in total, far beyond 3 x 4 KB
            log.write(QDateTime::currentDateTime(), Level::Info, QStringLiteral("test"), QStringLiteral("%1 %2").arg(i).arg(payload));
        log.close();

        const QStringList files = QDir(dir.path()).entryList({ QStringLiteral("t*.log") }, QDir::Files, QDir::Name);
        QCOMPARE(files, (QStringList{ QStringLiteral("t.1.log"), QStringLiteral("t.2.log"), QStringLiteral("t.log") }));
        for (const QString& f : files)
            QVERIFY2(QFileInfo(dir.filePath(f)).size() <= 4096, qPrintable(f));
        // Newest entry is in the active file; the oldest entries were discarded.
        QVERIFY(read(dir.filePath(QStringLiteral("t.log"))).contains(") 199 x"));
        QVERIFY(!read(dir.filePath(QStringLiteral("t.2.log"))).isEmpty());
        for (const QString& f : files)
            QVERIFY2(!read(dir.filePath(f)).contains(") 0 x"), qPrintable(f));
    }

    void reopenAppendsToExistingLog()
    {
        QTemporaryDir dir;
        QString error;
        {
            RotatingFileLog log;
            QVERIFY(log.open({ dir.path(), QStringLiteral("a"), 1 << 20, 2 }, &error));
            log.write(QDateTime::currentDateTime(), Level::Info, QStringLiteral("s"), QStringLiteral("first"));
        }
        RotatingFileLog log;
        QVERIFY(log.open({ dir.path(), QStringLiteral("a"), 1 << 20, 2 }, &error));
        log.write(QDateTime::currentDateTime(), Level::Info, QStringLiteral("s"), QStringLiteral("second"));
        log.close();
        const QByteArray text = read(dir.filePath(QStringLiteral("a.log")));
        QVERIFY(text.contains("first"));
        QVERIFY(text.contains("second"));
    }

    void processHandlerFiltersByLevelAndTagsSubsystem()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY2(gf::log::initialize({ dir.path() }, Level::Warning, &error), qPrintable(error));
        qCInfo(lcSettings) << "hidden-info";
        qCWarning(lcSettings) << "visible-warning";
        gf::log::setMinimumLevel(Level::Debug);
        qCDebug(lcApp) << "visible-debug";
        const QString file = gf::log::activeLogFile();
        gf::log::shutdown();

        const QByteArray text = read(file);
        QVERIFY(!text.contains("hidden-info"));
        QVERIFY(text.contains("WARN  [settings]"));
        QVERIFY(text.contains("visible-warning"));
        QVERIFY(text.contains("DEBUG [app]"));
    }
};

QTEST_GUILESS_MAIN(TestLogging)
#include "tst_logging.moc"
