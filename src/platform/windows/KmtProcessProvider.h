#pragma once

#include "monitoring/ProcessModel.h"
#include "platform/windows/ProcessList.h"

#include <QHash>

#include <vector>

namespace gf::mon::win {

// Per-process engine running time and memory from D3DKMT, for adapters whose drivers predate
// WDDM 2.0 (Windows' performance counters do not cover them). Needs PROCESS_QUERY_INFORMATION on
// each process, which a standard user only gets for their own processes; the others are counted
// as "not inspectable" rather than guessed. Verified in Phase 4 on the Intel HD 2000 (WDDM 1.2).
class KmtProcessProvider {
public:
    struct Adapter { quint64 luid = 0; QString name; };

    void setAdapters(const QList<Adapter>& adapters);
    bool hasSampled() const { return m_sampled; } // rates are valid from the next sample on
    // Adds usage for the configured adapters to `out` (keyed by pid).
    // `skipped` (optional) receives the processes that could not be opened for inspection.
    void sample(const QList<ProcessInfo>& processes, QHash<quint32, QList<ProcessAdapterUsage>>& out,
                int* inspectable, int* notInspectable, QList<ProcessInfo>* skipped = nullptr);

private:
    struct AdapterState {
        QString name;
        QStringList engineNames;
        std::vector<bool> segmentAperture;
        bool ready = false;
    };
    struct Baseline { std::vector<quint64> running; qint64 time100ns = 0; };
    bool prepare(quint64 luid, AdapterState& state);

    QHash<quint64, AdapterState> m_adapters;
    // (pid, creation time, luid) -> previous running times
    QHash<QString, Baseline> m_baselines;
    bool m_sampled = false;
};

} // namespace gf::mon::win
