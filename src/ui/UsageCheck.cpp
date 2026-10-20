#include "ui/UsageCheck.h"

namespace gf::ui {

using namespace gf::pref;

QList<SelectableGpu> selectableGpus(const hw::DetectionResult& detection)
{
    QList<SelectableGpu> out;
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        out << SelectableGpu{ a.name(), a.dxgi->vendorId, hw::formatLuid(a.dxgi->luid), a.classification.value == hw::AdapterClass::Integrated };
    }
    return out;
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
