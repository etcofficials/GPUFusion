#pragma once

// Monitoring data types and the pure logic behind them. A reading is never a bare number: it
// carries a state, so a metric that cannot be read is never displayed (or graphed) as zero.

#include "hardware/AdapterModel.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::mon {

struct MetricValue {
    enum class State {
        Ok,          // a real reading (0 is a real zero)
        Unsupported, // this adapter/driver/API does not provide the metric (permanent)
        Unavailable, // should be readable but the read failed or the source is off (transient)
        Unknown      // not determined yet (e.g. a rate needs two samples)
    };
    State state = State::Unknown;
    double value = 0.0; // meaningful only when state == Ok
    QString note;       // why it is not Ok, or where an Ok value came from

    static MetricValue ok(double v, QString source = {}) { return { State::Ok, v, std::move(source) }; }
    static MetricValue unsupported(QString why) { return { State::Unsupported, 0.0, std::move(why) }; }
    static MetricValue unavailable(QString why) { return { State::Unavailable, 0.0, std::move(why) }; }
    static MetricValue unknown(QString why) { return { State::Unknown, 0.0, std::move(why) }; }

    bool isOk() const { return state == State::Ok; }
    double valueOrNaN() const; // NaN unless Ok — used for graphs (NaN draws a gap)
};

enum class Unit { Percent, Bytes, Celsius, Watts, MHz, PState };

QString stateName(MetricValue::State state);
// "42 %", "355 MB", "40 °C", "P8" — or the state name ("Unsupported", ...) when not Ok.
QString formatMetric(const MetricValue& value, Unit unit);

// Busy share of one engine between two samples of its cumulative running time (100 ns units).
// Unknown on the first sample or when the counter went backwards (driver reset); clamped to 100.
MetricValue utilizationFromRunningTime(std::optional<quint64> previousRunning, quint64 currentRunning,
                                       qint64 elapsed100ns);

// Adapter utilization as Task Manager defines it: the busiest engine.
MetricValue busiestEngine(const QList<MetricValue>& engines);

struct SegmentUsage {
    bool aperture = false;   // true = system memory mapped for the GPU ("shared")
    quint64 bytesResident = 0;
};
struct MemoryUsage { quint64 dedicated = 0; quint64 shared = 0; };
MemoryUsage splitSegments(const QList<SegmentUsage>& segments);

// DXGK_ENGINE_TYPE value -> display name ("3D", "Video Decode", ...); "Engine N" when unknown.
QString engineTypeName(int engineType, int node);
// Appends " 1", " 2", ... to names that occur more than once ("Video Decode 1", "Video Decode 2").
QStringList disambiguate(const QStringList& names);

bool isStale(qint64 lastSampleMs, qint64 nowMs, int intervalMs);
int historyCapacity(int historySeconds, int intervalMs);

// ---- snapshot types ---------------------------------------------------------------------------

// What the monitor needs to know about an adapter (from hardware detection).
struct MonitorTarget {
    quint64 luid = 0;
    QString name;
    quint32 vendorId = 0, deviceId = 0;
    std::optional<hw::PciLocation> location;
    quint64 dedicatedTotal = 0;   // DXGI DedicatedVideoMemory
    quint64 sharedLimit = 0;      // DXGI SharedSystemMemory
    int wddmVersion = 0;          // KMT_DRIVERVERSION (e.g. 1200, 2300); 0 = unknown
    bool software = false;        // WARP: never shown as a GPU, only used to spot software rendering per process
};
// All DXGI adapters; software ones are flagged. Adapter-level monitoring skips software targets.
QList<MonitorTarget> targetsFrom(const hw::DetectionResult& detection);

struct EngineSample {
    int node = 0;
    QString name;
    MetricValue utilization;
};

struct AdapterSample {
    quint64 luid = 0;
    QString name;
    MetricValue utilization;      // busiest engine
    QList<EngineSample> engines;
    MetricValue dedicatedUsed, sharedUsed;
    quint64 dedicatedTotal = 0, sharedLimit = 0;
    MetricValue temperature, fanPercent, powerWatts, coreClockMHz, memoryClockMHz, pstate;
    QStringList sources;          // e.g. "D3DKMT", "NVML"
};

struct Snapshot {
    quint64 sequence = 0;
    QDateTime wallTime;
    qint64 steadyMs = 0;          // monotonic clock, for graphs and staleness
    QList<AdapterSample> adapters;
    double collectionMs = 0.0;    // time spent collecting this snapshot
    MetricValue processCpuPercent; // GPU Fusion's own CPU use (all threads), recent window
};

struct ProviderStatus {
    QString name;                 // "D3DKMT statistics", "NVIDIA NVML"
    bool active = false;
    QString detail;               // version, path, or why inactive
    QStringList unsupported;      // metrics the provider reported as not supported
};

} // namespace gf::mon

Q_DECLARE_METATYPE(gf::mon::Snapshot)
// ProcessSnapshot's metatype is declared in ProcessModel.h.
Q_DECLARE_METATYPE(QList<gf::mon::MonitorTarget>)
Q_DECLARE_METATYPE(QList<gf::mon::ProviderStatus>)
