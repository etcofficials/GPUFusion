#pragma once

// The diagnostic report: everything GPU Fusion knows about the graphics system, turned into
// plain-language issues with suggested actions, plus tables, rendered as Markdown and JSON.
// Pure: it only consumes data other modules already collected, so it is unit-tested with
// synthetic inputs and never probes or changes the system itself.

#include "hardware/AdapterModel.h"
#include "monitoring/MetricModel.h"
#include "monitoring/ProcessModel.h"
#include "preferences/PreferenceModel.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::diag {

struct Issue {
    enum class Severity { Error, Warning, Note };
    Severity severity = Severity::Note;
    QString subject;        // adapter name or area ("Monitoring", "Storage", ...)
    QString title;
    QString detail;
    QStringList actions;    // suggested next steps (may be empty)
};
QString severityName(Issue::Severity s);

struct OpenClInfo {
    bool loaderPresent = false;   // System32\OpenCL.dll
    QStringList icds;             // registered installable client drivers (file names only)
};

struct LogLine { QDateTime time; QString severity; QString subsystem; QString message; };

struct ReportInputs {
    QDateTime generated;
    QString appVersion, qtVersion, osDescription, buildDescription;
    std::optional<hw::DetectionResult> detection;
    std::optional<mon::Snapshot> snapshot;        // latest adapter readings (for metric availability)
    QList<mon::ProviderStatus> providers;
    bool monitoringPaused = false;
    std::optional<mon::ProcessSnapshot> processes; // only coverage figures are reported
    std::optional<pref::VerificationResult> preferenceVerification;
    int configuredPreferences = 0;
    QString dataRoot;
    bool dataFallback = false;
    QString dataProblem;
    QStringList settingsWarnings;
    OpenClInfo openCl;
    QList<LogLine> recentProblems;                 // recent WARN/ERROR log lines
};

// ---- multi-GPU compatibility ------------------------------------------------------------------

struct Technique {
    QString name;
    enum class Verdict { Feasible, NotFeasible, Unknown } verdict = Verdict::Unknown;
    QString reason;
};
QString verdictName(Technique::Verdict v);
// How the physical adapters could share work, judged from detected capabilities only.
QList<Technique> assessMultiGpu(const hw::DetectionResult& detection, const OpenClInfo& openCl);

// ---- metric availability ----------------------------------------------------------------------

struct MetricAvailability {
    QString metric;
    QList<mon::MetricValue> perAdapter; // same order as the adapters in the snapshot
};
QList<MetricAvailability> metricAvailability(const mon::Snapshot& snapshot);

// ---- report ---------------------------------------------------------------------------------------

// A table as plain text cells, shared by the Markdown export and the Diagnostics page.
struct Table {
    QStringList header;
    QList<QStringList> rows;
};

struct Report {
    ReportInputs inputs;
    QList<Issue> issues;                // most severe first
    QList<Technique> multiGpu;
    int count(Issue::Severity s) const;

    Table adapterTable() const;         // properties x non-software adapters
    Table metricTable() const;          // metrics x monitored adapters (availability)
    Table multiGpuTable() const;
    Table preferenceTable() const;      // preference -> GPU the probe used

    QString toMarkdown() const;
    QJsonObject toJson() const;
};

Report buildReport(const ReportInputs& inputs);

// Recent WARN/ERROR lines from GPU Fusion's log format ("<ISO time> WARN  [subsystem] (t123) message").
QList<LogLine> parseLogProblems(const QString& logText, int maxLines);

// ---- privacy ------------------------------------------------------------------------------------

// Removes personal identifiers from text before it leaves the machine: the user-profile folder
// becomes %USERPROFILE%, "COMPUTER\user" and the computer name become placeholders. The bare user
// name is not replaced on its own (it can be an ordinary word, e.g. "etc").
struct Redactor {
    QString userProfile;   // C:\Users\name
    QString userName;
    QString computerName;
    QString apply(const QString& text) const;
};
Redactor currentRedactor(); // from the environment of this process

} // namespace gf::diag
