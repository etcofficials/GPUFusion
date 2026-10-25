#pragma once

#include <QList>
#include <QString>

namespace gf::profiles {

// An installed or running program the user can pick for a profile. Discovery is generic and
// read-only: Start-menu shortcuts (all users and the current user), the installed-programs list in the
// registry, and running processes — nothing is launched, nothing is changed.
struct DiscoveredApp {
    QString name;
    QString exePath;                 // absolute, native separators, exists
    QString source;                  // "Start menu", "Installed programs", "Running"
};

QList<DiscoveredApp> discoverApps();

// Helpers (pure): whether a target looks like an installer/uninstaller/updater rather than the program.
bool looksLikeHelperExecutable(const QString& exePath);
// "C:\x\app.exe,0" / "\"C:\x\app.exe\"" -> "C:\x\app.exe" (DisplayIcon / UninstallString style values).
QString executableFromRegistryValue(const QString& value);

} // namespace gf::profiles
