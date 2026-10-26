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
// The arguments with the option put first, unless they already contain it.
QString withKnownOption(const QString& arguments, const KnownGpuOption& option);

} // namespace gf::profiles
