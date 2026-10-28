// The frame-time capture helper on this PC. Never raises the administrator prompt:
//   - not elevated (the normal case): the helper must refuse cleanly — "access denied", nothing
//     measured, a readable reason — and the parsed result must say so;
//   - elevated: each graphics API of GPUFusionFrameTest (Direct3D 11, 12, 9, OpenGL) is captured from
//     outside and must match the program's own present times.
#include "frames/FrameCapture.h"
#include "frames/FrameMeasure.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace gf::frames;

namespace {
QString bin(const QString& exe) { return QDir(QCoreApplication::applicationDirPath()).filePath(exe); }

Capture runCapture(const QStringList& extra, double seconds, int* exitCode, const QString& dir)
{
    const QString out = QDir(dir).filePath(QStringLiteral("capture.json"));
    QProcess p;
    p.start(bin(QStringLiteral("GPUFusionFrameCapture.exe")), QStringList{ QStringLiteral("--out"), out, QStringLiteral("--seconds"), QString::number(seconds) } + extra);
    p.waitForFinished(int(seconds * 1000) + 30000);
    *exitCode = p.exitCode();
    QFile f(out);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return Capture::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}
} // namespace

class TestFramesLive : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void helperRefusesCleanlyWithoutRights()
    {
        if (isElevated())
            QSKIP("Running with administrator rights: the capture is allowed (see apisMatchGroundTruth).");
        QTemporaryDir dir;
        int code = 0;
        const Capture c = runCapture({}, 2, &code, dir.path());
        QCOMPARE(code, 1);
        QVERIFY(!c.ok);
        QVERIFY(c.accessDenied());
        QVERIFY(!c.elevated);
        QVERIFY2(c.error.contains(QStringLiteral("administrator rights")) && c.error.contains(QStringLiteral("Nothing was measured")), qPrintable(c.error));
        QVERIFY(c.processes.isEmpty());
    }

    void badArgumentsAreRejected()
    {
        QProcess p;
        p.start(bin(QStringLiteral("GPUFusionFrameCapture.exe")), { QStringLiteral("--out"), QStringLiteral("result.txt") }); // must be .json
        QVERIFY(p.waitForFinished(10000));
        QCOMPARE(p.exitCode(), 2);
    }

    void apisMatchGroundTruth_data()
    {
        QTest::addColumn<QString>("api");
        QTest::addColumn<QString>("runtime");
        QTest::newRow("d3d11") << QStringLiteral("d3d11") << QStringLiteral("DXGI");
        QTest::newRow("d3d12") << QStringLiteral("d3d12") << QStringLiteral("DXGI");
        QTest::newRow("d3d9") << QStringLiteral("d3d9") << QStringLiteral("D3D9");
        QTest::newRow("opengl") << QStringLiteral("opengl") << QStringLiteral("Kernel");
    }
    void apisMatchGroundTruth()
    {
        QFETCH(QString, api);
        QFETCH(QString, runtime);
        if (!isElevated())
            QSKIP("Needs administrator rights (event tracing); the test never asks for them. Run the suite elevated to check capture accuracy.");
        QProcess test;
        test.start(bin(QStringLiteral("GPUFusionFrameTest.exe")), { QStringLiteral("--api"), api, QStringLiteral("--interval-ms"), QStringLiteral("10"),
                                                                   QStringLiteral("--seconds"), QStringLiteral("8") });
        QVERIFY(test.waitForStarted());
        QTest::qWait(1000);
        QTemporaryDir dir;
        int code = 0;
        const Capture c = runCapture({ QStringLiteral("--pid"), QString::number(test.processId()) }, 5, &code, dir.path());
        QVERIFY(test.waitForFinished(20000));
        const QJsonObject truth = QJsonDocument::fromJson(test.readAllStandardOutput()).object();
        if (!truth.value(QLatin1StringView("ok")).toBool())
            QSKIP(qPrintable(QStringLiteral("%1 is not available here: %2").arg(api, truth.value(QLatin1StringView("error")).toString())));
        QCOMPARE(code, 0);
        QVERIFY2(c.ok, qPrintable(c.error));
        const CapturedProcess* p = c.process(quint32(test.processId()));
        QVERIFY2(p, "no presents captured for the test program");
        const FrameStream* m = mainStream(*p);
        QCOMPARE(m->runtime, runtime);
        const FrameStats s = computeStats(m->t);
        // 5 s at 10 ms: about 500 presents; the median frame time equals the program's own.
        QVERIFY2(s.presents >= 470 && s.presents <= 510, qPrintable(QString::number(s.presents)));
        QVERIFY2(qAbs(s.medianMs - truth.value(QLatin1StringView("frameTimeMs")).toObject().value(QLatin1StringView("median")).toDouble()) < 0.5,
                 qPrintable(QString::number(s.medianMs)));
        QCOMPARE(c.eventsLost, quint64(0));
    }
};

QTEST_GUILESS_MAIN(TestFramesLive)
#include "tst_frames_live.moc"
