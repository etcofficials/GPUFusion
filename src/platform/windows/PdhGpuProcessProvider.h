#pragma once

#include "monitoring/ProcessModel.h"

namespace gf::mon::win {

// Per-process engine utilization and memory from Windows' GPU performance counters ("GPU Engine",
// "GPU Process Memory"), the same source as Task Manager. Covers every process without opening it,
// but only adapters whose driver is WDDM 2.0 or newer. One PDH query is kept open and re-collected;
// rates (utilization) need two collections, so the first sample reports Unknown.
class PdhGpuProcessProvider {
public:
    PdhGpuProcessProvider();
    ~PdhGpuProcessProvider();
    PdhGpuProcessProvider(const PdhGpuProcessProvider&) = delete;
    PdhGpuProcessProvider& operator=(const PdhGpuProcessProvider&) = delete;

    bool isOpen() const { return m_query != nullptr; }
    bool isPrimed() const { return m_primed; } // a previous collection exists, so rates are valid
    QString error() const { return m_error; }
    // Collects and returns readings for all instances; empty lists on failure (see error()).
    bool collect(QList<PdhEngineReading>* engines, QList<PdhMemoryReading>* memory);

private:
    void* m_query = nullptr;
    void* m_engine = nullptr;
    void* m_dedicated = nullptr;
    void* m_shared = nullptr;
    bool m_primed = false;
    QString m_error;
};

} // namespace gf::mon::win
