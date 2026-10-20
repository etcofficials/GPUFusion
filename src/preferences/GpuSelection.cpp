#include "preferences/GpuSelection.h"

namespace gf::pref {

namespace {

// By vendor and model name, not LUID: Windows assigns new LUIDs at every boot, so a verification
// saved before a restart would otherwise never match the current adapters. (Two identical GPUs of
// the same kind are rejected earlier by gpuFor() as ambiguous.)
bool sameAdapter(const ProbeAdapter& a, const SelectableGpu& g)
{
    return a.vendorId == g.vendorId && a.name == g.name;
}

const VerificationStep* stepFor(const VerificationResult& v, GpuPreference p)
{
    for (const VerificationStep& s : v.steps) {
        if (s.preference == p && s.ok && s.report.d3d11Default)
            return &s;
    }
    return nullptr;
}

// Windows' own ranking for a preference, from any successful probe report.
std::optional<ProbeAdapter> windowsRanking(const VerificationResult& v, GpuPreference p)
{
    for (const VerificationStep& s : v.steps) {
        if (!s.ok)
            continue;
        const auto& r = p == GpuPreference::PowerSaving ? s.report.minimumPower : s.report.highPerformance;
        if (r)
            return r;
    }
    return std::nullopt;
}

QString quoted(GpuPreference p)
{
    return QStringLiteral("“%1”").arg(gpuPreferenceText(p));
}

} // namespace

QString enforcementText(Enforcement e)
{
    switch (e) {
    case Enforcement::Verified:       return QStringLiteral("Verified on this PC");
    case Enforcement::Expected:       return QStringLiteral("Expected (not verified yet)");
    case Enforcement::NotEnforceable: return QStringLiteral("Windows cannot enforce this on this PC");
    case Enforcement::NotApplicable:  return QStringLiteral("Not applicable");
    case Enforcement::Unknown:        return QStringLiteral("Unknown — run the verification");
    }
    return {};
}

std::optional<SelectableGpu> gpuFor(GpuChoice choice, const QList<SelectableGpu>& gpus)
{
    if (choice != GpuChoice::Integrated && choice != GpuChoice::Discrete)
        return std::nullopt;
    const bool wantIntegrated = choice == GpuChoice::Integrated;
    std::optional<SelectableGpu> found;
    for (const SelectableGpu& g : gpus) {
        if (g.integrated != wantIntegrated)
            continue;
        if (found)
            return std::nullopt; // two of a kind: "the integrated GPU" is ambiguous
        found = g;
    }
    return found;
}

ChoicePlan planChoice(GpuChoice choice, const QList<SelectableGpu>& gpus, const std::optional<VerificationResult>& verification)
{
    ChoicePlan plan;
    plan.choice = choice;

    if (choice == GpuChoice::Automatic) {
        plan.write = GpuPreference::NotSet;
        plan.enforcement = Enforcement::NotApplicable;
        plan.summary = QStringLiteral("Windows decides: the stored preference for this program is removed.");
        if (verification) {
            if (const VerificationStep* s = stepFor(*verification, GpuPreference::NotSet))
                plan.reasons << QStringLiteral("With no preference, programs that let Windows choose got %1 (verified with the probe).")
                                    .arg(s->report.d3d11Default->name);
        }
        return plan;
    }

    if (choice == GpuChoice::Both) {
        plan.enforcement = Enforcement::NotApplicable;
        plan.summary = QStringLiteral("Not possible as a setting: Windows assigns each program one GPU for its default device.");
        plan.reasons << QStringLiteral("A program uses two GPUs only if it was written to (it must create a device on each and divide the "
                                       "work itself); no Windows setting can make an existing program do that.")
                     << QStringLiteral("See the dual-GPU check for this program, and the Workload Lab for measured experiments with GPU "
                                       "Fusion's own multi-GPU workloads.");
        return plan;
    }

    const GpuPreference pref = choice == GpuChoice::Integrated ? GpuPreference::PowerSaving : GpuPreference::HighPerformance;
    plan.target = gpuFor(choice, gpus);
    if (!plan.target) {
        plan.enforcement = Enforcement::Unknown;
        plan.summary = choice == GpuChoice::Integrated ? QStringLiteral("No single integrated GPU was detected, so this choice has no target.")
                                                       : QStringLiteral("No single discrete GPU was detected, so this choice has no target.");
        return plan;
    }
    const SelectableGpu& target = *plan.target;

    if (!verification) {
        plan.enforcement = Enforcement::Unknown;
        plan.summary = QStringLiteral("Not verified on this PC yet: run the verification to find out whether %1 maps to %2.")
                           .arg(quoted(pref), target.name);
        return plan;
    }

    if (const VerificationStep* s = stepFor(*verification, pref)) {
        const ProbeAdapter& got = *s->report.d3d11Default;
        if (sameAdapter(got, target)) {
            plan.write = pref;
            plan.enforcement = Enforcement::Verified;
            plan.summary = QStringLiteral("Stores %1, which put the probe on %2 when verified.").arg(quoted(pref), target.name);
            plan.reasons << QStringLiteral("With %1 the probe's Direct3D device was created on %2.").arg(quoted(pref), got.name);
            if (const VerificationStep* none = stepFor(*verification, GpuPreference::NotSet); none && sameAdapter(*none->report.d3d11Default, target))
                plan.reasons << QStringLiteral("Programs get %1 even without a preference here; storing it makes the choice explicit.").arg(target.name);
            plan.reasons << QStringLiteral("This holds for programs that let Windows choose the GPU; programs that pick an adapter themselves "
                                           "ignore the preference.");
            return plan;
        }
        plan.enforcement = Enforcement::NotEnforceable;
        plan.summary = QStringLiteral("Windows does not move programs to %1 on this PC: with %2 the probe still ran on %3.")
                           .arg(target.name, quoted(pref), got.name);
        plan.reasons << QStringLiteral("Verified with the probe: under %1 its default Direct3D device was created on %2, not %3.")
                            .arg(quoted(pref), got.name, target.name);
        if (const auto ranked = windowsRanking(*verification, pref); ranked && !sameAdapter(*ranked, target))
            plan.reasons << QStringLiteral("Windows ranks %1 first for %2, so it does not regard %3 as the GPU for that preference.")
                                .arg(ranked->name, quoted(pref), target.name);
        plan.reasons << QStringLiteral("GPU Fusion therefore does not store %1 for this choice — it would not do what it says.").arg(quoted(pref));
        return plan;
    }

    // No verification step for this preference: fall back to Windows' ranking (intent, not proof).
    if (const auto ranked = windowsRanking(*verification, pref)) {
        if (sameAdapter(*ranked, target)) {
            plan.write = pref;
            plan.enforcement = Enforcement::Expected;
            plan.summary = QStringLiteral("Windows ranks %1 first for %2; not verified yet.").arg(target.name, quoted(pref));
        } else {
            plan.enforcement = Enforcement::NotEnforceable;
            plan.summary = QStringLiteral("Windows ranks %1, not %2, first for %3.").arg(ranked->name, target.name, quoted(pref));
        }
        return plan;
    }
    plan.enforcement = Enforcement::Unknown;
    plan.summary = QStringLiteral("The verification did not cover %1; run it again.").arg(quoted(pref));
    return plan;
}

} // namespace gf::pref
