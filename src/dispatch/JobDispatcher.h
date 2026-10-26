#pragma once

// Generic multi-GPU job dispatcher (capability class C: independent work on several GPUs at once).
// A batch of independent jobs (one per input file) is spread over the chosen GPUs through a shared
// queue: every GPU's worker takes the next job as soon as it is free, so a faster GPU simply does more
// jobs. What a job does is an Executor:
//   - a command template for any program, with placeholders for the GPU and the files (the program
//     must offer its own way to pick a GPU, e.g. an adapter index option, or integrate the GPU Fusion
//     SDK, whose variables are set per job);
//   - built-in: H.264 decoding with Media Foundation on the job's GPU.
// compareConfigurations() measures the same batch on each GPU alone and on all of them, checks that
// every job succeeded and produced the same output as on that GPU alone, and only calls the
// multi-GPU run faster when the benchmark significance rule says so.

#include "benchmarks/BenchmarkModel.h"
#include "hardware/AdapterModel.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

namespace gf::dispatch {

struct Worker {
    QString name;
    quint32 vendorId = 0;
    quint64 luid = 0;
    int dxgiIndex = -1;       // DXGI enumeration index (what many programs' "adapter" options take)
    bool integrated = false;
};
// Physical GPUs in DXGI order.
QList<Worker> workersFrom(const hw::DetectionResult& detection);
// "all" | "integrated" | "discrete" | comma-separated DXGI indexes -> indexes into `workers`.
std::optional<QList<int>> selectWorkers(const QList<Worker>& workers, const QString& spec, QString* error);

struct Job {
    int index = 0;
    QString input;
    QString output;           // empty when the job writes nothing
};
// One job per input; output = outputDir/<pattern with {stem}/{name}/{ext}/{job} expanded> (empty
// pattern: no output file).
QList<Job> jobsFor(const QStringList& inputs, const QString& outputDir, const QString& outputPattern);
// Files in a folder (non-recursive, sorted), or the given files.
QStringList inputFiles(const QString& folderOrFiles);

struct JobResult {
    int job = -1;
    int worker = -1;          // index into the worker list
    bool ok = false;
    double ms = 0;
    QString error;
    std::optional<QString> checksum; // identity of the job's output (file hash, frame count, ...)
};
using Executor = std::function<JobResult(const Job&, const Worker&, const std::atomic<bool>* cancel)>;

struct RunResult {
    QList<int> workers;       // which workers took part
    double wallMs = 0;
    QList<JobResult> jobs;    // by job index
    bool cancelled = false;
    int failed() const;
    QMap<int, int> jobsPerWorker() const;
};
using Progress = std::function<void(int done, int total)>;
// Runs every job once on the `use`d workers (one job at a time per entry; a worker listed twice runs
// two jobs at once; dynamic queue). Stragglers are avoided: a worker does not take the next job when
// another GPU would finish all remaining jobs before this worker finished that one — judged from the
// job times measured so far in this run, or from `expectedJobMs` (worker index -> ms per job, e.g.
// measured on that GPU alone) until then.
RunResult runJobs(const QList<Worker>& workers, const QList<int>& use, const QList<Job>& jobs, const Executor& executor,
                  const std::atomic<bool>* cancel = nullptr, const Progress& progress = {},
                  const QMap<int, double>& expectedJobMs = {});
// The straggler rule (pure): should a worker that needs `myMs` per job take one of `remaining` jobs
// when the fastest other worker needs `fastestOtherMs`?
bool shouldTakeJob(double myMs, double fastestOtherMs, int remaining);

// ---- executors ------------------------------------------------------------------------------------

// Placeholders: {dxgi_index} {luid} {luid_pair} {luid_high} {luid_low} {vendor_id} {vendor_hex} {gpu_name}
//               {input} {output} {stem} {job}   (the GPU ones mean the same as in profile arguments)
QString expandPlaceholders(const QString& text, const Worker& w, const Job& j);
// Splits the template like a command line first, then expands each argument (so paths with spaces
// stay one argument).
QStringList commandLine(const QString& commandTemplate, const Worker& w, const Job& j);
// Runs the command per job; also sets GPUFUSION_MODE/GPUFUSION_ADAPTERS for the job's GPU so
// SDK-integrated programs follow the assignment. ok = exit code 0 and, if the job has an output,
// a non-empty output file (its SHA-1 is the checksum).
Executor commandExecutor(const QString& commandTemplate, int timeoutMs = 30 * 60 * 1000);
// Decodes the input (H.264, e.g. MP4) with Media Foundation on the job's GPU; checksum = frame count.
// ok only if the frames were decoded on that GPU (DXVA), not in software.
Executor decodeExecutor();

// ---- measured comparison ----------------------------------------------------------------------------

struct ConfigResult {
    QString label;            // "NVIDIA GeForce GT 730 alone", "All 2 GPUs"
    QList<int> workers;
    QList<RunResult> runs;
    bench::Stats wall;        // of runs[].wallMs (successful runs only)
    bool allOk = false;       // every job of every run succeeded
};
struct Experiment {
    QList<ConfigResult> configs;      // each single GPU, then all chosen GPUs together
    std::optional<int> bestSingle;    // index into configs
    std::optional<bench::Difference> difference; // all-GPUs vs best single
    bool outputsConsistent = true;    // every job's output in the multi-GPU run equals the same GPU's output alone
    QString verdict;
    QStringList notes;
};
// Measures `repetitions` runs per configuration (outputs of each configuration go to its own
// sub-folder of outputDir). Needs at least two workers.
Experiment compareConfigurations(const QList<Worker>& workers, const QList<int>& use, const QStringList& inputs,
                                 const QString& outputDir, const QString& outputPattern, const Executor& executor,
                                 int repetitions, const std::atomic<bool>* cancel = nullptr,
                                 const std::function<void(const QString& stage)>& progress = {});
// The interpretation (pure; used by compareConfigurations and tests).
void analyze(Experiment& e, const QList<Worker>& workers);

} // namespace gf::dispatch
