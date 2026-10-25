#pragma once

#include "hardware/AdapterModel.h"
#include "monitoring/UsageObserver.h"
#include "profiles/AppProfile.h"

#include <QHash>

#include <atomic>

namespace gf::pref { class PreferenceManager; }

namespace gf::profiles {

QList<pref::SelectableGpu> selectableGpus(const hw::DetectionResult& detection);
pref::MultiGpuSystem multiGpuSystem(const hw::DetectionResult& detection, const std::optional<pref::VerificationResult>& probeVerification);

// Everything assessMode needs for one executable: this PC's GPUs, what the Windows preference does
// here (probe verification, if run), and the dual-GPU assessment of the program (from its imports).
CapabilityInputs capabilityInputs(const QString& exePath, const hw::DetectionResult& detection,
                                  const std::optional<pref::VerificationResult>& probeVerification);

struct ApplyResult {
    bool ok = false;
    QString message;            // what was done, in plain language
    QString storedValue;        // Windows value read back afterwards ("" = none stored)
    QString exePath;            // the executable the change was applied to (current version)
};

// Applies the profile's mode with the strongest generic mechanism that is justified on this PC:
//  - a verified/expected Windows per-app preference for that GPU, written through the journalled
//    PreferenceManager and read back;
//  - otherwise no Windows value at all (any stored one is removed, so it cannot push the program the
//    other way); the GPU choice then reaches only programs launched by GPU Fusion that read it (SDK).
ApplyResult applyProfile(AppProfile& profile, pref::PreferenceManager& manager, const CapabilityInputs& inputs);

// Variables GPU Fusion sets when it launches a program from its profile (read by the SDK):
//   GPUFUSION_MODE=automatic|integrated|discrete|both, GPUFUSION_ADAPTERS=<luid>[,<luid>],
//   GPUFUSION_PROFILE=<profile id>
QHash<QString, QString> launchEnvironment(const AppProfile& profile, const QList<pref::SelectableGpu>& gpus);

// Splits the profile's argument string the way a command line is split.
QStringList profileArguments(const AppProfile& profile);

std::optional<quint32> launchProfile(const AppProfile& profile, const QList<pref::SelectableGpu>& gpus, QString* error);

// Launches the program from its profile, observes its process tree and evaluates the result against
// the profile's mode (Integrated/Discrete -> that GPU; Both -> both GPUs rendering; Automatic ->
// where it ran). Stores the result in profile.lastVerification. Call from a worker thread.
mon::UsageVerdict verifyProfile(AppProfile& profile, const hw::DetectionResult& detection, const std::optional<QString>& storedValue,
                                int seconds, const std::atomic<bool>* cancel = nullptr, const mon::ObserveProgress& progress = {});

// The verdict for a profile's mode from evidence (pure; used by verifyProfile and tests).
mon::UsageVerdict evaluateProfileEvidence(mon::UsageEvidence evidence, GpuMode mode, const QList<pref::SelectableGpu>& gpus,
                                          ProfileVerification* record);

} // namespace gf::profiles
