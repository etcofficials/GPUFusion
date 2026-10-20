#pragma once

#include "hardware/AdapterModel.h"
#include "monitoring/UsageVerification.h"
#include "preferences/GpuSelection.h"

namespace gf::pref { class PreferenceManager; }

namespace gf::ui {

// Physical GPUs as selectable choices (name, vendor, LUID, integrated?).
QList<pref::SelectableGpu> selectableGpus(const hw::DetectionResult& detection);

// Completes observed evidence with what is stored for the program and the adapter that stored value
// asks for ("Power saving" -> the integrated GPU, "High performance" -> the discrete GPU), then
// evaluates it. Whether Windows honours the request is exactly what the observation decides.
// `storedValueSelectsNoAdapter` is set when a value is stored that requests no specific adapter.
mon::UsageVerdict evaluateForApp(mon::UsageEvidence evidence, const pref::PreferenceManager& manager,
                                 const hw::DetectionResult& detection, bool* storedValueSelectsNoAdapter = nullptr);

} // namespace gf::ui
