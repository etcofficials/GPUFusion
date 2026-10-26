#include "profiles/AppProfile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>

namespace gf::profiles {

using pref::Enforcement;
using pref::GpuChoice;
using pref::GpuPreference;

QString modeKey(GpuMode m)
{
    switch (m) {
    case GpuMode::Automatic:  return QStringLiteral("automatic");
    case GpuMode::Integrated: return QStringLiteral("integrated");
    case GpuMode::Discrete:   return QStringLiteral("discrete");
    case GpuMode::Both:       return QStringLiteral("both");
    }
    return {};
}

QString modeText(GpuMode m)
{
    switch (m) {
    case GpuMode::Automatic:  return QStringLiteral("Automatic");
    case GpuMode::Integrated: return QStringLiteral("Integrated GPU");
    case GpuMode::Discrete:   return QStringLiteral("Discrete GPU");
    case GpuMode::Both:       return QStringLiteral("Both GPUs");
    }
    return {};
}

std::optional<GpuMode> modeFromKey(const QString& key)
{
    for (GpuMode m : { GpuMode::Automatic, GpuMode::Integrated, GpuMode::Discrete, GpuMode::Both }) {
        if (modeKey(m) == key)
            return m;
    }
    return std::nullopt;
}

QString modeStatusText(ModeStatus s)
{
    switch (s) {
    case ModeStatus::Verified:        return QStringLiteral("Verified");
    case ModeStatus::Expected:        return QStringLiteral("Expected (not verified yet)");
    case ModeStatus::NeedsAppSupport: return QStringLiteral("Only with the program's support");
    case ModeStatus::NeedsUserAction: return QStringLiteral("Needs a change only you can make");
    case ModeStatus::Unsupported:     return QStringLiteral("Not possible on this PC");
    case ModeStatus::Unknown:         return QStringLiteral("Unknown");
    }
    return {};
}

// ---- JSON ----------------------------------------------------------------------------------------

QJsonObject ProfileVerification::toJson() const
{
    return QJsonObject{ { "time", time.toString(Qt::ISODateWithMs) }, { "mode", modeKey(mode) }, { "outcome", outcome },
                        { "headline", headline }, { "stages", QJsonArray::fromStringList(stages) },
                        { "renderingExecutables", QJsonArray::fromStringList(renderingExecutables) }, { "confirmed", confirmed } };
}

std::optional<ProfileVerification> ProfileVerification::fromJson(const QJsonObject& o)
{
    if (!o.contains(QLatin1StringView("outcome")))
        return std::nullopt;
    ProfileVerification v;
    v.time = QDateTime::fromString(o.value(QLatin1StringView("time")).toString(), Qt::ISODateWithMs);
    v.mode = modeFromKey(o.value(QLatin1StringView("mode")).toString()).value_or(GpuMode::Automatic);
    v.outcome = o.value(QLatin1StringView("outcome")).toString();
    v.headline = o.value(QLatin1StringView("headline")).toString();
    for (const QJsonValue& s : o.value(QLatin1StringView("stages")).toArray())
        v.stages << s.toString();
    for (const QJsonValue& s : o.value(QLatin1StringView("renderingExecutables")).toArray())
        v.renderingExecutables << s.toString();
    v.confirmed = o.value(QLatin1StringView("confirmed")).toBool();
    return v;
}

QJsonObject AppProfile::toJson() const
{
    QJsonObject o{ { "schemaVersion", 1 }, { "id", id }, { "name", name }, { "exePath", exePath }, { "arguments", arguments },
                   { "mode", modeKey(mode) }, { "shareGpuChoiceWithApp", shareGpuChoiceWithApp }, { "appliedValue", appliedValue },
                   { "created", created.toString(Qt::ISODateWithMs) }, { "modified", modified.toString(Qt::ISODateWithMs) } };
    if (lastVerification)
        o.insert(QStringLiteral("lastVerification"), lastVerification->toJson());
    return o;
}

std::optional<AppProfile> AppProfile::fromJson(const QJsonObject& o)
{
    const QString exe = o.value(QLatin1StringView("exePath")).toString();
    if (exe.isEmpty() || o.value(QLatin1StringView("schemaVersion")).toInt() > 1)
        return std::nullopt;
    AppProfile p;
    p.exePath = exe;
    p.id = profileIdFor(exe); // derived, so a hand-edited id cannot point at another program
    p.name = o.value(QLatin1StringView("name")).toString(QFileInfo(exe).completeBaseName());
    p.arguments = o.value(QLatin1StringView("arguments")).toString();
    p.mode = modeFromKey(o.value(QLatin1StringView("mode")).toString()).value_or(GpuMode::Automatic);
    p.shareGpuChoiceWithApp = o.value(QLatin1StringView("shareGpuChoiceWithApp")).toBool(true);
    p.appliedValue = o.value(QLatin1StringView("appliedValue")).toString();
    p.created = QDateTime::fromString(o.value(QLatin1StringView("created")).toString(), Qt::ISODateWithMs);
    p.modified = QDateTime::fromString(o.value(QLatin1StringView("modified")).toString(), Qt::ISODateWithMs);
    if (o.contains(QLatin1StringView("lastVerification")))
        p.lastVerification = ProfileVerification::fromJson(o.value(QLatin1StringView("lastVerification")).toObject());
    return p;
}

QString normalizedPath(const QString& exePath)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(exePath).absoluteFilePath())).toLower();
}

QString profileIdFor(const QString& exePath)
{
    const QByteArray h = QCryptographicHash::hash(normalizedPath(exePath).toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("%1-%2").arg(QFileInfo(exePath).completeBaseName().toLower().left(32), QString::fromLatin1(h.left(12)));
}

// ---- capabilities --------------------------------------------------------------------------------

namespace {

const char* kSdkHint = "Programs that choose their GPU themselves (their own setting) use it regardless; programs integrating the "
                       "GPU Fusion SDK receive the chosen GPU when GPU Fusion launches them.";

} // namespace

ModeCapability assessMode(const AppProfile& profile, GpuMode mode, const CapabilityInputs& in)
{
    ModeCapability c;
    c.mode = mode;
    const auto& lv = profile.lastVerification;
    const bool verifiedThisMode = lv && lv->mode == mode;

    if (mode == GpuMode::Automatic) {
        const pref::ChoicePlan plan = pref::planChoice(GpuChoice::Automatic, in.gpus, in.probeVerification);
        c.capabilityClass = QStringLiteral("A");
        c.mechanism = QStringLiteral("Remove the program's Windows per-app preference");
        c.write = GpuPreference::NotSet;
        c.status = ModeStatus::Verified;
        c.summary = QStringLiteral("Windows decides which GPU the program uses.");
        c.reasons = plan.reasons;
        if (verifiedThisMode && !lv->headline.isEmpty())
            c.reasons.prepend(QStringLiteral("Last verification (%1): %2").arg(lv->time.toString(QStringLiteral("yyyy-MM-dd HH:mm")), lv->headline));
        return c;
    }

    if (mode == GpuMode::Both) {
        c.capabilityClass = QStringLiteral("D");
        if (in.gpus.size() < 2) {
            c.status = ModeStatus::Unsupported;
            c.summary = QStringLiteral("Only one GPU is available.");
            return c;
        }
        c.mechanism = QStringLiteral("Launch through GPU Fusion with both GPUs passed to the program (GPUFUSION_ADAPTERS)");
        c.status = ModeStatus::NeedsAppSupport;
        c.summary = QStringLiteral("Only programs that integrate the GPU Fusion SDK (or otherwise divide their own work between GPUs) can "
                                   "use both; for other programs this mode behaves like the default.");
        c.reasons << QStringLiteral("Splitting one rendering workload across both GPUs (class E): %1").arg(in.dualGpu.cases.value(0))
                  << in.dualGpu.verdict;
        c.reasons.append(in.dualGpu.evidence);
        if (verifiedThisMode) {
            c.reasons.prepend(QStringLiteral("Last verification (%1): %2").arg(lv->time.toString(QStringLiteral("yyyy-MM-dd HH:mm")), lv->headline));
            if (lv->outcome == QLatin1StringView("partly") || lv->confirmed) {
                c.status = ModeStatus::Verified;
                c.summary = QStringLiteral("Verified: both GPUs did work for this program.");
            }
        }
        return c;
    }

    const GpuChoice choice = mode == GpuMode::Integrated ? GpuChoice::Integrated : GpuChoice::Discrete;
    const pref::ChoicePlan plan = pref::planChoice(choice, in.gpus, in.probeVerification);
    c.capabilityClass = QStringLiteral("A");
    c.reasons = plan.reasons;
    c.write = plan.write;
    const QString target = plan.target ? plan.target->name : modeText(mode);
    // The profile passes the GPU through the program's own option (GPU placeholder in its arguments).
    auto argumentsMechanism = [&](const QString& lead) {
        c.status = ModeStatus::Expected;
        c.write.reset(); // a Windows value would not be shown to work: none is written
        c.mechanism = QStringLiteral("The program's own GPU option in its launch arguments, filled in for %1 when GPU Fusion starts it")
                          .arg(target);
        c.summary = QStringLiteral("%1 this profile passes the GPU to the program through its own option. Use \"Verify on this PC\" to "
                                   "confirm that the program honours it.").arg(lead);
        c.reasons << QStringLiteral("Applies only when the program is started from GPU Fusion (Launch / Verify / --launch-profile); "
                                    "started any other way, it uses its default GPU.");
    };
    switch (plan.enforcement) {
    case Enforcement::Verified:
        c.status = ModeStatus::Verified;
        c.mechanism = QStringLiteral("Windows per-app preference (%1)").arg(pref::gpuPreferenceText(*plan.write));
        c.summary = QStringLiteral("Windows places programs that let it choose on %1 (verified with the probe).").arg(target);
        break;
    case Enforcement::Expected:
        c.status = ModeStatus::Expected;
        c.mechanism = QStringLiteral("Windows per-app preference (%1)").arg(pref::gpuPreferenceText(*plan.write));
        c.summary = plan.summary;
        break;
    case Enforcement::NotEnforceable:
        if (usesGpuPlaceholders(profile.arguments) && plan.target) {
            argumentsMechanism(QStringLiteral("Windows cannot place programs on %1 here, but").arg(target));
            break;
        }
        c.status = ModeStatus::NeedsAppSupport;
        c.mechanism = QStringLiteral("Launch through GPU Fusion with the GPU passed to the program (GPUFUSION_ADAPTERS)");
        c.summary = QStringLiteral("Windows cannot place programs on %1 here. %2").arg(target, QLatin1StringView(kSdkHint));
        c.reasons << QStringLiteral("If the program has its own GPU option, put it in the launch arguments with a GPU placeholder "
                                    "(e.g. {luid_pair}, {dxgi_index}); GPU Fusion fills it in for %1.").arg(target)
                  << QStringLiteral("Hypothesis (needs your decision, untested): driving the display from %1 and making it the primary "
                                    "display adapter would make it the default for programs that do not choose a GPU.").arg(target);
        break;
    case Enforcement::NotApplicable:
    case Enforcement::Unknown:
        if (usesGpuPlaceholders(profile.arguments) && plan.target) {
            argumentsMechanism(QStringLiteral("Whether Windows can place programs on %1 here is not known yet, but").arg(target));
            break;
        }
        c.status = plan.target ? ModeStatus::Unknown : ModeStatus::Unsupported;
        c.mechanism = QStringLiteral("Windows per-app preference, once verified");
        c.summary = plan.summary;
        break;
    }
    if (verifiedThisMode) {
        c.reasons.prepend(QStringLiteral("Last verification (%1): %2").arg(lv->time.toString(QStringLiteral("yyyy-MM-dd HH:mm")), lv->headline));
        if (lv->confirmed) {
            c.status = ModeStatus::Verified;
            c.summary = QStringLiteral("Verified for this program: it rendered on %1.").arg(target);
        }
    }
    return c;
}

QList<ModeCapability> assessAllModes(const AppProfile& profile, const CapabilityInputs& in)
{
    QList<ModeCapability> out;
    for (GpuMode m : { GpuMode::Automatic, GpuMode::Integrated, GpuMode::Discrete, GpuMode::Both })
        out << assessMode(profile, m, in);
    return out;
}

QStringList requestedAdapterLuids(GpuMode mode, const QList<pref::SelectableGpu>& gpus)
{
    QStringList out;
    const auto integrated = pref::gpuFor(GpuChoice::Integrated, gpus);
    const auto discrete = pref::gpuFor(GpuChoice::Discrete, gpus);
    switch (mode) {
    case GpuMode::Automatic:
        break;
    case GpuMode::Integrated:
        if (integrated) out << integrated->luid;
        break;
    case GpuMode::Discrete:
        if (discrete) out << discrete->luid;
        break;
    case GpuMode::Both:
        if (discrete) out << discrete->luid;
        if (integrated) out << integrated->luid;
        break;
    }
    return out;
}

// ---- GPU placeholders ------------------------------------------------------------------------------

QStringList gpuPlaceholders()
{
    return { QStringLiteral("{luid}"), QStringLiteral("{luid_pair}"), QStringLiteral("{luid_high}"), QStringLiteral("{luid_low}"),
             QStringLiteral("{dxgi_index}"), QStringLiteral("{vendor_id}"), QStringLiteral("{vendor_hex}"), QStringLiteral("{gpu_name}") };
}

bool usesGpuPlaceholders(const QString& arguments)
{
    for (const QString& p : gpuPlaceholders())
        if (arguments.contains(p))
            return true;
    return false;
}

std::optional<pref::SelectableGpu> modeGpu(GpuMode mode, const QList<pref::SelectableGpu>& gpus)
{
    switch (mode) {
    case GpuMode::Automatic:  return std::nullopt;
    case GpuMode::Integrated: return pref::gpuFor(GpuChoice::Integrated, gpus);
    case GpuMode::Discrete:
    case GpuMode::Both:       return pref::gpuFor(GpuChoice::Discrete, gpus);
    }
    return std::nullopt;
}

QString expandGpuPlaceholders(const QString& text, const pref::SelectableGpu& gpu)
{
    // LUID text "HHHHHHHH_LLLLLLLL": HighPart is a signed LONG, LowPart an unsigned DWORD.
    const QStringList parts = gpu.luid.split(u'_');
    const qint32 high = parts.size() == 2 ? qint32(parts[0].toUInt(nullptr, 16)) : 0;
    const quint32 low = parts.size() == 2 ? parts[1].toUInt(nullptr, 16) : 0;
    QString s = text;
    s.replace(QLatin1StringView("{luid_pair}"), QStringLiteral("%1,%2").arg(high).arg(low))
        .replace(QLatin1StringView("{luid_high}"), QString::number(high))
        .replace(QLatin1StringView("{luid_low}"), QString::number(low))
        .replace(QLatin1StringView("{luid}"), gpu.luid)
        .replace(QLatin1StringView("{dxgi_index}"), QString::number(gpu.dxgiIndex))
        .replace(QLatin1StringView("{vendor_id}"), QString::number(gpu.vendorId))
        .replace(QLatin1StringView("{vendor_hex}"), QStringLiteral("0x%1").arg(gpu.vendorId, 4, 16, QLatin1Char('0')))
        .replace(QLatin1StringView("{gpu_name}"), gpu.name);
    return s;
}

QStringList launchArguments(const QString& arguments, GpuMode mode, const QList<pref::SelectableGpu>& gpus)
{
    const auto gpu = modeGpu(mode, gpus);
    QStringList out;
    for (const QString& a : QProcess::splitCommand(arguments)) {
        if (!usesGpuPlaceholders(a))
            out << a;
        else if (gpu)
            out << expandGpuPlaceholders(a, *gpu);
        // else: left out, so the program chooses its GPU itself
    }
    return out;
}

} // namespace gf::profiles
