#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace gf::pref::win {

// Runs GPUFusionProbe.exe without a console window (inheriting GPU Fusion's environment, so its
// driver shader cache stays in the data folder) and returns its stdout. Times out after `timeoutMs`.
std::optional<QByteArray> runProbe(const QString& probePath, QString* error, int timeoutMs = 15000);

// Starts an application the user chose, detached, with the user's original environment TEMP.
bool launchDetached(const QString& exePath, QString* error);

} // namespace gf::pref::win
