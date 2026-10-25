#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::pref::win {

// Runs GPUFusionProbe.exe without a console window (inheriting GPU Fusion's environment, so its
// driver shader cache stays in the data folder) and returns its stdout. Times out after `timeoutMs`.
std::optional<QByteArray> runProbe(const QString& probePath, QString* error, int timeoutMs = 15000);

// Starts an application the user chose, detached, with the user's original environment TEMP.
bool launchDetached(const QString& exePath, QString* error);
// The same with arguments and optional extra environment variables (e.g. GPUFUSION_ADAPTERS for
// SDK-integrated programs); returns the new process id (nullopt on failure).
std::optional<quint32> launchDetachedPid(const QString& exePath, const QStringList& arguments, QString* error,
                                         const QHash<QString, QString>& extraEnvironment = {});

} // namespace gf::pref::win
