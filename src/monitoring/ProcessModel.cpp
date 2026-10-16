#include "monitoring/ProcessModel.h"

#include <QRegularExpression>

namespace gf::mon {

namespace {

quint64 luidFrom(const QString& high, const QString& low)
{
    return (quint64(high.toUInt(nullptr, 16)) << 32) | quint64(low.toUInt(nullptr, 16));
}

} // namespace

std::optional<PdhEngineInstance> parsePdhEngineInstance(const QString& name)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^pid_(\d+)_luid_0x([0-9a-f]{8})_0x([0-9a-f]{8})_phys_(\d+)_eng_(\d+)_engtype_(.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(name);
    if (!m.hasMatch())
        return std::nullopt;
    return PdhEngineInstance{ m.captured(1).toUInt(), luidFrom(m.captured(2), m.captured(3)), m.captured(4).toInt(),
                              m.captured(5).toInt(), m.captured(6) };
}

std::optional<PdhProcessInstance> parsePdhProcessInstance(const QString& name)
{
    static const QRegularExpression re(QStringLiteral(R"(^pid_(\d+)_luid_0x([0-9a-f]{8})_0x([0-9a-f]{8})_phys_(\d+)$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(name);
    if (!m.hasMatch())
        return std::nullopt;
    return PdhProcessInstance{ m.captured(1).toUInt(), luidFrom(m.captured(2), m.captured(3)), m.captured(4).toInt() };
}

Activity activityOf(const ProcessAdapterUsage& u)
{
    for (const EngineSample& e : u.engines) {
        if (e.utilization.isOk() && e.utilization.value >= kActiveThresholdPercent)
            return Activity::Active;
    }
    const bool memory = (u.dedicated.isOk() && u.dedicated.value > 0) || (u.shared.isOk() && u.shared.value > 0);
    return memory ? Activity::MemoryOnly : Activity::None;
}

QString activityText(Activity a)
{
    switch (a) {
    case Activity::Active:     return QStringLiteral("Active");
    case Activity::MemoryOnly: return QStringLiteral("Memory only");
    case Activity::None:       return QStringLiteral("None");
    }
    return {};
}

const ProcessAdapterUsage* ProcessSample::usageOn(quint64 luid) const
{
    for (const ProcessAdapterUsage& u : adapters) {
        if (u.luid == luid)
            return &u;
    }
    return nullptr;
}

QString pdhEngineDisplayName(const QString& type, int engine)
{
    static const QHash<QString, QString> names{
        { QStringLiteral("3d"), QStringLiteral("3D") },
        { QStringLiteral("videodecode"), QStringLiteral("Video Decode") },
        { QStringLiteral("videoencode"), QStringLiteral("Video Encode") },
        { QStringLiteral("videoprocessing"), QStringLiteral("Video Processing") },
        { QStringLiteral("copy"), QStringLiteral("Copy") },
        { QStringLiteral("legacyoverlay"), QStringLiteral("Overlay") },
        { QStringLiteral("overlay"), QStringLiteral("Overlay") },
        { QStringLiteral("crypto"), QStringLiteral("Crypto") },
    };
    if (type.isEmpty())
        return QStringLiteral("Engine %1").arg(engine);
    const auto it = names.constFind(type.toLower());
    return it != names.constEnd() ? *it : type; // e.g. "Compute_0" stays as the driver named it
}

QHash<quint32, QList<ProcessAdapterUsage>> aggregatePdh(const QList<PdhEngineReading>& engines,
                                                        const QList<PdhMemoryReading>& memory,
                                                        const QList<quint64>& adapterLuids)
{
    QHash<quint32, QHash<quint64, ProcessAdapterUsage>> byProcess;
    auto usage = [&](quint32 pid, quint64 luid) -> ProcessAdapterUsage& {
        ProcessAdapterUsage& u = byProcess[pid][luid];
        if (u.luid == 0) {
            u.luid = luid;
            u.source = QStringLiteral("Windows performance counters");
            u.utilization = MetricValue::unknown(QStringLiteral("Needs two samples"));
            u.dedicated = u.shared = MetricValue::unavailable(QStringLiteral("No memory counter for this process"));
        }
        return u;
    };
    for (const PdhEngineReading& r : engines) {
        if (!adapterLuids.contains(r.instance.luid))
            continue;
        ProcessAdapterUsage& u = usage(r.instance.pid, r.instance.luid);
        u.engines << EngineSample{ r.instance.engine, pdhEngineDisplayName(r.instance.engineType, r.instance.engine), r.utilization };
    }
    for (const PdhMemoryReading& r : memory) {
        if (!adapterLuids.contains(r.instance.luid))
            continue;
        ProcessAdapterUsage& u = usage(r.instance.pid, r.instance.luid);
        u.dedicated = r.dedicated;
        u.shared = r.shared;
    }

    QHash<quint32, QList<ProcessAdapterUsage>> out;
    for (auto p = byProcess.begin(); p != byProcess.end(); ++p) {
        for (ProcessAdapterUsage& u : *p) {
            std::sort(u.engines.begin(), u.engines.end(), [](const EngineSample& a, const EngineSample& b) { return a.node < b.node; });
            QStringList names;
            for (const EngineSample& e : u.engines)
                names << e.name;
            names = disambiguate(names); // "Video Decode 1", "Video Decode 2", ... as on the Dashboard
            for (int i = 0; i < u.engines.size(); ++i)
                u.engines[i].name = names[i];
            QList<MetricValue> values;
            for (const EngineSample& e : u.engines)
                values << e.utilization;
            if (!values.isEmpty())
                u.utilization = busiestEngine(values);
            else
                u.utilization = MetricValue::ok(0, QStringLiteral("No engine instances for this process"));
            double best = -1;
            for (const EngineSample& e : u.engines) {
                if (e.utilization.isOk() && e.utilization.value > best) {
                    best = e.utilization.value;
                    u.busiestEngine = e.name;
                }
            }
            if (u.utilization.isOk() && u.utilization.value < kActiveThresholdPercent)
                u.busiestEngine.clear();
            out[p.key()] << u;
        }
    }
    return out;
}

} // namespace gf::mon
