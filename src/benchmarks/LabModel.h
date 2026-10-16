#pragma once

// Interpretation of Workload Lab sessions — pure and unit-tested. Every "faster"/"slower" uses the
// same significance rule as the benchmarks (compareStats); only correct, completed results count.

#include "benchmarks/BenchmarkModel.h"

namespace gf::bench {

struct LabRow {
    const BenchmarkResult* result = nullptr;
    bool reference = false;               // the row everything else is compared with
    std::optional<Difference> vsReference;
    std::optional<double> idealMs;        // split modes: zero-overhead time of a perfect split of the same workers
    QString detail;                       // work shares / throughput
};

struct LabAnalysis {
    const BenchmarkResult* reference = nullptr;
    QString referenceLabel;               // "NVIDIA GeForce GT 730 alone"
    QString headline;                     // one-sentence answer
    std::optional<Difference::Verdict> verdict; // of the headline comparison, when there is one
    QStringList findings;                 // supporting sentences
    QList<LabRow> rows;                   // in session order
};

LabAnalysis analyzeSplit(const Session& session);
LabAnalysis analyzeDecode(const Session& session);

// "1.15× faster", "1.30× slower", "no significant difference" — for tables.
QString describeDifference(const Difference& d);

} // namespace gf::bench
