// Live checks of running-instance handling with GPUFusionProbe as the program (never another
// program): instances are found by path; a close request closes a program with a window; a program
// that refuses, or has no window, is reported as still running and is not ended by GPU Fusion.
#include "profiles/RunningInstances.h"

#include "ProbePreferenceGuard.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTest>
#include <QThread>

using namespace gf;
using namespace gf::profiles;

namespace {

bool waitFor(const std::function<bool()>& condition, int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (condition())
            return true;
        QThread::msleep(100);
    }
    return condition();
}

} // namespace

class TestRunningLive : public QObject {
    Q_OBJECT
    QProcess m_probe; // started by the test, so the test can always end it

    bool start(const QStringList& args)
    {
        m_probe.start(probeguard::probePath(), args);
        return m_probe.waitForStarted(10000);
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        if (!runningInstances(probeguard::probePath()).isEmpty())
            QSKIP("A probe is already running (another test?)");
    }

    void cleanup()
    {
        if (m_probe.state() != QProcess::NotRunning) {
            m_probe.kill();
            m_probe.waitForFinished(5000);
        }
    }

    void programWithAWindowIsClosed()
    {
        QVERIFY(start({ QStringLiteral("--window"), QStringLiteral("60") }));
        QVERIFY(waitFor([] { return runningInstances(probeguard::probePath()).size() == 1; }, 5000));
        QThread::msleep(500); // let it create its window
        const CloseOutcome r = closeGracefully(probeguard::probePath(), 10000);
        qInfo().noquote() << r.message;
        QVERIFY2(r.ok(), qPrintable(r.message));
        QCOMPARE(r.instances, 1);
        QVERIFY(r.windowsAsked >= 1);
        QVERIFY(m_probe.waitForFinished(5000));
        QVERIFY(m_probe.readAllStandardOutput().contains("\"closedByRequest\":true")); // it closed itself, on request
        QVERIFY(runningInstances(probeguard::probePath()).isEmpty());
        QVERIFY(closeGracefully(probeguard::probePath()).message.contains(QStringLiteral("not running")));
    }

    void programThatRefusesIsNotForced()
    {
        QVERIFY(start({ QStringLiteral("--window"), QStringLiteral("60"), QStringLiteral("--ignore-close") }));
        QVERIFY(waitFor([] { return !runningInstances(probeguard::probePath()).isEmpty(); }, 5000));
        QThread::msleep(500);
        const CloseOutcome r = closeGracefully(probeguard::probePath(), 2000);
        qInfo().noquote() << r.message;
        QVERIFY(!r.ok());
        QCOMPARE(r.remaining.size(), 1);
        QVERIFY(r.message.contains(QStringLiteral("does not end programs by force")));
        QCOMPARE(m_probe.state(), QProcess::Running); // still running: GPU Fusion did not end it
    }

    void programWithoutAWindowIsReported()
    {
        QVERIFY(start({ QStringLiteral("--render"), QStringLiteral("30") }));
        QVERIFY(waitFor([] { return !runningInstances(probeguard::probePath()).isEmpty(); }, 5000));
        const CloseOutcome r = closeGracefully(probeguard::probePath(), 1500);
        qInfo().noquote() << r.message;
        QVERIFY(!r.ok());
        QCOMPARE(r.windowsAsked, 0);
        QVERIFY(r.message.contains(QStringLiteral("without a window")));
        QCOMPARE(m_probe.state(), QProcess::Running);
    }
};

QTEST_GUILESS_MAIN(TestRunningLive)
#include "tst_running_live.moc"
