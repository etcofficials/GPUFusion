#pragma once

// Compatibility registry: for any program, what GPU Fusion can do with it on this PC, answered
// separately for six capabilities that must never be confused:
//   SplitFrames      rendering one program's frames across several GPUs
//   SplitWorkloads   one program running independent workloads on different GPUs
//   MoveOffGameGpu   moving this program's GPU work off the GPU a game uses (to the other GPU)
//   SelectGpu        choosing which GPU the program uses
//   ConcurrentJobs   independent jobs of this program running on both GPUs at once
//   Scheduling       better performance through workload scheduling / resource management
// Each answer has a status (Supported = tested and verified; Experimental = implemented, not yet
// validated; Partial = only some features or workloads; Unsupported = not possible through the
// documented interfaces on this hardware; NotTested = not established) and its reasons.
//
// The answers come from (1) what the program is, read from its files without starting it: imported
// graphics APIs, engine and runtime (Chromium/Electron, Java, Unity, Unreal, Godot, ...), packaging,
// GPU Fusion SDK use; (2) facts about this PC (which GPUs have Direct3D 12, Vulkan, whether Windows'
// preference can place programs on each GPU); (3) registry entries with evidence: built in, and the
// user's own in <data>\config\compatibility.json; (4) what GPU Fusion verified for this program. New
// programs, engines and mechanisms are added as entries or detectors, not by changing the classifier.
// Pure except where noted (file reads).

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::hw { struct DetectionResult; }

namespace gf::compat {

enum class Capability { SplitFrames, SplitWorkloads, MoveOffGameGpu, SelectGpu, ConcurrentJobs, Scheduling };
enum class Status { Supported, Experimental, Partial, Unsupported, NotTested };
QList<Capability> allCapabilities();
QString capabilityKey(Capability c);     // "split-frames", ...
QString capabilityText(Capability c);    // in words
QString statusKey(Status s);             // "supported", ...
QString statusText(Status s);            // "Supported", ...
std::optional<Capability> capabilityFromKey(const QString& key);
std::optional<Status> statusFromKey(const QString& key);

// What a program is, from its files.
struct ProgramFacts {
    QString exePath, exeName;            // exeName lower-case
    bool importsRead = false;
    QString importError;
    QStringList imports;                 // lower-case DLL names
    bool d3d9 = false, d3d11 = false, d3d12 = false, dxgi = false, opengl = false, vulkan = false, cuda = false, opencl = false;
    QStringList runtimes;                // "Chromium", "Electron", "Java", "Unity", "Unreal Engine", "Godot", "SDL", ".NET", "GPU Fusion SDK"
    bool storePackage = false;           // Microsoft Store / Xbox package
    bool usesGpuFusionSdk = false;
    QStringList apis() const;            // "Direct3D 11", ...
};
// Reads imports and looks at the files around the executable (no program is started).
ProgramFacts examine(const QString& exePath);
// The same from already-known pieces (for tests and callers that have them).
ProgramFacts factsFrom(const QString& exePath, const std::optional<QStringList>& imports, const QStringList& filesBeside);

// Facts about this PC.
struct SystemFacts {
    int physicalGpus = 0;
    QStringList gpus;                    // names, the discrete one first
    QStringList d3d12Gpus;               // GPUs with a Direct3D 12 device
    QStringList vulkanGpus;              // GPUs with a Vulkan driver (vulkan-1.dll present and an ICD for them)
    QString integratedGpu, discreteGpu;  // names; empty if absent
    bool integratedHasD3D11Fl11 = false; // compute shaders / feature level 11_0 on the integrated GPU
    std::optional<bool> preferenceReachesIntegrated; // verified: Windows' preference can put programs on the integrated GPU
};
// From GPU Fusion's adapter detection. `preferenceReachesIntegrated`: from the preference verification
// (nullopt when it was not run). Vulkan: only when Windows' Vulkan loader exists, and then per GPU
// only as "not established" (no Vulkan device is created here).
SystemFacts systemFacts(const hw::DetectionResult& detection, std::optional<bool> preferenceReachesIntegrated);

// One registry entry: matched by executable names (lower-case), runtimes or APIs; sets statuses.
struct Entry {
    QString id, program;
    QStringList executables;             // exact file names, lower-case
    QStringList runtimes;                // any of these runtimes (e.g. "Electron")
    QHash<Capability, Status> status;
    QHash<Capability, QString> reason;
    QString mechanism;                   // how GPU Fusion does SelectGpu / MoveOffGameGpu, if it can
    QString evidence;                    // what the entry rests on
    bool userDefined = false;
};
QList<Entry> builtInEntries();
// Built-in entries plus the user's file (user entries first: they take precedence). Problems with
// the file are reported, never fatal.
QList<Entry> loadEntries(const QString& userFile, QStringList* problems = nullptr);
QJsonObject entryToJson(const Entry& e);
std::optional<Entry> entryFromJson(const QJsonObject& o, QString* problem = nullptr);

struct Answer {
    Capability capability = Capability::SelectGpu;
    Status status = Status::NotTested;
    QString summary;                     // one sentence
    QStringList reasons;
    QString source;                      // "classifier", "registry: <id>", "verified on this PC"
};
struct Assessment {
    ProgramFacts program;
    QList<Answer> answers;               // one per capability, in allCapabilities() order
    QStringList matchedEntries;
    const Answer& answer(Capability c) const;
};

// What GPU Fusion itself verified for this program (e.g. from a profile's verification).
struct ProgramEvidence {
    std::optional<bool> ranOnIntegrated; // verified to render on the integrated GPU (true) or not (false)
    std::optional<bool> ranOnDiscrete;
    QString note;
};

Assessment assess(const ProgramFacts& program, const SystemFacts& system, const QList<Entry>& entries, const ProgramEvidence& evidence = {});

// A table of assessments as an HTML page (for reports) and as plain lines (for logs).
QString assessmentsHtml(const QList<Assessment>& list, const SystemFacts& system);
QStringList assessmentLines(const Assessment& a);

} // namespace gf::compat
