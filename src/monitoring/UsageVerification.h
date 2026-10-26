#pragma once

// Did an application really run on the GPU the user asked for? Evaluates what was observed while
// the application (and its child processes) ran, in stages that are reported separately:
//   1. preference saved (read back from Windows)   2. application launched
//   3. GPU engine activity observed for its processes   4. the requested adapter confirmed
// Rendering is counted only from 3D and compute engines of the application's own processes —
// never from total GPU load, video decode/encode, copy engines, or other processes (such as the
// desktop compositor). Where a driver does not name its engines (WDDM 1.x), activity on that
// adapter is reported as "type unknown" and cannot prove rendering. Pure; unit-tested.

#include "monitoring/MetricModel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::mon {

enum class EngineKind { Render3D, Compute, Video, Copy, Unnamed, Other };
EngineKind classifyEngine(const QString& name); // "3D", "Compute_0", "Video Decode 2", "Copy", "Engine 0", ...

// One observation of one process of the application tree on one adapter.
struct UsageObservation {
    int pass = 0;                      // sampling pass index (0-based)
    quint32 pid = 0;
    QString exe;                       // full path when readable, else the process name
    quint64 luid = 0;
    QList<EngineSample> engines;       // per-engine utilisation of this process on this adapter
};

struct ObservedAdapter {
    quint64 luid = 0;
    QString name;
};

struct UsageEvidence {
    QString targetExe;
    std::optional<quint64> requestedLuid;   // nullopt: no specific adapter requested (Automatic)
    QString requestedName;
    std::optional<QString> storedPreference; // read back after saving; nullopt = no value stored
    bool preferenceRequired = true;          // false for Automatic (nothing has to be stored)
    bool launched = false;
    QString launchError;
    bool alreadyRunning = false;             // an instance was running before the launch
    int passes = 0;                          // sampling passes taken
    QList<ObservedAdapter> adapters;
    QList<UsageObservation> observations;    // all passes
    // Processes of the tree whose GPU use on pre-WDDM 2.0 adapters could not be read (D3DKMT needs a
    // process handle: other accounts' and protected processes cannot be opened). Executable paths.
    QStringList uninspected;
    QStringList legacyAdapters;              // names of the adapters read through D3DKMT
};

enum class StageState { Passed, Failed, Inconclusive, NotApplicable };
QString stageStateText(StageState s);

struct UsageStage {
    QString name;
    StageState state = StageState::Inconclusive;
    QString detail;
};

enum class UsageOutcome {
    Confirmed,          // rendering seen only on the requested adapter
    ConfirmedUnnamed,   // GPU work seen only on the requested adapter, whose driver does not name engines
    WrongAdapter,       // rendering seen on another adapter, none on the requested one
    BothAdapters,       // rendering seen on the requested adapter and another one
    Observed,           // nothing was requested (Automatic): where it ran is reported
    NoActivity,         // the application did no measurable GPU work while observed
    NotLaunched
};

struct AdapterActivity {
    quint64 luid = 0;
    QString name;
    double renderPeak = 0, renderMean = 0;   // 3D + compute, % (max over engines per pass)
    double videoPeak = 0, copyPeak = 0;
    double unnamedPeak = 0;                  // engines without a type (WDDM 1.x)
    QStringList executables;                 // processes of the tree that did GPU work here
};

struct UsageVerdict {
    UsageOutcome outcome = UsageOutcome::NoActivity;
    QList<UsageStage> stages;
    QList<AdapterActivity> activity;         // per adapter with any activity
    QString headline;
    QStringList details;
    QStringList renderingExecutables;        // which executables of the tree rendered (launcher resolution)
};

// Thresholds: an adapter "renders" for the application if its render activity reached
// kRenderPeakPercent in at least kRenderPasses passes.
inline constexpr double kRenderPeakPercent = 2.0;
inline constexpr int kRenderPasses = 2;

UsageVerdict evaluateUsage(const UsageEvidence& evidence);

} // namespace gf::mon
