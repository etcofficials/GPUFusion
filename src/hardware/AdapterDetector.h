#pragma once

#include "hardware/AdapterModel.h"

namespace gf::hw {

// Runs a complete detection: DXGI + Direct3D probes, Device Manager, merge, findings.
// Blocking (typically 0.1–1 s when devices are probed); call it off the UI thread.
// When `previous` is given, capability probes of unchanged adapters are reused.
DetectionResult detectAdapters(const DetectionResult* previous);

// One-line description per adapter for logs.
QString summarize(const Adapter& adapter);

} // namespace gf::hw
