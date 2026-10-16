#pragma once

#include "workloads/HashKernel.h"

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gf::work {

// A Direct3D 11 device on one specific adapter (by LUID) that runs the hash workload with a pixel
// shader (shader model 4.0, so FL 10_0+ adapters without compute shaders work too) and measures
// GPU <-> CPU transfers. Devices and render targets are created once and reused across runs.
class D3D11HashDevice {
public:
    ~D3D11HashDevice();
    static std::unique_ptr<D3D11HashDevice> create(quint64 luid, QString* error);

    QString adapterName() const { return m_adapterName; }
    QString featureLevel() const { return m_featureLevel; }

    struct RunResult {
        bool ok = false;
        QString error;
        double wallMs = 0;                 // first draw submitted -> GPU finished
        std::optional<double> gpuMs;       // GPU timestamp queries, when not disjoint
        double readbackMs = 0;             // copy to CPU memory (only when requested)
        std::vector<std::uint32_t> output; // width * rows values, when read back
    };
    // Computes rows [firstRow, firstRow + rows) of the image described by `p`.
    RunResult run(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, bool readback);

    // One unit of a split workload: computes rows [firstRow, firstRow + rows) and copies them into
    // `dest` (width * rows values). No extra synchronisation or timing queries. Returns an empty
    // string on success; `computeMs` / `readbackMs` receive the wall time of each phase.
    QString computeInto(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, std::uint32_t* dest,
                        double* computeMs = nullptr, double* readbackMs = nullptr);

    struct TransferResult {
        bool ok = false;
        QString error;
        double uploadMs = 0, readbackMs = 0;
        std::uint64_t bytes = 0;
        bool dataIntact = false;           // what came back equals what was sent
    };
    TransferResult transfer(std::uint64_t bytes);

private:
    D3D11HashDevice();
    struct Impl;
    std::unique_ptr<Impl> d;
    QString m_adapterName, m_featureLevel;
};

} // namespace gf::work
