#include "monitoring/MetricModel.h"

#include <QHash>

#include <cmath>
#include <limits>

namespace gf::mon {

double MetricValue::valueOrNaN() const
{
    return state == State::Ok ? value : std::numeric_limits<double>::quiet_NaN();
}

QString stateName(MetricValue::State state)
{
    switch (state) {
    case MetricValue::State::Ok:          return QStringLiteral("OK");
    case MetricValue::State::Unsupported: return QStringLiteral("Unsupported");
    case MetricValue::State::Unavailable: return QStringLiteral("Unavailable");
    case MetricValue::State::Unknown:     return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString formatMetric(const MetricValue& v, Unit unit)
{
    if (!v.isOk())
        return stateName(v.state);
    switch (unit) {
    case Unit::Percent: return QStringLiteral("%1 %").arg(v.value < 10 && v.value > 0 ? QString::number(v.value, 'f', 1) : QString::number(qRound(v.value)));
    case Unit::Bytes:   return hw::formatBytes(quint64(std::max(0.0, v.value)));
    case Unit::Celsius: return QStringLiteral("%1 °C").arg(qRound(v.value));
    case Unit::Watts:   return QStringLiteral("%1 W").arg(v.value, 0, 'f', 1);
    case Unit::MHz:     return QStringLiteral("%1 MHz").arg(qRound(v.value));
    case Unit::PState:  return QStringLiteral("P%1").arg(qRound(v.value));
    }
    return QString::number(v.value);
}

MetricValue utilizationFromRunningTime(std::optional<quint64> previous, quint64 current, qint64 elapsed100ns)
{
    if (!previous)
        return MetricValue::unknown(QStringLiteral("Needs two samples"));
    if (current < *previous)
        return MetricValue::unknown(QStringLiteral("Engine counter restarted (driver reset?)"));
    if (elapsed100ns <= 0)
        return MetricValue::unknown(QStringLiteral("No time elapsed between samples"));
    const double busy = 100.0 * double(current - *previous) / double(elapsed100ns);
    return MetricValue::ok(std::clamp(busy, 0.0, 100.0), QStringLiteral("D3DKMT engine running time"));
}

MetricValue busiestEngine(const QList<MetricValue>& engines)
{
    if (engines.isEmpty())
        return MetricValue::unavailable(QStringLiteral("No engines reported"));
    std::optional<double> best;
    for (const MetricValue& e : engines) {
        if (e.isOk())
            best = std::max(best.value_or(0.0), e.value);
    }
    if (best)
        return MetricValue::ok(*best, QStringLiteral("Busiest engine (Task Manager's definition)"));
    // No usable engine: report the most informative state.
    for (auto s : { MetricValue::State::Unknown, MetricValue::State::Unavailable, MetricValue::State::Unsupported }) {
        for (const MetricValue& e : engines) {
            if (e.state == s)
                return e;
        }
    }
    return engines.first();
}

MemoryUsage splitSegments(const QList<SegmentUsage>& segments)
{
    MemoryUsage m;
    for (const SegmentUsage& s : segments)
        (s.aperture ? m.shared : m.dedicated) += s.bytesResident;
    return m;
}

QString engineTypeName(int engineType, int node)
{
    switch (engineType) { // DXGK_ENGINE_TYPE
    case 1: return QStringLiteral("3D");
    case 2: return QStringLiteral("Video Decode");
    case 3: return QStringLiteral("Video Encode");
    case 4: return QStringLiteral("Video Processing");
    case 5: return QStringLiteral("Scene Assembly");
    case 6: return QStringLiteral("Copy");
    case 7: return QStringLiteral("Overlay");
    case 8: return QStringLiteral("Crypto");
    case 9: return QStringLiteral("Video Codec");
    default: return QStringLiteral("Engine %1").arg(node);
    }
}

QStringList disambiguate(const QStringList& names)
{
    QHash<QString, int> total, seen;
    for (const QString& n : names)
        ++total[n];
    QStringList out;
    for (const QString& n : names)
        out << (total.value(n) > 1 ? QStringLiteral("%1 %2").arg(n).arg(++seen[n]) : n);
    return out;
}

bool isStale(qint64 lastSampleMs, qint64 nowMs, int intervalMs)
{
    return nowMs - lastSampleMs > std::max<qint64>(3LL * intervalMs, 3000);
}

int historyCapacity(int historySeconds, int intervalMs)
{
    if (intervalMs <= 0)
        return 2;
    const qint64 points = (qint64(historySeconds) * 1000 + intervalMs - 1) / intervalMs + 1;
    return int(std::clamp<qint64>(points, 2, 14401));
}

QList<MonitorTarget> targetsFrom(const hw::DetectionResult& detection)
{
    QList<MonitorTarget> targets;
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi)
            continue;
        MonitorTarget t;
        t.software = a.isSoftware();
        t.wddmVersion = a.dxgi->wddmVersion;
        t.luid = a.dxgi->luid;
        t.name = a.name();
        t.vendorId = a.dxgi->vendorId;
        t.deviceId = a.dxgi->deviceId;
        t.location = a.dxgi->location;
        t.dedicatedTotal = a.dxgi->dedicatedVideoMemory;
        t.sharedLimit = a.dxgi->sharedSystemMemory;
        targets << t;
    }
    return targets;
}

} // namespace gf::mon
