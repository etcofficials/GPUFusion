#pragma once

#include "core/RingBuffer.h"
#include "monitoring/MetricModel.h"

#include <QHash>

namespace gf::mon {

struct HistoryPoint {
    qint64 tMs = 0;   // steady clock
    float value = 0;  // NaN when the reading was not Ok (drawn as a gap, never as zero)
};

enum class Series { Utilization, DedicatedMemory, SharedMemory, Temperature };

// Bounded per-adapter time series. Capacity = points per series; adapters that disappear from
// the snapshots (removed, or re-created with a new LUID after a driver reset) are dropped.
class History {
public:
    void setCapacity(int points);
    int capacity() const { return m_capacity; }
    void append(const Snapshot& snapshot);
    void clear() { m_adapters.clear(); }

    // nullptr if the adapter has no history.
    const RingBuffer<HistoryPoint>* series(quint64 luid, Series s) const;

private:
    struct AdapterHistory {
        RingBuffer<HistoryPoint> utilization, dedicated, shared, temperature;
        const RingBuffer<HistoryPoint>& get(Series s) const;
        RingBuffer<HistoryPoint>& get(Series s) { return const_cast<RingBuffer<HistoryPoint>&>(std::as_const(*this).get(s)); }
    };
    QHash<quint64, AdapterHistory> m_adapters;
    int m_capacity = 301;
};

} // namespace gf::mon
