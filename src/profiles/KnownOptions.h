#pragma once

// Optional suggestions for programs that have their own command-line option for choosing a GPU.
// GPU Fusion works without them (profiles are generic); a suggestion only fills the launch-arguments
// field with that program's option and a GPU placeholder (see AppProfile.h). Built-in entries say what
// evidence they rest on; users can add their own in <data>\config\gpu-options.json:
//   [ { "executables": ["tool.exe"], "arguments": "--gpu {dxgi_index}", "note": "..." } ]
// Pure except loadKnownOptions (reads that file).

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::profiles {

struct KnownGpuOption {
    QStringList executables;   // file names, case-insensitive
    QString program;           // "Chromium-based browsers"
    QString arguments;         // inserted before the profile's other arguments
    QString evidence;          // what it rests on (verified where, or documented where)
    QString caveat;            // e.g. "close every window first"
    bool userDefined = false;
};

QList<KnownGpuOption> builtInKnownOptions();
// Built-in entries plus the user's file (user entries first, so they take precedence). Invalid
// entries in the file are skipped and reported in *problems.
QList<KnownGpuOption> loadKnownOptions(const QString& userFile, QStringList* problems = nullptr);
std::optional<KnownGpuOption> knownOptionFor(const QString& exePath, const QList<KnownGpuOption>& options);
// Whether a program is built on Chromium (browsers, Electron and CEF apps), judged by the runtime files
// Chromium ships next to the executable or in a version folder beside it (icudtl.dat plus a V8 snapshot).
// Returns the files that showed it, or an empty list.
QStringList chromiumRuntimeFiles(const QString& exePath);
// knownOptionFor by name, else — for programs detected as Chromium-based — the Chromium switch
// (marked as detected, not verified for that program).
std::optional<KnownGpuOption> suggestedOptionFor(const QString& exePath, const QList<KnownGpuOption>& options);
// The arguments with the option put first, unless they already contain it.
QString withKnownOption(const QString& arguments, const KnownGpuOption& option);

} // namespace gf::profiles
