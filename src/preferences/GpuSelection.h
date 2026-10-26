#pragma once

// Adapter-oriented GPU choice for one application ("use the Intel GPU", "use the NVIDIA GPU",
// "automatic", "both") — mapped onto the only per-application mechanism Windows 10 offers, the
// UserGpuPreferences value (GpuPreference=0/1/2), *only* where the evidence justifies the mapping.
//
// Evidence, strongest first:
//   1. Verification: GPUFusionProbe was launched under each preference and reported which adapter its
//      default Direct3D 11 device was created on (VerificationResult).
//   2. Windows' ranking: what IDXGIFactory6::EnumAdapterByGpuPreference returns for MINIMUM_POWER /
//      HIGH_PERFORMANCE (from any probe report) — what Windows *intends*, not proof.
// Pure; unit-tested.

#include "preferences/PreferenceModel.h"

namespace gf::pref {

// A physical GPU the user can pick (built by the caller from hardware detection).
struct SelectableGpu {
    QString name;
    quint32 vendorId = 0;
    QString luid;             // "HHHHHHHH_LLLLLLLL", as reported by the probe
    bool integrated = false;  // detection's classification
    int dxgiIndex = -1;       // DXGI enumeration index (what many programs' adapter options take)
};

enum class GpuChoice { Automatic, Integrated, Discrete, Both };

enum class Enforcement {
    Verified,        // the probe got the requested adapter under this preference on this PC
    Expected,        // Windows ranks the requested adapter first for this preference; not verified yet
    NotEnforceable,  // verified: under this preference programs still get another adapter
    NotApplicable,   // nothing to enforce (Automatic) or not expressible (Both)
    Unknown          // no evidence either way
};
QString enforcementText(Enforcement e);

struct ChoicePlan {
    GpuChoice choice = GpuChoice::Automatic;
    std::optional<SelectableGpu> target;      // the adapter the user asked for (Integrated/Discrete)
    std::optional<GpuPreference> write;       // what to store; nullopt = do not write anything
    Enforcement enforcement = Enforcement::Unknown;
    QString summary;                          // one line for the UI
    QStringList reasons;                      // the evidence, in plain language
};

// Picks the integrated / discrete adapter from the list (nullopt when there is not exactly one kind).
std::optional<SelectableGpu> gpuFor(GpuChoice choice, const QList<SelectableGpu>& gpus);

ChoicePlan planChoice(GpuChoice choice, const QList<SelectableGpu>& gpus, const std::optional<VerificationResult>& verification);

} // namespace gf::pref
