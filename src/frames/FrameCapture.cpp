#include "frames/FrameCapture.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace gf::frames {

int CapturedProcess::frames() const
{
    int n = 0;
    for (const FrameStream& s : streams)
        n += int(s.t.size());
    return n;
}

namespace {
QList<double> doubles(const QJsonArray& a)
{
    QList<double> out;
    out.reserve(a.size());
    for (const QJsonValue& v : a)
        out << v.toDouble();
    return out;
}
} // namespace

Capture Capture::fromJson(const QJsonObject& o)
{
    Capture c;
    c.ok = o.value(QLatin1StringView("ok")).toBool();
    c.error = o.value(QLatin1StringView("error")).toString();
    c.errorCode = qint64(o.value(QLatin1StringView("errorCode")).toDouble());
    c.elevated = o.value(QLatin1StringView("elevated")).toBool();
    c.started = QDateTime::fromString(o.value(QLatin1StringView("started")).toString(), Qt::ISODate);
    c.requestedSeconds = o.value(QLatin1StringView("requestedSeconds")).toDouble();
    c.wallMs = o.value(QLatin1StringView("wallMs")).toDouble();
    c.endReason = o.value(QLatin1StringView("endReason")).toString();
    for (const QJsonValue& v : o.value(QLatin1StringView("targets")).toArray())
        c.targets << quint32(v.toDouble());
    for (const QJsonValue& v : o.value(QLatin1StringView("providers")).toArray()) {
        const QJsonObject p = v.toObject();
        if (p.value(QLatin1StringView("status")).toInt() != 0)
            c.providerProblems << QStringLiteral("%1 (error %2)").arg(p.value(QLatin1StringView("name")).toString()).arg(p.value(QLatin1StringView("status")).toInt());
    }
    c.events = quint64(o.value(QLatin1StringView("events")).toDouble());
    c.eventsLost = quint64(o.value(QLatin1StringView("eventsLost")).toDouble());
    c.buffersLost = quint64(o.value(QLatin1StringView("buffersLost")).toDouble());
    c.framesNotStored = quint64(o.value(QLatin1StringView("framesNotStored")).toDouble());
    const QJsonObject overhead = o.value(QLatin1StringView("overhead")).toObject();
    c.helperCpuMs = overhead.value(QLatin1StringView("cpuMs")).toDouble();
    c.helperCpuPercent = overhead.value(QLatin1StringView("cpuPercentOfAllCores")).toDouble();
    c.helperPeakWorkingSetMB = overhead.value(QLatin1StringView("peakWorkingSetMB")).toDouble();
    for (const QJsonValue& pv : o.value(QLatin1StringView("processes")).toArray()) {
        const QJsonObject po = pv.toObject();
        CapturedProcess p;
        p.pid = quint32(po.value(QLatin1StringView("pid")).toDouble());
        p.name = po.value(QLatin1StringView("name")).toString();
        p.path = po.value(QLatin1StringView("path")).toString();
        for (const QJsonValue& sv : po.value(QLatin1StringView("streams")).toArray()) {
            const QJsonObject so = sv.toObject();
            FrameStream s;
            s.runtime = so.value(QLatin1StringView("runtime")).toString();
            s.swapChain = so.value(QLatin1StringView("swapChain")).toString();
            if (so.value(QLatin1StringView("syncInterval")).isDouble())
                s.syncInterval = so.value(QLatin1StringView("syncInterval")).toInt();
            s.presentFlags = quint32(so.value(QLatin1StringView("presentFlags")).toDouble());
            s.occluded = so.value(QLatin1StringView("occluded")).toInt();
            s.discarded = so.value(QLatin1StringView("discarded")).toInt();
            s.t = doubles(so.value(QLatin1StringView("t")).toArray());
            s.inPresentMs = doubles(so.value(QLatin1StringView("inPresentMs")).toArray());
            p.streams << s;
        }
        c.processes << p;
    }
    return c;
}

const CapturedProcess* Capture::process(quint32 pid) const
{
    for (const CapturedProcess& p : processes)
        if (p.pid == pid)
            return &p;
    return nullptr;
}

QString runtimeText(const QString& runtime)
{
    if (runtime == QLatin1StringView("DXGI"))
        return QStringLiteral("DXGI (Direct3D 10, 11 or 12)");
    if (runtime == QLatin1StringView("D3D9"))
        return QStringLiteral("Direct3D 9");
    if (runtime == QLatin1StringView("Kernel"))
        return QStringLiteral("kernel presents (OpenGL, Vulkan or another API without its own events)");
    return runtime;
}

const FrameStream* mainStream(const CapturedProcess& p)
{
    const FrameStream* best = nullptr;
    auto rank = [](const FrameStream& s) { return s.runtime == QLatin1StringView("Kernel") ? 0 : 1; };
    for (const FrameStream& s : p.streams) {
        if (!best || rank(s) > rank(*best) || (rank(s) == rank(*best) && s.t.size() > best->t.size()))
            best = &s;
    }
    return best;
}

FrameStats computeStats(const QList<double>& t)
{
    FrameStats s;
    s.presents = int(t.size());
    if (t.size() < 2)
        return s;
    QList<double> ft;
    ft.reserve(t.size() - 1);
    for (qsizetype i = 1; i < t.size(); ++i)
        ft << t[i] - t[i - 1];
    s.frames = int(ft.size());
    s.spanMs = t.last() - t.first();
    s.fps = s.spanMs > 0 ? 1000.0 * s.frames / s.spanMs : 0;
    double sum = 0;
    for (double x : ft)
        sum += x;
    s.meanMs = sum / ft.size();
    double var = 0;
    for (double x : ft)
        var += (x - s.meanMs) * (x - s.meanMs);
    s.sdMs = ft.size() > 1 ? std::sqrt(var / (ft.size() - 1)) : 0;
    QList<double> sorted = ft;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted[std::min<qsizetype>(sorted.size() - 1, qsizetype(p * (sorted.size() - 1) + 0.5))]; };
    s.medianMs = pct(0.5);
    s.p95Ms = pct(0.95);
    s.p99Ms = pct(0.99);
    s.maxMs = sorted.last();
    const qsizetype worst = std::max<qsizetype>(1, sorted.size() / 100);
    double worstSum = 0;
    for (qsizetype i = sorted.size() - worst; i < sorted.size(); ++i)
        worstSum += sorted[i];
    s.low1Fps = worstSum > 0 ? 1000.0 * worst / worstSum : 0;
    for (double x : ft)
        s.longFrames += x > 2 * s.medianMs && x > s.medianMs + 4 ? 1 : 0;
    // Mean frame time per whole second (frames assigned by their end time).
    const double origin = t.first();
    double windowSum = 0;
    int windowCount = 0, window = 0;
    for (qsizetype i = 1; i < t.size(); ++i) {
        const int w = int((t[i] - origin) / 1000.0);
        if (w != window) {
            if (windowCount > 0)
                s.perSecondMeanMs << windowSum / windowCount;
            window = w;
            windowSum = 0;
            windowCount = 0;
        }
        windowSum += t[i] - t[i - 1];
        ++windowCount;
    }
    // The last, partial second is left out unless it is the only one.
    if (s.perSecondMeanMs.isEmpty() && windowCount > 0)
        s.perSecondMeanMs << windowSum / windowCount;
    return s;
}

} // namespace gf::frames
