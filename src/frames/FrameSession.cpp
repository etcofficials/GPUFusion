#include "frames/FrameSession.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace gf::frames {

namespace {

QString ms(double v) { return QString::number(v, 'f', 2); }

QJsonObject gpuJson(const GpuEstimate& g)
{
    return { { QStringLiteral("adapter"), g.adapter }, { QStringLiteral("luid"), QString::number(g.luid, 16) },
             { QStringLiteral("samples"), g.samples }, { QStringLiteral("meanPercent"), g.meanPercent },
             { QStringLiteral("maxPercent"), g.maxPercent }, { QStringLiteral("busiestEngine"), g.busiestEngine },
             { QStringLiteral("source"), g.source } };
}

QString escapeHtml(const QString& s) { return s.toHtmlEscaped(); }

} // namespace

QJsonObject FrameSession::toJson() const
{
    QJsonArray gpus;
    for (const GpuEstimate& g : gpu)
        gpus << gpuJson(g);
    return { { QStringLiteral("format"), 1 },
             { QStringLiteral("id"), id },
             { QStringLiteral("label"), label },
             { QStringLiteral("created"), created.toString(Qt::ISODateWithMs) },
             { QStringLiteral("pid"), qint64(pid) },
             { QStringLiteral("processName"), processName },
             { QStringLiteral("processPath"), processPath },
             { QStringLiteral("capture"), captureJson },
             { QStringLiteral("gpuEstimate"), gpus },
             { QStringLiteral("gpuNotes"), QJsonArray::fromStringList(gpuNotes) },
             { QStringLiteral("fusionCpuPercent"), fusionCpuPercent },
             { QStringLiteral("error"), error } };
}

std::optional<FrameSession> FrameSession::fromJson(const QJsonObject& o)
{
    if (o.value(QLatin1StringView("format")).toInt() != 1 || o.value(QLatin1StringView("id")).toString().isEmpty())
        return std::nullopt;
    FrameSession s;
    s.id = o.value(QLatin1StringView("id")).toString();
    s.label = o.value(QLatin1StringView("label")).toString();
    s.created = QDateTime::fromString(o.value(QLatin1StringView("created")).toString(), Qt::ISODateWithMs);
    s.pid = quint32(o.value(QLatin1StringView("pid")).toDouble());
    s.processName = o.value(QLatin1StringView("processName")).toString();
    s.processPath = o.value(QLatin1StringView("processPath")).toString();
    s.captureJson = o.value(QLatin1StringView("capture")).toObject();
    s.capture = Capture::fromJson(s.captureJson);
    for (const QJsonValue& v : o.value(QLatin1StringView("gpuEstimate")).toArray()) {
        const QJsonObject g = v.toObject();
        GpuEstimate e;
        e.adapter = g.value(QLatin1StringView("adapter")).toString();
        e.luid = g.value(QLatin1StringView("luid")).toString().toULongLong(nullptr, 16);
        e.samples = g.value(QLatin1StringView("samples")).toInt();
        e.meanPercent = g.value(QLatin1StringView("meanPercent")).toDouble();
        e.maxPercent = g.value(QLatin1StringView("maxPercent")).toDouble();
        e.busiestEngine = g.value(QLatin1StringView("busiestEngine")).toString();
        e.source = g.value(QLatin1StringView("source")).toString();
        s.gpu << e;
    }
    for (const QJsonValue& v : o.value(QLatin1StringView("gpuNotes")).toArray())
        s.gpuNotes << v.toString();
    s.fusionCpuPercent = o.value(QLatin1StringView("fusionCpuPercent")).toDouble(-1);
    s.error = o.value(QLatin1StringView("error")).toString();
    return s;
}

QString newSessionId()
{
    return QStringLiteral("frames-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
}

bool saveSession(const QString& dir, const FrameSession& s, QString* error)
{
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("Could not create %1").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    QSaveFile f(QDir(dir).filePath(s.id + QStringLiteral(".json")));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(s.toJson()).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

std::optional<FrameSession> loadSession(const QString& file, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!d.isObject()) {
        if (error)
            *error = pe.errorString();
        return std::nullopt;
    }
    auto s = FrameSession::fromJson(d.object());
    if (!s && error)
        *error = QStringLiteral("Not a frame-time measurement of a known format.");
    return s;
}

QList<FrameSession> loadSessions(const QString& dir)
{
    QList<FrameSession> out;
    const QStringList files = QDir(dir).entryList({ QStringLiteral("frames-*.json") }, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QString& name : files) {
        if (auto s = loadSession(QDir(dir).filePath(name)))
            out << *s;
    }
    return out;
}

FrameComparison compareSessions(const FrameSession& before, const FrameSession& after)
{
    FrameComparison c;
    const CapturedProcess* b = before.target();
    const CapturedProcess* a = after.target();
    const FrameStream* bs = b ? mainStream(*b) : nullptr;
    const FrameStream* as = a ? mainStream(*a) : nullptr;
    if (!before.measured() || !after.measured() || !bs || !as) {
        c.reason = QStringLiteral("Both measurements must have recorded frames of the program.");
        return c;
    }
    c.before = computeStats(bs->t);
    c.after = computeStats(as->t);
    if (c.before.perSecondMeanMs.size() < 2 || c.after.perSecondMeanMs.size() < 2) {
        c.reason = QStringLiteral("Each measurement needs at least two whole seconds of frames.");
        return c;
    }
    const bench::Stats sb = bench::computeStats(c.before.perSecondMeanMs);
    const bench::Stats sa = bench::computeStats(c.after.perSecondMeanMs);
    c.difference = bench::compareStats(sa, sb);
    c.comparable = true;
    const QString what = QStringLiteral("mean frame time %1 ms → %2 ms (%3 → %4 fps)").arg(ms(sb.mean), ms(sa.mean))
                             .arg(sb.mean > 0 ? 1000.0 / sb.mean : 0, 0, 'f', 1).arg(sa.mean > 0 ? 1000.0 / sa.mean : 0, 0, 'f', 1);
    switch (c.difference.verdict) {
    case bench::Difference::Verdict::Faster:
        c.text = QStringLiteral("Faster after: %1, %2× the frame rate.").arg(what).arg(c.difference.speedup, 0, 'f', 2);
        break;
    case bench::Difference::Verdict::Slower:
        c.text = QStringLiteral("Slower after: %1.").arg(what);
        break;
    case bench::Difference::Verdict::NoSignificantChange:
        c.text = QStringLiteral("No significant difference: %1.").arg(what);
        break;
    }
    if (before.processName.compare(after.processName, Qt::CaseInsensitive) != 0)
        c.text += QStringLiteral(" Note: the two measurements are of different programs (%1, %2).").arg(before.processName, after.processName);
    return c;
}

QStringList summaryLines(const FrameSession& s)
{
    QStringList out;
    if (!s.measured()) {
        out << (s.error.isEmpty() ? s.capture.error : s.error);
        return out;
    }
    const CapturedProcess* p = s.target();
    const FrameStream* m = p ? mainStream(*p) : nullptr;
    if (!m || m->t.size() < 2) {
        out << QStringLiteral("%1 presented no frames during the %2 s capture: it may not be rendering (minimized, paused, or not a "
                              "graphics program), or it presents in a way Windows' graphics events do not show.")
                   .arg(s.processName).arg(s.capture.wallMs / 1000.0, 0, 'f', 1);
    } else {
        const FrameStats st = computeStats(m->t);
        out << QStringLiteral("Measured (Windows graphics events, %1): %2 frames in %3 s — %4 fps; frame time median %5 ms, 95th percentile %6 ms, "
                              "99th %7 ms, longest %8 ms; 1 % low %9 fps; %10 long frames (stutters).")
                   .arg(runtimeText(m->runtime)).arg(st.frames).arg(st.spanMs / 1000.0, 0, 'f', 1).arg(st.fps, 0, 'f', 1)
                   .arg(ms(st.medianMs), ms(st.p95Ms), ms(st.p99Ms), ms(st.maxMs)).arg(st.low1Fps, 0, 'f', 1).arg(st.longFrames);
        if (m->syncInterval)
            out << QStringLiteral("Sync interval %1 (%2).").arg(*m->syncInterval).arg(*m->syncInterval == 0 ? QStringLiteral("no vertical sync")
                                                                                                             : QStringLiteral("waits for the display"));
        if (m->occluded > 0)
            out << QStringLiteral("%1 of its presents were of a hidden window (counted, but not shown).").arg(m->occluded);
        int swapChains = 0;
        bool kernelToo = false;
        for (const FrameStream& fs : p->streams) {
            swapChains += fs.runtime == QLatin1StringView("Kernel") ? 0 : 1;
            kernelToo = kernelToo || fs.runtime == QLatin1StringView("Kernel");
        }
        if (swapChains > 1)
            out << QStringLiteral("It presented through %1 swap chains; the one shown has the most presents (%2).").arg(swapChains).arg(m->t.size());
        if (kernelToo && m->runtime != QLatin1StringView("Kernel"))
            out << QStringLiteral("Its presents also appear as kernel-level presents (the same frames, seen at the driver level); they are not counted twice.");
    }
    if (s.gpu.isEmpty()) {
        out << QStringLiteral("GPU utilization: not available for this process%1").arg(s.gpuNotes.isEmpty() ? QStringLiteral(".") : QStringLiteral(" (%1).").arg(s.gpuNotes.join(QStringLiteral("; "))));
    } else {
        QStringList parts;
        for (const GpuEstimate& g : s.gpu)
            parts << QStringLiteral("%1 %2 % mean, %3 % max (%4)").arg(g.adapter).arg(g.meanPercent, 0, 'f', 0).arg(g.maxPercent, 0, 'f', 0)
                         .arg(g.busiestEngine.isEmpty() ? g.source : g.busiestEngine + QStringLiteral(", ") + g.source);
        out << QStringLiteral("Estimated GPU use by the program (busiest engine, sampled once a second): %1.").arg(parts.join(QStringLiteral("; ")));
    }
    out << QStringLiteral("Measurement cost: the capture helper used %1 % of all CPU cores and %2 MB of memory%3; %4 events received, %5 lost.")
               .arg(s.capture.helperCpuPercent, 0, 'f', 2).arg(s.capture.helperPeakWorkingSetMB, 0, 'f', 1)
               .arg(s.fusionCpuPercent >= 0 ? QStringLiteral(", GPU Fusion %1 %").arg(s.fusionCpuPercent, 0, 'f', 2) : QString())
               .arg(s.capture.events).arg(s.capture.eventsLost);
    if (s.capture.eventsLost > 0 || s.capture.buffersLost > 0)
        out << QStringLiteral("Some events were lost, so some frames may be missing and the frame times around them too long.");
    if (!s.capture.providerProblems.isEmpty())
        out << QStringLiteral("Not enabled: %1.").arg(s.capture.providerProblems.join(QStringLiteral(", ")));
    return out;
}

QString reportHtml(const FrameSession& s, const FrameSession* before)
{
    QString h;
    h += QStringLiteral("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                        "<title>Frame times — %1</title><style>body{font:15px/1.5 system-ui,Segoe UI,sans-serif;max-width:900px;margin:2em auto;padding:0 16px;"
                        "color:#1b1f24;background:#fff}h1{font-size:1.4em}table{border-collapse:collapse;margin:1em 0}td,th{border:1px solid #ccd;padding:4px 8px;"
                        "text-align:left}th{background:#eef1f5}.m{color:#555}.ok{color:#0a7d32}.bad{color:#b3261e}"
                        "@media(prefers-color-scheme:dark){body{background:#16181c;color:#e6e8eb}th{background:#23272e}td,th{border-color:#3a3f47}.m{color:#aab}}"
                        "</style></head><body>")
             .arg(escapeHtml(s.processName));
    h += QStringLiteral("<h1>Frame times: %1</h1><p class=\"m\">%2 · process %3 · %4%5</p>")
             .arg(escapeHtml(s.processName), s.created.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(s.pid)
             .arg(escapeHtml(QDir::toNativeSeparators(s.processPath)), s.label.isEmpty() ? QString() : QStringLiteral(" · label “%1”").arg(escapeHtml(s.label)));
    if (before) {
        const FrameComparison c = compareSessions(*before, s);
        h += QStringLiteral("<h2>Before / after</h2><p class=\"%1\"><b>%2</b></p>")
                 .arg(c.comparable && c.difference.verdict == bench::Difference::Verdict::Faster ? QStringLiteral("ok")
                      : c.comparable && c.difference.verdict == bench::Difference::Verdict::Slower ? QStringLiteral("bad") : QStringLiteral("m"),
                      escapeHtml(c.comparable ? c.text : c.reason));
        if (c.comparable) {
            h += QStringLiteral("<table><tr><th></th><th>Before (%1)</th><th>After (%2)</th></tr>")
                     .arg(escapeHtml(before->label.isEmpty() ? before->created.toString(QStringLiteral("HH:mm:ss")) : before->label),
                          escapeHtml(s.label.isEmpty() ? s.created.toString(QStringLiteral("HH:mm:ss")) : s.label));
            auto row = [&](const QString& name, double x, double y, int d = 2) {
                h += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>").arg(name, QString::number(x, 'f', d), QString::number(y, 'f', d));
            };
            row(QStringLiteral("Frames per second"), c.before.fps, c.after.fps, 1);
            row(QStringLiteral("Frame time, median (ms)"), c.before.medianMs, c.after.medianMs);
            row(QStringLiteral("Frame time, 95th percentile (ms)"), c.before.p95Ms, c.after.p95Ms);
            row(QStringLiteral("Frame time, 99th percentile (ms)"), c.before.p99Ms, c.after.p99Ms);
            row(QStringLiteral("1 % low (fps)"), c.before.low1Fps, c.after.low1Fps, 1);
            row(QStringLiteral("Long frames (stutters)"), c.before.longFrames, c.after.longFrames, 0);
            h += QStringLiteral("</table><p class=\"m\">Compared: the mean frame time of each whole second (%1 and %2 seconds). “Faster” or "
                                "“slower” only when the difference is more than twice the combined standard error and at least 1 %.</p>")
                     .arg(c.before.perSecondMeanMs.size()).arg(c.after.perSecondMeanMs.size());
        }
    }
    h += QStringLiteral("<h2>This measurement</h2><ul>");
    for (const QString& line : summaryLines(s))
        h += QStringLiteral("<li>%1</li>").arg(escapeHtml(line));
    h += QStringLiteral("</ul>");
    if (const CapturedProcess* p = s.target(); p && !p->streams.isEmpty()) {
        h += QStringLiteral("<h2>Streams</h2><table><tr><th>API</th><th>Swap chain</th><th>Presents</th><th>fps</th><th>Median ms</th><th>99th ms</th>"
                            "<th>Time in present, median ms</th></tr>");
        for (const FrameStream& fs : p->streams) {
            const FrameStats st = computeStats(fs.t);
            QList<double> inPresent = fs.inPresentMs;
            std::sort(inPresent.begin(), inPresent.end());
            h += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td><td>%7</td></tr>")
                     .arg(escapeHtml(runtimeText(fs.runtime)), escapeHtml(fs.swapChain)).arg(fs.t.size()).arg(st.fps, 0, 'f', 1)
                     .arg(ms(st.medianMs), ms(st.p99Ms), inPresent.isEmpty() ? QStringLiteral("not measured") : ms(inPresent[inPresent.size() / 2]));
        }
        h += QStringLiteral("</table>");
    }
    h += QStringLiteral("<h2>What is and is not measured</h2><ul>"
                        "<li><b>Measured</b>: the time the program called Present (frame-to-frame time) for DXGI (Direct3D 10/11/12) and Direct3D 9 swap "
                        "chains, and kernel-level presents for OpenGL, Vulkan and other APIs; time spent inside the Present call (DXGI, Direct3D 9).</li>"
                        "<li><b>Estimated</b>: the program's GPU use, from Windows performance counters or D3DKMT statistics sampled once a second "
                        "(busiest engine per GPU).</li>"
                        "<li><b>Not measured</b>: when frames were actually shown on the display, dropped frames, input latency, GPU time per frame.</li>"
                        "<li>Nothing was loaded into the program and nothing about it was changed.</li></ul>");
    h += QStringLiteral("<p class=\"m\">GPU Fusion frame-time measurement, format 1.</p></body></html>");
    return h;
}

} // namespace gf::frames
