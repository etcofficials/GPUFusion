#pragma once

#include <QString>

namespace gf {

// Where GPU Fusion keeps its writable data (settings, logs, benchmarks, reports).
//
// Resolution order (first match wins):
//   1. --data-dir=<path> on the command line
//   2. a "GPUFusion.datadir" file next to the executable whose first non-empty line is a
//      path (relative paths are resolved against the executable's directory);
//      development builds use this to point at G:\GPUFusion\data
//   3. "<exe dir>\data" (portable layout used by the release package)
// If the chosen location cannot be created or written, the per-user application data
// folder is used instead and the fallback is reported (it is normally on C:).
struct DataLocation {
    enum class Source { CommandLine, PointerFile, Portable, Fallback };

    QString root;
    Source source = Source::Portable;
    QString problem; // non-empty when the preferred location was rejected

    QString logsDir() const;
    QString benchmarksDir() const;
    QString reportsDir() const;
    QString configDir() const;
    QString tempDir() const;       // process TEMP/TMP (keeps driver shader caches off the system drive)
    QString settingsFile() const;
};

QString describe(DataLocation::Source source);

inline constexpr const char* kDataDirPointerFile = "GPUFusion.datadir";

// Pure resolution logic; fallbackRoot is injected so tests do not touch the user profile.
DataLocation resolveDataLocation(const QString& exeDir, const QString& commandLineOverride,
                                 const QString& fallbackRoot);

// Creates root and its standard sub-folders; returns an error message on failure.
QString ensureDataLayout(const DataLocation& location);

} // namespace gf
