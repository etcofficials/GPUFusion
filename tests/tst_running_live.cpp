// Live checks of running-instance handling with GPUFusionProbe as the program (never another
// program): instances are found by path; a close request closes a program with a window; a program
// that refuses, or has no window, is reported as still running and is not ended by GPU Fusion.
#include "hardware/AdapterDetector.h"
#include "profiles/RunningInstances.h"

#include "ProbePreferenceGuard.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <windows.h>
#include <shellapi.h>

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

    // A GPU Fusion shortcut, opened like a double-click, starts the program through its profile: the
    // GPU placeholder in the profile's arguments arrives filled in for the integrated GPU.
    void shortcutStartsTheProgramThroughItsProfile()
    {
        const QString app = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusion.exe"));
        if (!QFileInfo::exists(app))
            QSKIP("GPUFusion.exe is not built next to the tests");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString data = QDir::toNativeSeparators(dir.filePath(QStringLiteral("data")));
        auto gpuFusion = [&](const QStringList& args) {
            QProcess p;
            p.start(app, QStringList{ QStringLiteral("--data-dir"), data } + args);
            return p.waitForFinished(60000) ? p.exitCode() : -1;
        };
        QCOMPARE(gpuFusion({ QStringLiteral("--apply-profile"), probeguard::probePath(), QStringLiteral("--mode"), QStringLiteral("integrated"),
                             QStringLiteral("--profile-args"), QStringLiteral("--adapter {luid} --window 30") }), 0);
        QCOMPARE(gpuFusion({ QStringLiteral("--create-shortcut"), probeguard::probePath(), QStringLiteral("--shortcut-dir"), dir.path() }), 0);
        const QString lnk = dir.filePath(QStringLiteral("GPUFusionProbe (Integrated GPU).lnk"));
        QVERIFY(QFileInfo::exists(lnk));

        // Open it as Explorer would.
        QVERIFY(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(lnk).utf16()),
                                                        nullptr, nullptr, SW_SHOWNORMAL)) > 32);
        QVERIFY(waitFor([] { return !runningInstances(probeguard::probePath()).isEmpty(); }, 20000));
        QProcess query; // the probe's command line, as Windows records it
        query.start(QStringLiteral("powershell"), { QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
            QStringLiteral("(Get-CimInstance Win32_Process -Filter \"Name='GPUFusionProbe.exe'\").CommandLine") });
        QVERIFY(query.waitForFinished(30000));
        const QString commandLine = QString::fromLocal8Bit(query.readAllStandardOutput());
        qInfo().noquote() << commandLine.trimmed();
        const CloseOutcome closed = closeGracefully(probeguard::probePath(), 10000);
        QVERIFY2(closed.ok(), qPrintable(closed.message));

        QString integratedLuid;
        for (const hw::Adapter& a : hw::detectAdapters(nullptr).adapters)
            if (a.dxgi && !a.isSoftware() && a.classification.value == hw::AdapterClass::Integrated)
                integratedLuid = hw::formatLuid(a.dxgi->luid);
        if (integratedLuid.isEmpty())
            QVERIFY(!commandLine.contains(QStringLiteral("--adapter"))); // no integrated GPU: the option is left out
        else
            QVERIFY2(commandLine.contains(QStringLiteral("--adapter ") + integratedLuid), qPrintable(commandLine));
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
