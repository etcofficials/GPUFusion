#pragma once

#include "hardware/AdapterModel.h"

namespace gf::hw::win {

struct DxgiEnumeration {
    QList<DxgiAdapter> adapters;
    QStringList errors;
};

// Enumerates DXGI adapters and probes Direct3D 11 / 12 capabilities, WDDM version, PCI location
// and display outputs. Devices are created only for the duration of the probe and released.
// If `previous` contains an adapter with the same LUID and driver version, its capability probes
// are reused instead of creating devices again (capsReused = true).
DxgiEnumeration enumerateDxgi(const QList<DxgiAdapter>* previous);

} // namespace gf::hw::win
