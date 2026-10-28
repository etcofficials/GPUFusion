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
    if (!profile.variant.isEmpty()) {
        // One Windows value per executable, owned by the main profile: a variant never writes or removes it.
        r.storedValue = manager.rawValue(r.exePath).value_or(QString());
        profile.appliedValue.clear();
        r.ok = true;
        r.message = QStringLiteral("Variant saved; no Windows setting changed (the program's Windows GPU preference belongs to its main "
                                   "profile). %1").arg(usesGpuPlaceholders(profile.arguments)
                                       ? QStringLiteral("The GPU is passed in the launch arguments when GPU Fusion starts it.")
                                       : QStringLiteral("Without a GPU option in the arguments (or the SDK), the program uses its default GPU."));
        qCInfo(lcPrefs).noquote() << "Profile" << displayName(profile) << modeKey(profile.mode) << ":" << r.message;
        return r;
    }
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
    env.insert(QStringLiteral("GPUFUSION_ENV_VERSION"), QStringLiteral("1")); // format of these variables (SDK 2+ checks it)
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
    const QStringList args = profileArguments(profile, gpus);
    const QHash<QString, QString> env = launchEnvironment(profile, gpus);
    QString localError;
    const auto pid = pref::win::launchDetachedPid(exe, args, &localError, env);
    if (error)
        *error = localError;
    QStringList vars;
    for (auto it = env.cbegin(); it != env.cend(); ++it)
        vars << QStringLiteral("%1=%2").arg(it.key(), it.value());
    vars.sort();
    if (pid)
        qCInfo(lcPrefs).noquote() << QStringLiteral("Launched %1 (process %2) from its profile, mode %3; arguments: %4; variables: %5")
                                         .arg(displayName(profile)).arg(*pid).arg(modeKey(profile.mode))
                                         .arg(args.isEmpty() ? QStringLiteral("none") : args.join(u' '))
                                         .arg(vars.isEmpty() ? QStringLiteral("none") : vars.join(QStringLiteral(" ")));
    else
        qCWarning(lcPrefs).noquote() << "Could not launch" << displayName(profile) << ":" << localError;
    return pid;
}

QString launchMessage(const AppProfile& profile, quint32 pid)
{
    const QString started = QStringLiteral("Started %1 (process %2).").arg(displayName(profile)).arg(pid);
    QString what;
    switch (profile.mode) {
    case GpuMode::Both:
        what = QStringLiteral("Both GPUs were offered to it; only programs that integrate the GPU Fusion SDK use them, others run on one GPU as in Automatic.");
        break;
    case GpuMode::Automatic:
        what = QStringLiteral("Windows and the program choose its GPU (Automatic).");
        break;
    default:
        what = QStringLiteral("Its profile asks for %1.").arg(modeText(profile.mode));
        break;
    }
    QStringList parts{ started, what, QStringLiteral("A started process does not show which GPU it uses: the GPU profile column on the "
                                                     "Applications page and Verify do.") };
    parts << launchCaveats(profile);
    return parts.join(u' ');
}

bool isPackagedInstall(const QString& exePath)
{
    const QString path = QDir::fromNativeSeparators(exePath).toLower();
    for (const char* marker : { "/windowsapps/", "/xboxgames/", "/wpsystem/" }) {
        if (path.contains(QLatin1StringView(marker)))
            return true;
    }
    const QDir dir = QFileInfo(exePath).absoluteDir();
    return QFileInfo::exists(dir.filePath(QStringLiteral("AppxManifest.xml"))) || QFileInfo::exists(dir.filePath(QStringLiteral("MicrosoftGame.config")));
}

QStringList launchCaveats(const AppProfile& profile)
{
    QStringList out;
    if (isPackagedInstall(profile.exePath))
        out << QStringLiteral("This program is installed as a Microsoft Store / Xbox package: Windows may start it through its package, "
                              "and a copy that is already running may take over, so its GPU option and GPU Fusion's variables may not reach it.");
    return out;
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
    // Both asks for both GPUs: rendering on the discrete GPU plus work on the other (even of unknown type,
    // from a driver that does not name engines) is what it asks for.
    if (mode == GpuMode::Both && v.outcome == mon::UsageOutcome::Confirmed && !v.otherAdaptersWithUnnamedWork.isEmpty()) {
        v.outcome = mon::UsageOutcome::BothAdapters;
        v.headline = QStringLiteral("Partly: the program used %1 and %2.").arg(e.requestedName, v.otherAdaptersWithUnnamedWork.join(QStringLiteral(" and ")));
    }
    if (mode == GpuMode::Both && v.outcome == mon::UsageOutcome::BothAdapters) {
        // What Both asks for. This shows that both GPUs did work for the program, not that its rendering was sped up.
        v.headline.replace(QStringLiteral("Partly: "), QStringLiteral("Both GPUs used: "));
        v.details << QStringLiteral("Both GPUs did work for the program. That alone does not show that its rendering was split or faster.");
    }
    // Both, but the program's work was on one GPU only: say so plainly (never "Confirmed" for Both).
    const bool oneGpuOfBoth = mode == GpuMode::Both
        && (v.outcome == mon::UsageOutcome::Confirmed || v.outcome == mon::UsageOutcome::ConfirmedUnnamed || v.outcome == mon::UsageOutcome::WrongAdapter);
    if (oneGpuOfBoth) {
        QStringList where;
        for (const mon::AdapterActivity& a : v.activity)
            where << a.name;
        v.headline = QStringLiteral("One GPU only: the program's GPU work ran on %1. Both GPUs were offered, but the program did not use the second one.")
                         .arg(where.isEmpty() ? e.requestedName : where.join(QStringLiteral(", ")));
        v.details << QStringLiteral("Both GPUs mode passes both GPUs to programs that integrate the GPU Fusion SDK. Other programs ignore them "
                                    "and run on one GPU, as in Automatic; GPU Fusion does not split another program's work.");
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
        if (oneGpuOfBoth)
            record->outcome = QStringLiteral("one-gpu");
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
