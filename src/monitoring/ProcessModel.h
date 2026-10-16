#pragma once

// Per-process GPU usage: data types and pure logic. Two sources feed it:
//  - Windows performance counters (PDH "GPU Engine" / "GPU Process Memory") for WDDM 2.0+ adapters;
//    they cover every process without opening it.
//  - D3DKMT per-process statistics for WDDM 1.x adapters; they need a process handle with
//    PROCESS_QUERY_INFORMATION, so without administrator rights only the user's own processes count.
// Per-process figures are kept separate from adapter-wide totals (MetricModel.h).

#include "monitoring/MetricModel.h"
#include "preferences/PreferenceModel.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace gf::mon {

// "pid_1344_luid_0x00000000_0x00007EDA_phys_0_eng_1_engtype_Compute_0"
struct PdhEngineInstance {
    quint32 pid = 0;
    quint64 luid = 0;
    int phys = 0;
    int engine = 0;
    QString engineType; // as Windows names it: "3D", "Compute_0", "VideoDecode", "Copy", ...
};
std::optional<PdhEngineInstance> parsePdhEngineInstance(const QString& name);

// "pid_1344_luid_0x00000000_0x00007EDA_phys_0"
struct PdhProcessInstance {
    quint32 pid = 0;
    quint64 luid = 0;
    int phys = 0;
};
std::optional<PdhProcessInstance> parsePdhProcessInstance(const QString& name);

// Per-process usage of one adapter.
struct ProcessAdapterUsage {
    quint64 luid = 0;
    MetricValue utilization;      // busiest engine for this process on this adapter
    QString busiestEngine;
    QList<EngineSample> engines;  // engines with a reading for this process
    MetricValue dedicated, shared;
    QString source;               // "Windows performance counters" or "D3DKMT"
};

enum class Activity {
    Active,      // engine time in the latest sample
    MemoryOnly,  // holds GPU memory but did not use an engine in the latest sample
    None
};
// A process counts as using the GPU only from engine time; holding memory alone is not usage.
Activity activityOf(const ProcessAdapterUsage& usage);
QString activityText(Activity a);
constexpr double kActiveThresholdPercent = 0.1;

struct ProcessSample {
    quint32 pid = 0;
    QString name;                 // executable name (always known)
    QString path;                 // full path, empty if it could not be read
    QString pathError;            // why the path is missing
    QList<ProcessAdapterUsage> adapters; // only adapters with any engine reading or memory

    const ProcessAdapterUsage* usageOn(quint64 luid) const;
};

struct ProcessSnapshot {
    QDateTime wallTime;
    qint64 steadyMs = 0;
    QList<ProcessSample> processes;   // every process found, including those without GPU usage
    int processCount = 0;
    // Coverage of the D3DKMT (WDDM 1.x) path: processes that could not be opened are not measured there.
    int legacyInspectable = 0;
    int legacyNotInspectable = 0;
    QStringList legacyAdapterNames;
    QStringList pdhAdapterNames;
    QString pdhError;                 // non-empty if the performance counters could not be read
    double collectionMs = 0;
    // False for a priming sample: utilization needs a previous sample, so it is Unknown everywhere
    // and ordering by it is meaningless.
    bool ratesReady = false;
};

// ---- aggregation helpers (pure) ---------------------------------------------------------------

struct PdhEngineReading { PdhEngineInstance instance; MetricValue utilization; };
struct PdhMemoryReading { PdhProcessInstance instance; MetricValue dedicated, shared; };

// Groups PDH readings into per-(process, adapter) usage for the given adapters.
QHash<quint32, QList<ProcessAdapterUsage>> aggregatePdh(const QList<PdhEngineReading>& engines,
                                                        const QList<PdhMemoryReading>& memory,
                                                        const QList<quint64>& adapterLuids);

// Display name of a Windows engine type from PDH ("VideoDecode" -> "Video Decode", "3D" -> "3D").
QString pdhEngineDisplayName(const QString& engineType, int engine);

// ---- Windows per-application graphics preference (defined in preferences/) ----------------------
using pref::GpuPreference;
using pref::parseGpuPreference;
using pref::gpuPreferenceText;

} // namespace gf::mon

Q_DECLARE_METATYPE(gf::mon::ProcessSnapshot)
