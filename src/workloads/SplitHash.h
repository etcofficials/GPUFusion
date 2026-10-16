#pragma once

#include "workloads/D3D11HashDevice.h"
#include "workloads/SplitPlan.h"

#include <QString>

#include <vector>

namespace gf::work {

// Runs the hash workload with its rows divided between several workers at once — GPUs (each with
// its own Direct3D 11 device and thread) and optionally the CPU — and assembles the whole image in
// CPU memory. The two GPUs here share nothing (different vendors, WDDM 1.2), so every GPU's rows
// travel back through system memory; that transfer is part of the measured time.
struct SplitWorker {
    QString label;
    D3D11HashDevice* gpu = nullptr; // a GPU worker, or
    unsigned cpuThreads = 0;        // a CPU worker with this many threads (gpu == nullptr)
};

struct SplitSpec {
    enum class Mode { Static, Dynamic };
    Mode mode = Mode::Static;
    std::vector<double> shares;     // Static: share of the rows for each worker
    std::uint32_t align = 16;       // Static: range boundaries are multiples of this
    std::uint32_t chunkRows = 32;   // Dynamic: rows per work item; workers take the next item when free
};

struct WorkerReport {
    std::uint32_t rows = 0;
    int items = 0;
    double busyMs = 0;     // from the common start until this worker finished its last item
    double computeMs = 0;  // sum over items: GPU draw-to-finish (GPU) or computation (CPU)
    double readbackMs = 0; // sum over items: copy from GPU memory to the image (GPU only)
};

struct SplitResult {
    bool ok = false;
    QString error;
    double wallMs = 0;                 // common start -> the whole image is in CPU memory
    std::vector<WorkerReport> workers; // same order as the workers passed in
    std::vector<std::uint32_t> image;  // width * height values
};

SplitResult runSplit(const HashParams& p, const std::vector<SplitWorker>& workers, const SplitSpec& spec);

} // namespace gf::work
