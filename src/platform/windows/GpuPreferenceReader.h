#pragma once

#include "monitoring/ProcessModel.h"

#include <QHash>

namespace gf::mon::win {

// Reads (never writes) the per-user graphics preferences that Windows Settings > Display >
// Graphics settings stores under HKCU\Software\Microsoft\DirectX\UserGpuPreferences.
// Keys of the result are executable paths in lower case with backslashes.
struct GpuPreferences {
    QHash<QString, GpuPreference> byPath;
    QString error; // empty on success (a missing key just means nothing is set)
};
GpuPreferences readUserGpuPreferences();

QString preferenceKey(const QString& exePath); // normalises a path for lookup

} // namespace gf::mon::win
