#include "ui/UsageCheck.h"

#include "profiles/ProfileActions.h"

namespace gf::ui {

using namespace gf::pref;

QList<SelectableGpu> selectableGpus(const hw::DetectionResult& detection)
{
    return profiles::selectableGpus(detection); // one definition for the whole app
}

mon::UsageVerdict evaluateForApp(mon::UsageEvidence e, const PreferenceManager& manager, const hw::DetectionResult& detection,
                                 bool* storedValueSelectsNoAdapter)
{
    e.storedPreference = manager.rawValue(e.targetExe);
    const GpuPreference current = manager.current(e.targetExe);
    e.preferenceRequired = current != GpuPreference::NotSet;
    bool noAdapter = e.preferenceRequired;
    if (current == GpuPreference::PowerSaving || current == GpuPreference::HighPerformance) {
        const GpuChoice c = current == GpuPreference::PowerSaving ? GpuChoice::Integrated : GpuChoice::Discrete;
        const QList<SelectableGpu> gpus = selectableGpus(detection);
        // The stored value requests the adapter it is meant to select; whether Windows honours it is
        // exactly what the observation decides.
        if (const auto target = gpuFor(c, gpus)) {
            for (const mon::ObservedAdapter& a : e.adapters) {
                if (hw::formatLuid(a.luid).compare(target->luid, Qt::CaseInsensitive) == 0) {
                    e.requestedLuid = a.luid;
                    e.requestedName = a.name;
                    noAdapter = false;
                }
            }
        }
    }
    if (storedValueSelectsNoAdapter)
        *storedValueSelectsNoAdapter = noAdapter;
    return mon::evaluateUsage(e);
}

} // namespace gf::ui
