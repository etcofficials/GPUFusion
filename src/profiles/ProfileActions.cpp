#include "profiles/ProfileActions.h"

#include "core/Logging.h"
#include "platform/windows/PeImports.h"
#include "platform/windows/ProbeRunner.h"
#include "preferences/PreferenceModel.h"
#include "profiles/ProfileStore.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

namespace gf::profiles {

using pref::GpuChoice;
using pref::GpuPreference;

QList<pref::SelectableGpu> selectableGpus(const hw::DetectionResult& detection)
{
    QList<pref::SelectableGpu> out;
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        out << pref::SelectableGpu{ a.name(), a.dxgi->vendorId, hw::formatLuid(a.dxgi->luid), a.classification.value == hw::AdapterClass::Integrated,
                                    a.dxgi->index };
    }
    return out;
}

pref::MultiGpuSystem multiGpuSystem(const hw::DetectionResult& detection, const std::optional<pref::VerificationResult>& probeVerification)
{
    pref::MultiGpuSystem s;
    QSet<quint32> vendors;
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        ++s.physicalGpus;
        vendors.insert(a.dxgi->vendorId);
        if (a.dxgi->d3d12.device.ok())
            ++s.d3d12Gpus;
        else
            s.gpusWithoutD3D12 << a.name();
    }
    s.sameVendor = vendors.size() == 1;
    s.preferenceMovesApps = probeVerification && probeVerification->restored && !probeVerification->preferenceHasNoEffect();
    return s;
}

CapabilityInputs capabilityInputs(const QString& exePath, const hw::DetectionResult& detection,
                                  const std::optional<pref::VerificationResult>& probeVerification)
{
    CapabilityInputs in;
    in.gpus = selectableGpus(detection);
    in.probeVerification = probeVerification;
    const QString exe = resolveCurrentExecutable(exePath);
    QString error;
    const auto imports = pref::win::readImports(exe, &error);
    in.dualGpu = pref::assessDualGpu(QFileInfo(exe).fileName(), imports, error, multiGpuSystem(detection, probeVerification));
    return in;
}

ApplyResult applyProfile(AppProfile& profile, pref::PreferenceManager& manager, const CapabilityInputs& inputs)
{
    ApplyResult r;
    r.exePath = resolveCurrentExecutable(profile.exePath);
    QString why;
    if (!pref::isConfigurableExecutable(r.exePath, &why)) {
        r.message = why;
        return r;
    }
    const ModeCapability cap = assessMode(profile, profile.mode, inputs);
    const GpuPreference value = cap.write.value_or(GpuPreference::NotSet); // unjustified -> no Windows value
    const pref::ChangeResult change = manager.current(r.exePath) == value && (value != GpuPreference::NotSet || !manager.rawValue(r.exePath))
        ? pref::ChangeResult{ true, {}, std::nullopt }
        : manager.apply(r.exePath, value);
    if (!change.ok) {
        r.message = change.error;
        return r;
    }
    const std::optional<QString> stored = manager.rawValue(r.exePath);
    r.storedValue = stored.value_or(QString());
    profile.appliedValue = r.storedValue;
    r.ok = true;
    if (value == GpuPreference::NotSet) {
        r.message = cap.write ? QStringLiteral("Windows decides: no per-app value is stored for %1.").arg(QFileInfo(r.exePath).fileName())
                  : usesGpuPlaceholders(profile.arguments)
                      ? QStringLiteral("No Windows value is stored (it would not select this GPU here); the GPU is passed to the program "
                                       "in its launch arguments when GPU Fusion starts it.")
                      : QStringLiteral("No Windows value is stored (it would not select this GPU here); the choice is passed to the "
                                       "program when GPU Fusion launches it.");
    } else {
        r.message = QStringLiteral("Stored and read back: %1").arg(r.storedValue);
    }
    if (normalizedPath(r.exePath) != normalizedPath(profile.exePath))
        r.message += QStringLiteral(" (applied to the current version: %1)").arg(r.exePath);
    qCInfo(lcPrefs).noquote() << "Profile" << profile.name << modeKey(profile.mode) << ":" << r.message;
    return r;
}

QHash<QString, QString> launchEnvironment(const AppProfile& profile, const QList<pref::SelectableGpu>& gpus)
{
    QHash<QString, QString> env;
    if (!profile.shareGpuChoiceWithApp)
        return env;
    env.insert(QStringLiteral("GPUFUSION_MODE"), modeKey(profile.mode));
    env.insert(QStringLiteral("GPUFUSION_ADAPTERS"), requestedAdapterLuids(profile.mode, gpus).join(u','));
    env.insert(QStringLiteral("GPUFUSION_PROFILE"), profile.id);
    return env;
}

QStringList profileArguments(const AppProfile& profile, const QList<pref::SelectableGpu>& gpus)
{
    return launchArguments(profile.arguments, profile.mode, gpus);
}

std::optional<quint32> launchProfile(const AppProfile& profile, const QList<pref::SelectableGpu>& gpus, QString* error)
{
    const QString exe = resolveCurrentExecutable(profile.exePath);
    return pref::win::launchDetachedPid(exe, profileArguments(profile, gpus), error, launchEnvironment(profile, gpus));
}

mon::UsageVerdict evaluateProfileEvidence(mon::UsageEvidence e, GpuMode mode, const QList<pref::SelectableGpu>& gpus,
                                          ProfileVerification* record)
{
    // Requested adapter by mode: Integrated/Discrete -> that GPU; Both -> the discrete GPU as the primary
    // (rendering on both then shows as "partly", which is what Both asks for).
    std::optional<pref::SelectableGpu> target;
    if (mode == GpuMode::Integrated)
        target = pref::gpuFor(GpuChoice::Integrated, gpus);
    else if (mode == GpuMode::Discrete || mode == GpuMode::Both)
        target = pref::gpuFor(GpuChoice::Discrete, gpus);
    if (target) {
        for (const mon::ObservedAdapter& a : e.adapters) {
            if (hw::formatLuid(a.luid).compare(target->luid, Qt::CaseInsensitive) == 0) {
                e.requestedLuid = a.luid;
                e.requestedName = a.name;
            }
        }
    }
    mon::UsageVerdict v = mon::evaluateUsage(e);
    if (mode == GpuMode::Both && v.outcome == mon::UsageOutcome::BothAdapters) {
        // What Both asks for. This shows that both GPUs did work for the program, not that its rendering was sped up.
        v.headline.replace(QStringLiteral("Partly: "), QStringLiteral("Both GPUs used: "));
        v.details << QStringLiteral("Both GPUs did work for the program. That alone does not show that its rendering was split or faster.");
    }
    if (record) {
        record->time = QDateTime::currentDateTime();
        record->mode = mode;
        record->headline = v.headline;
        record->stages.clear();
        for (const mon::UsageStage& s : v.stages)
            record->stages << (s.detail.isEmpty() ? QStringLiteral("%1: %2").arg(s.name, mon::stageStateText(s.state))
                                                  : QStringLiteral("%1: %2 — %3").arg(s.name, mon::stageStateText(s.state), s.detail));
        record->renderingExecutables = v.renderingExecutables;
        switch (v.outcome) {
        case mon::UsageOutcome::Confirmed:        record->outcome = QStringLiteral("confirmed"); break;
        case mon::UsageOutcome::ConfirmedUnnamed: record->outcome = QStringLiteral("confirmed-unnamed"); break;
        case mon::UsageOutcome::WrongAdapter:     record->outcome = QStringLiteral("not-applied"); break;
        case mon::UsageOutcome::BothAdapters:     record->outcome = QStringLiteral("partly"); break;
        case mon::UsageOutcome::Observed:         record->outcome = QStringLiteral("observed"); break;
        case mon::UsageOutcome::NoActivity:       record->outcome = QStringLiteral("no-activity"); break;
        case mon::UsageOutcome::NotLaunched:      record->outcome = QStringLiteral("not-launched"); break;
        }
        record->confirmed = mode == GpuMode::Both
            ? v.outcome == mon::UsageOutcome::BothAdapters
            : (mode != GpuMode::Automatic && (v.outcome == mon::UsageOutcome::Confirmed || v.outcome == mon::UsageOutcome::ConfirmedUnnamed));
    }
    return v;
}

mon::UsageVerdict verifyProfile(AppProfile& profile, const hw::DetectionResult& detection, const std::optional<QString>& storedValue,
                                int seconds, const std::atomic<bool>* cancel, const mon::ObserveProgress& progress)
{
    const QList<pref::SelectableGpu> gpus = selectableGpus(detection);
    mon::ObserveOptions o;
    o.exe = resolveCurrentExecutable(profile.exePath);
    o.arguments = profileArguments(profile, gpus);
    o.environment = launchEnvironment(profile, gpus);
    o.seconds = seconds;
    mon::UsageEvidence e = mon::observeUsage(mon::targetsFrom(detection), o, cancel, progress);
    e.storedPreference = storedValue;
    e.preferenceRequired = storedValue.has_value(); // stage 1 reports what the profile stored (if anything)
    ProfileVerification record;
    const mon::UsageVerdict v = evaluateProfileEvidence(e, profile.mode, gpus, &record);
    profile.lastVerification = record;
    return v;
}

} // namespace gf::profiles
