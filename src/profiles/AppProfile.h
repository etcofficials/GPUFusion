#pragma once

// Generic per-application GPU profiles: one per executable, for any program (never a built-in list).
// A profile records the GPU mode the user chose, optional launch arguments, what was applied and what
// verification observed, and — computed, not stored — what each mode can achieve for this program on
// this PC, by capability class:
//   A  select which GPU renders the program        (Windows per-app preference; generic)
//   B  different GPUs for different programs       (one profile per program, launched with its mode)
//   C  independent workloads across GPUs           (GPU Fusion's dispatcher; not a property of one app)
//   D  one program distributing its own work       (only programs integrating the GPU Fusion SDK)
//   E  one rendering workload split across GPUs    (needs Direct3D 12 on every GPU)
// Pure; unit-tested.

#include "preferences/DualGpu.h"
#include "preferences/GpuSelection.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::profiles {

enum class GpuMode { Automatic, Integrated, Discrete, Both };
QString modeKey(GpuMode m);                  // "automatic", "integrated", "discrete", "both"
QString modeText(GpuMode m);                 // "Automatic", "Integrated GPU", ...
std::optional<GpuMode> modeFromKey(const QString& key);

struct ProfileVerification {
    QDateTime time;
    GpuMode mode = GpuMode::Automatic;       // the mode that was in effect
    QString outcome;                         // "confirmed", "not-applied", "partly", "observed", "no-activity", ...
    QString headline;
    QStringList stages;                      // "<name>: <state> — <detail>"
    QStringList renderingExecutables;
    bool confirmed = false;                  // the requested GPU was confirmed
    QJsonObject toJson() const;
    static std::optional<ProfileVerification> fromJson(const QJsonObject& o);
};

struct AppProfile {
    QString id;                              // derived from the executable path (see profileIdFor)
    QString name;                            // display name
    QString exePath;                         // absolute, native separators
    QString arguments;                       // optional launch arguments, passed to the program as typed
    GpuMode mode = GpuMode::Automatic;
    bool shareGpuChoiceWithApp = true;       // set GPUFUSION_* variables for SDK-integrated programs
    QString appliedValue;                    // Windows preference value read back after applying ("" = none)
    std::optional<ProfileVerification> lastVerification;
    QDateTime created, modified;

    QJsonObject toJson() const;
    static std::optional<AppProfile> fromJson(const QJsonObject& o);
};

// Stable id for an executable: case-insensitive, separator-insensitive hash of its absolute path.
QString profileIdFor(const QString& exePath);
QString normalizedPath(const QString& exePath);

// ---- capability assessment ------------------------------------------------------------------------

enum class ModeStatus {
    Verified,           // observed for this program, or verified for this kind of selection on this PC
    Expected,           // the mechanism should work; not verified yet
    NeedsAppSupport,    // only if the program selects the GPU itself or integrates the SDK
    NeedsUserAction,    // possible after a change only the user can make (e.g. display wiring)
    Unsupported,        // not possible on this PC
    Unknown
};
QString modeStatusText(ModeStatus s);

struct ModeCapability {
    GpuMode mode = GpuMode::Automatic;
    ModeStatus status = ModeStatus::Unknown;
    QString capabilityClass;   // "A", "B", "D", "E" — what kind of capability this mode means
    QString mechanism;         // how GPU Fusion applies it
    QString summary;
    QStringList reasons;
    std::optional<pref::GpuPreference> write; // Windows value to store, if any
};

struct CapabilityInputs {
    QList<pref::SelectableGpu> gpus;
    std::optional<pref::VerificationResult> probeVerification; // what the preference does on this PC
    pref::DualGpuAssessment dualGpu;                           // for this executable
};

ModeCapability assessMode(const AppProfile& profile, GpuMode mode, const CapabilityInputs& in);
QList<ModeCapability> assessAllModes(const AppProfile& profile, const CapabilityInputs& in);

// What GPU Fusion tells an SDK-integrated program when it launches it in a mode: the adapters to use,
// in order (LUIDs as "HHHHHHHH_LLLLLLLL"). Automatic -> none (the program decides).
QStringList requestedAdapterLuids(GpuMode mode, const QList<pref::SelectableGpu>& gpus);

} // namespace gf::profiles
