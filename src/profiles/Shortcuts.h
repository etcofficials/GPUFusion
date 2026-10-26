#pragma once

// Shortcuts that start a program through its GPU Fusion profile. A profile's GPU option (placeholders
// in its arguments), the SDK variables and the running-instance check apply only when GPU Fusion
// starts the program; a shortcut makes that the everyday way to start it. The shortcut runs
//   GPUFusion.exe --launch-profile <id> --interactive [--data-dir <dir>]
// which starts the program without showing GPU Fusion's window, and asks (in a small dialog) what to
// do if the program is already running. Shortcuts are written only where the user chooses.

#include "profiles/AppProfile.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace gf::profiles {

struct ShortcutSpec {
    QString path;           // the .lnk file
    QString target;         // GPUFusion.exe
    QStringList arguments;
    QString workingDirectory;
    QString iconPath;       // the program's own icon
    int iconIndex = 0;
    QString description;
};

// "<program name> (<mode>)", safe as a file name.
QString shortcutName(const AppProfile& profile);
// `dataDir` non-empty: GPU Fusion was started with --data-dir, so the shortcut uses the same data.
QStringList shortcutArguments(const AppProfile& profile, const QString& dataDir = {});
ShortcutSpec profileShortcut(const AppProfile& profile, const QString& gpuFusionExe, const QString& folder, const QString& dataDir = {});
// Command-line form of the arguments (quoted where needed), as stored in the .lnk.
QString joinArguments(const QStringList& arguments);

bool writeShortcut(const ShortcutSpec& spec, QString* error);
std::optional<ShortcutSpec> readShortcut(const QString& path, QString* error);
// The user's Desktop folder (empty if unknown).
QString desktopFolder();

} // namespace gf::profiles
