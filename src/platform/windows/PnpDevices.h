#pragma once

#include "hardware/AdapterModel.h"

namespace gf::hw::win {

struct PnpEnumeration {
    QList<PnpDevice> devices;
    QStringList errors;
};

// Lists every display-class device Windows knows about — present, disabled, failing, and devices
// that were installed earlier but are not detected now — with driver details. Read-only.
PnpEnumeration enumerateDisplayDevices();

} // namespace gf::hw::win
