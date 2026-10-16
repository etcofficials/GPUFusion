#pragma once

// Benchmark results, statistics and comparisons — pure and unit-tested.
// Rules encoded here: results are compared only with an earlier result of the *same* test version,
// parameters, target (GPU model + driver, or CPU) — never across different workloads; a difference
// is called faster/slower only when it is larger than the measured run-to-run variation.

#include "monitoring/MetricModel.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::bench {

struct Stats {
    int n = 0;
    double mean = 0, stddev = 0, min = 0, max = 0, median = 0; // stddev: sample standard deviation
    double cv() const { return mean > 0 ? stddev / mean : 0; }  // coefficient of variation
};
Stats computeStats(QList<double> values);

struct RunSample {
    double durationMs = 0;
    QMap<QString, double> metrics; // e.g. "gpuMs", "readbackMs", "uploadMBps"
    bool correct = true;
    QString note;
};

// What a benchmark ran on. For GPUs the identity is model + driver (LUIDs change between boots).
struct Target {
    QString name;          // "NVIDIA GeForce GT 730" or "CPU: Intel(R) Core(TM) i5-2400S"
    quint32 vendorId = 0, deviceId = 0, subSysId = 0;
    QString driverVersion;
    QString key() const;
};

enum class Status { Completed, Failed, Skipped, Cancelled };
QString statusName(Status s);

struct BenchmarkResult {
    QString testId;        // stable id, e.g. "gpu.hash"
    int testVersion = 1;   // bump when the measured work changes
    QString name;
    QString category;
    QString measures;      // what durationMs is, in words
    Target target;
    QString api;           // "Direct3D 11 (FL 10_1)", "CPU, 4 threads", ...
    QJsonObject params;
    int warmupRuns = 0, measuredRuns = 0;
    QList<RunSample> samples;
    Stats duration;
    QMap<QString, Stats> metrics;
    std::optional<bool> correct; // nullopt = not checked
    QString correctness;
    mon::MetricValue cpuPercent, gpuBusyPercent, peakWorkingSetMB, gpuDedicatedMB;
    QStringList warnings;
    QStringList notes;     // supporting observations (e.g. which GPU engine was busy)
    QString error;
    Status status = Status::Completed;
    QDateTime started;
    double totalMs = 0;

    QString comparableKey() const;
    QJsonObject toJson() const;
    static std::optional<BenchmarkResult> fromJson(const QJsonObject& o);
};

// Fills duration/metrics statistics and `correct` from the samples.
void summarize(BenchmarkResult& r);

struct Session {
    QString id;            // "20261016-213000-000"
    QDateTime started;
    QString appVersion, os, cpu;
    double ramGB = 0;
    QStringList adapters;
    QList<BenchmarkResult> results;

    QJsonObject toJson() const;
    static std::optional<Session> fromJson(const QJsonObject& o);
};

struct Comparison {
    enum class Verdict { NoBaseline, Faster, Slower, NoSignificantChange };
    Verdict verdict = Verdict::NoBaseline;
    double deltaPercent = 0;   // (current - baseline) / baseline, on mean duration
    QDateTime baselineTime;
    QString text;
};
// Most recent earlier completed result with the same comparable key (sessions in any order).
std::optional<std::pair<BenchmarkResult, QDateTime>> findBaseline(const BenchmarkResult& r, const QList<Session>& history,
                                                                   const QString& excludeSessionId);
Comparison compare(const BenchmarkResult& current, const std::optional<std::pair<BenchmarkResult, QDateTime>>& baseline);

// The significance rule used everywhere: `current` differs from `reference` only when the means differ
// by more than twice the combined standard error (Welch) and by at least 1 %.
struct Difference {
    enum class Verdict { Faster, Slower, NoSignificantChange };
    Verdict verdict = Verdict::NoSignificantChange;
    double speedup = 1;       // reference.mean / current.mean (> 1: current is faster)
    double deltaPercent = 0;  // (current - reference) / reference
};
Difference compareStats(const Stats& current, const Stats& reference);

// Plain-language comparisons between targets that did identical, verified work in one session
// (the hash workload on each GPU and on the CPU; transfers of the same size). Results that failed,
// were cancelled or produced wrong output are left out. Ratios are of mean durations; differences
// within twice the combined standard error are reported as "about the same".
QStringList sessionSummary(const Session& s);

// One CSV row per result (same columns for every session, so files can be appended).
QString csvHeader();
QString csvRows(const Session& s);
QString csvField(const QString& value);

// Files (path, modification time) older than the retention period; 0 days = keep everything.
QStringList filesToPrune(const QList<std::pair<QString, QDateTime>>& files, const QDateTime& now, int retentionDays);

} // namespace gf::bench
