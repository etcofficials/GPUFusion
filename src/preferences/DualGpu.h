#pragma once

// Can a given application use both GPUs? Answers four different questions that are easily confused:
//   A. one rendering workload distributed across two GPUs (D3D12 explicit multi-adapter, Vulkan
//      device groups, SLI/CrossFire) — needs support in the application *and* suitable GPUs;
//   B. one application using two GPUs for different tasks (e.g. render on one, compute or video on
//      the other) — only if the application offers a device choice per task;
//   C. two separate applications on different GPUs — needs each application to be placed;
//   D. a game rendering on one GPU while another program computes on the other.
// No Windows setting turns an ordinary single-GPU program into a multi-GPU one; this assessment
// says so plainly. Pure; unit-tested.

#include <QString>
#include <QStringList>

#include <optional>

namespace gf::pref {

struct GraphicsApis {
    bool d3d9 = false, d3d11 = false, d3d12 = false, dxgi = false, opengl = false, vulkan = false, cuda = false, opencl = false;
    QStringList names() const;
};
GraphicsApis apisFromImports(const QStringList& importedDlls);

struct MultiGpuSystem {
    int physicalGpus = 0;
    int d3d12Gpus = 0;                // physical GPUs where a D3D12 device could be created
    bool sameVendor = false;          // all physical GPUs from one vendor (SLI/CrossFire/device groups need this)
    bool preferenceMovesApps = false; // verified: Windows' preference places programs on either GPU
    QStringList gpusWithoutD3D12;
};

enum class DualGpuSupport { NotPossible, OnlyIfApplicationImplementsIt, Unknown };

struct DualGpuAssessment {
    DualGpuSupport support = DualGpuSupport::Unknown;
    QString verdict;                  // one sentence about this application on this PC
    QStringList cases;                // A–D, each answered for this PC / application
    QStringList evidence;
};

// `imports`: the executable's imported DLLs (nullopt if they could not be read, with `importError`).
DualGpuAssessment assessDualGpu(const QString& exeName, const std::optional<QStringList>& imports, const QString& importError,
                                const MultiGpuSystem& system);

} // namespace gf::pref
