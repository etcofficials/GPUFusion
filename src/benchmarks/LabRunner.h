#pragma once

#include "benchmarks/BenchmarkRunner.h"
#include "workloads/VideoDecode.h"

namespace gf::bench {

// Workload Lab experiments. Each produces a Session of ordinary benchmark results (warm-up +
// measured runs, statistics, correctness), stored and compared like any other benchmark. During
// every mode, D3DKMT records which engine of each GPU was busy, as evidence of where work ran.
struct LabOptions {
    int warmupRuns = 1, measuredRuns = 5;
    QString preset = QStringLiteral("standard");
    work::HashParams hash;
    std::uint32_t chunkRows = 32; // dynamic split: rows per work item
    unsigned cpuThreads = 0;      // CPU threads in the "+ CPU" mode; 0 = all cores but one
    QString mediaDir;             // where the generated test video is kept
    work::TestClipSpec clip;
};

// Lab sessions live apart from the benchmark sessions: <benchmarks>\lab (test video in lab\media).
QString labDir(const QString& benchmarksDir);
LabOptions labOptions(int warmupRuns, int measuredRuns, const QString& preset, const QString& labDirectory);

// Experiment 1 — one hash image computed by several workers at once, assembled in CPU memory and
// checked bit-exactly against the CPU reference. Results (test ids):
//   lab.split.single       each GPU alone, and the helper CPU threads alone (same code path)
//   lab.split.equal        rows split evenly between the GPUs
//   lab.split.balanced     rows split in proportion to each GPU's speed in this session
//   lab.split.dynamic      GPUs take fixed-size items from a shared queue until done
//   lab.split.dynamic-cpu  the same, with CPU threads taking items too
Session runSplitExperiment(const hw::DetectionResult& detection, const LabOptions& options, const Progress& progress,
                           const std::atomic<bool>* cancel);

// Experiment 2 — H.264 decoding of the generated test video, as fast as possible. Every decoder's
// frames are first compared with the software decoder's (H.264 decoding is bit-exact by spec).
//   lab.decode.one         one stream on one decoder (each GPU, and software)
//   lab.decode.two-same    two streams at once on the same GPU (or both in software)
//   lab.decode.two-split   two streams at once, one on each GPU
Session runDecodeExperiment(const hw::DetectionResult& detection, const LabOptions& options, const Progress& progress,
                            const std::atomic<bool>* cancel);

} // namespace gf::bench
