#pragma once

#include "benchmarks/BenchmarkModel.h"
#include "hardware/AdapterModel.h"
#include "workloads/HashKernel.h"

#include <atomic>
#include <functional>

namespace gf::bench {

struct BenchmarkSpec {
    QString id;            // test id; GPU tests run once per adapter
    QString name;
    QString category;
    QString description;
    quint64 luid = 0;      // adapter for GPU tests, 0 otherwise
    Target target;
    QString uid() const { return luid ? QStringLiteral("%1@%2").arg(id, hw::formatLuid(luid)) : id; }
};

struct RunOptions {
    int warmupRuns = 1;
    int measuredRuns = 5;
    work::HashParams hash;
    quint64 transferBytes = 32ull << 20;
    QString probePath;
    QString preset = QStringLiteral("standard"); // recorded with the session
};

// Workload sizes offered in the UI and CLI (recorded in each result's parameters).
work::HashParams hashPreset(const QString& name); // "quick", "standard", "heavy"

QList<BenchmarkSpec> availableBenchmarks(const hw::DetectionResult& detection, const QString& probePath);

// Runs benchmarks synchronously (call it on a worker thread). `progress(done, total, step)` is
// called between runs; `cancel` is checked between runs — a measured run is never interrupted.
using Progress = std::function<void(int done, int total, const QString& step)>;
Session runBenchmarks(const QList<BenchmarkSpec>& specs, const RunOptions& options, const hw::DetectionResult& detection,
                      const Progress& progress, const std::atomic<bool>* cancel);

// Shared with the Workload Lab (LabRunner).
Session beginSession(const hw::DetectionResult& detection); // id, time, host facts, adapters
Target gpuTarget(const hw::Adapter& adapter);                // model + driver identity
QJsonObject hashParamsJson(const work::HashParams& p);

// Host facts recorded with a session.
QString cpuName();
double installedRamGB();

} // namespace gf::bench
