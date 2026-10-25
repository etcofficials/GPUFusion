#pragma once

#include "monitoring/MetricModel.h"
#include "monitoring/UsageVerification.h"

#include <QHash>
#include <QStringList>

#include <atomic>
#include <functional>

namespace gf::mon {

// Launches a program (with the user's normal environment) and records, once per interval, the GPU
// engine activity of its process tree — the launched process and every process it starts — on each
// physical adapter: Windows GPU performance counters for WDDM 2.0+ adapters, D3DKMT per-process
// statistics for older ones. Fills the launch and observation parts of UsageEvidence; the caller adds
// what was requested and what is stored. Runs synchronously: call it from a worker thread.
struct ObserveOptions {
    QString exe;
    QStringList arguments;
    QHash<QString, QString> environment;   // extra variables for the launched program
    int seconds = 20;
    int intervalMs = 1000;
};

using ObserveProgress = std::function<void(int pass, int passes, int processes)>;

UsageEvidence observeUsage(const QList<MonitorTarget>& targets, const ObserveOptions& options,
                           const std::atomic<bool>* cancel = nullptr, const ObserveProgress& progress = {});

} // namespace gf::mon
