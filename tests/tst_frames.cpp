// Frame-time measurement without tracing: the helper's JSON, statistics, the choice of the main stream,
// sessions on disk, before/after comparison and the report. The capture itself (which needs
// administrator rights) is checked in tst_frames_live.
#include "frames/FrameCapture.h"
#include "frames/FrameSession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace gf::frames;

namespace {

// Present times at a fixed interval, with optional stalls.
QList<double> paced(int frames, double intervalMs, int stallEvery = 0, double stallMs = 0)
{
    QList<double> t;
    double now = 5;
    for (int i = 0; i < frames; ++i) {
        t << now;
        now += intervalMs + (stallEvery > 0 && i > 0 && i % stallEvery == 0 ? stallMs : 0);
    }
    return t;
}

QJsonArray json(const QList<double>& v)
{
    QJsonArray a;
    for (double x : v)
        a << x;
    return a;
}

QJsonObject captureJson(quint32 pid, const QJsonArray& streams, bool ok = true)
{
    QJsonObject process{ { QStringLiteral("pid"), qint64(pid) }, { QStringLiteral("name"), QStringLiteral("game.exe") },
                         { QStringLiteral("path"), QStringLiteral("C:\\Games\\game.exe") }, { QStringLiteral("streams"), streams } };
    return { { QStringLiteral("tool"), QStringLiteral("GPUFusionFrameCapture") }, { QStringLiteral("ok"), ok },
             { QStringLiteral("elevated"), true }, { QStringLiteral("started"), QStringLiteral("2026-10-28T12:00:00Z") },
             { QStringLiteral("requestedSeconds"), 10 }, { QStringLiteral("wallMs"), 10000.0 }, { QStringLiteral("targets"), QJsonArray{ qint64(pid) } },
             { QStringLiteral("providers"), QJsonArray{ QJsonObject{ { QStringLiteral("name"), QStringLiteral("Microsoft-Windows-DXGI") }, { QStringLiteral("status"), 0 } },
                                                          QJsonObject{ { QStringLiteral("name"), QStringLiteral("Microsoft-Windows-D3D9") }, { QStringLiteral("status"), 1168 } } } },
             { QStringLiteral("events"), 2400 }, { QStringLiteral("eventsLost"), 0 }, { QStringLiteral("buffersLost"), 0 },
             { QStringLiteral("overhead"), QJsonObject{ { QStringLiteral("cpuMs"), 31.0 }, { QStringLiteral("cpuPercentOfAllCores"), 0.08 },
                                                        { QStringLiteral("peakWorkingSetMB"), 6.5 } } },
             { QStringLiteral("processes"), QJsonArray{ process } } };
}

QJsonObject stream(const QString& runtime, const QList<double>& t, const QString& swapChain = QStringLiteral("0x1A2B"))
{
    QJsonObject s{ { QStringLiteral("runtime"), runtime }, { QStringLiteral("swapChain"), swapChain }, { QStringLiteral("presentFlags"), 0 },
                   { QStringLiteral("occluded"), 0 }, { QStringLiteral("discarded"), 0 }, { QStringLiteral("t"), json(t) },
                   { QStringLiteral("inPresentMs"), QJsonArray{} } };
    s.insert(QStringLiteral("syncInterval"), runtime == QLatin1StringView("DXGI") ? QJsonValue(0) : QJsonValue(QJsonValue::Null));
    return s;
}

FrameSession session(const QString& id, const QJsonObject& capture, const QString& label = {})
{
    FrameSession s;
    s.id = id;
    s.label = label;
    s.created = QDateTime(QDate(2026, 10, 28), QTime(12, 0));
    s.pid = 4242;
    s.processName = QStringLiteral("game.exe");
    s.captureJson = capture;
    s.capture = Capture::fromJson(capture);
    return s;
}

} // namespace

class TestFrames : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // A steady 10 ms pace: 100 fps, every percentile 10 ms, no stutters.
    void steadyPace()
    {
        const FrameStats s = computeStats(paced(1001, 10.0));
        QCOMPARE(s.presents, 1001);
        QCOMPARE(s.frames, 1000);
        QVERIFY(qAbs(s.fps - 100.0) < 1e-6);
        QVERIFY(qAbs(s.medianMs - 10.0) < 1e-9 && qAbs(s.p99Ms - 10.0) < 1e-9 && qAbs(s.maxMs - 10.0) < 1e-9);
        QCOMPARE(s.longFrames, 0);
        QVERIFY(qAbs(s.low1Fps - 100.0) < 1e-6);
        QCOMPARE(s.perSecondMeanMs.size(), 10); // 10 whole seconds; the single frame after them is left out
        QVERIFY(computeStats({}).frames == 0 && computeStats({ 1.0 }).frames == 0);
    }

    // Known stutters: a 50 ms stall every 30 frames at 60 fps.
    void stuttersAreCounted()
    {
        const FrameStats s = computeStats(paced(601, 16.667, 30, 50));
        QCOMPARE(s.longFrames, 19);
        QVERIFY(qAbs(s.medianMs - 16.667) < 1e-6);
        QVERIFY(s.maxMs > 66 && s.maxMs < 67);
        QVERIFY(s.low1Fps < 16); // the slowest 1 % are the stalls
        QVERIFY(s.p95Ms < 17);
    }

    // The helper's JSON, including a provider that could not be enabled and the cost of the capture.
    void parsesTheHelperOutput()
    {
        const Capture c = Capture::fromJson(captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), paced(11, 10)) }));
        QVERIFY(c.ok);
        QVERIFY(c.elevated);
        QCOMPARE(c.processes.size(), 1);
        QCOMPARE(c.processes[0].name, QStringLiteral("game.exe"));
        QCOMPARE(c.processes[0].streams[0].t.size(), 11);
        QCOMPARE(c.processes[0].streams[0].syncInterval.value_or(-1), 0);
        QCOMPARE(c.providerProblems, QStringList{ QStringLiteral("Microsoft-Windows-D3D9 (error 1168)") });
        QCOMPARE(c.helperPeakWorkingSetMB, 6.5);
        QVERIFY(c.process(4242) && !c.process(1));

        QJsonObject denied{ { QStringLiteral("ok"), false }, { QStringLiteral("errorCode"), 5 }, { QStringLiteral("error"), QStringLiteral("denied") } };
        const Capture d = Capture::fromJson(denied);
        QVERIFY(!d.ok && d.accessDenied());
    }

    // A DXGI program's kernel presents (from its driver) do not replace its swap chain as the main
    // stream; a program with kernel presents only (OpenGL, Vulkan) is measured through them.
    void mainStreamPrefersTheRuntime()
    {
        const Capture c = Capture::fromJson(captureJson(4242, QJsonArray{ stream(QStringLiteral("Kernel"), paced(500, 5), QStringLiteral("0x0")),
                                                                         stream(QStringLiteral("DXGI"), paced(100, 10)) }));
        QCOMPARE(mainStream(c.processes[0])->runtime, QStringLiteral("DXGI"));
        const Capture gl = Capture::fromJson(captureJson(4242, QJsonArray{ stream(QStringLiteral("Kernel"), paced(500, 5), QStringLiteral("0x0")) }));
        QCOMPARE(mainStream(gl.processes[0])->runtime, QStringLiteral("Kernel"));
        QVERIFY(runtimeText(QStringLiteral("Kernel")).contains(QStringLiteral("OpenGL")));
    }

    // Saved and loaded unchanged; listed newest first.
    void sessionsRoundTrip()
    {
        QTemporaryDir dir;
        FrameSession a = session(QStringLiteral("frames-20261028-120000-000"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), paced(301, 10)) }),
                                 QStringLiteral("before"));
        a.gpu << GpuEstimate{ QStringLiteral("NVIDIA GeForce GT 730"), 0x808D, 9, 71.5, 93.0, QStringLiteral("3D"), QStringLiteral("Windows GPU performance counters") };
        a.fusionCpuPercent = 0.4;
        FrameSession b = a;
        b.id = QStringLiteral("frames-20261028-120500-000");
        b.label = QStringLiteral("after");
        QString error;
        QVERIFY2(saveSession(dir.path(), a, &error) && saveSession(dir.path(), b, &error), qPrintable(error));
        const QList<FrameSession> all = loadSessions(dir.path());
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].label, QStringLiteral("after"));
        QCOMPARE(all[1].gpu.size(), 1);
        QCOMPARE(all[1].gpu[0].luid, quint64(0x808D));
        QCOMPARE(all[1].gpu[0].maxPercent, 93.0);
        QCOMPARE(all[1].target()->streams[0].t.size(), 301);
        QVERIFY(!FrameSession::fromJson(QJsonObject{ { QStringLiteral("format"), 2 }, { QStringLiteral("id"), QStringLiteral("x") } }));
    }

    // Before/after with the significance rule on per-second means: a real change is reported, noise
    // and too-short captures are not.
    void beforeAfter()
    {
        auto withJitter = [](int frames, double ms, double jitter) {
            QList<double> t;
            double now = 0;
            for (int i = 0; i < frames; ++i) {
                t << now;
                now += ms + ((i * 7919) % 13 - 6) * jitter / 6.0;
            }
            return t;
        };
        const FrameSession before = session(QStringLiteral("frames-a"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), withJitter(1000, 20, 2)) }), QStringLiteral("before"));
        const FrameSession after = session(QStringLiteral("frames-b"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), withJitter(1600, 12.5, 2)) }), QStringLiteral("after"));
        const FrameComparison c = compareSessions(before, after);
        QVERIFY2(c.comparable, qPrintable(c.reason));
        QCOMPARE(c.difference.verdict, gf::bench::Difference::Verdict::Faster);
        QVERIFY2(c.text.startsWith(QStringLiteral("Faster after")), qPrintable(c.text));
        QCOMPARE(compareSessions(after, before).difference.verdict, gf::bench::Difference::Verdict::Slower);

        const FrameSession same = session(QStringLiteral("frames-c"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), withJitter(1000, 20, 2)) }));
        QCOMPARE(compareSessions(before, same).difference.verdict, gf::bench::Difference::Verdict::NoSignificantChange);

        const FrameSession shortOne = session(QStringLiteral("frames-d"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), paced(50, 10)) }));
        QVERIFY(!compareSessions(before, shortOne).comparable);
        FrameSession failed = before;
        failed.error = QStringLiteral("The administrator prompt was declined, so nothing was measured.");
        QVERIFY(!compareSessions(failed, after).comparable);
    }

    // Summary and report say what is measured, what is estimated and what is not measured.
    void reportSeparatesMeasuredFromEstimated()
    {
        FrameSession s = session(QStringLiteral("frames-e"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), paced(601, 16.667, 30, 50)) }), QStringLiteral("after"));
        s.gpu << GpuEstimate{ QStringLiteral("NVIDIA GeForce GT 730"), 1, 9, 71.5, 93.0, QStringLiteral("3D"), QStringLiteral("Windows GPU performance counters") };
        const QStringList lines = summaryLines(s);
        QVERIFY2(lines[0].startsWith(QStringLiteral("Measured (Windows graphics events, DXGI")), qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral("19 long frames")), qPrintable(lines[0]));
        QVERIFY(lines.join(u'\n').contains(QStringLiteral("Estimated GPU use by the program")));
        QVERIFY(lines.join(u'\n').contains(QStringLiteral("Measurement cost: the capture helper used 0.08 %")));
        QVERIFY(!lines.join(u'\n').contains(QStringLiteral("%%")));
        const FrameSession before = session(QStringLiteral("frames-f"), captureJson(4242, QJsonArray{ stream(QStringLiteral("DXGI"), paced(601, 20)) }), QStringLiteral("before"));
        const QString html = reportHtml(s, &before);
        QVERIFY(html.contains(QStringLiteral("<h2>Before / after</h2>")));
        QVERIFY(html.contains(QStringLiteral("<b>Not measured</b>: when frames were actually shown")));
        QVERIFY(html.contains(QStringLiteral("Nothing was loaded into the program")));

        FrameSession none = session(QStringLiteral("frames-g"), captureJson(4242, QJsonArray{}));
        QVERIFY2(summaryLines(none)[0].contains(QStringLiteral("presented no frames")), qPrintable(summaryLines(none)[0]));
        FrameSession denied;
        denied.error = QStringLiteral("The administrator prompt was declined, so nothing was measured.");
        QCOMPARE(summaryLines(denied), QStringList{ denied.error });
    }
};

QTEST_GUILESS_MAIN(TestFrames)
#include "tst_frames.moc"
