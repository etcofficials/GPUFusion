#include "monitoring/History.h"

#include <QSet>

namespace gf::mon {

const RingBuffer<HistoryPoint>& History::AdapterHistory::get(Series s) const
{
    switch (s) {
    case Series::Utilization:     return utilization;
    case Series::DedicatedMemory: return dedicated;
    case Series::SharedMemory:    return shared;
    case Series::Temperature:     return temperature;
    }
    return utilization;
}

void History::setCapacity(int points)
{
    m_capacity = std::max(2, points);
    for (AdapterHistory& h : m_adapters) {
        for (Series s : { Series::Utilization, Series::DedicatedMemory, Series::SharedMemory, Series::Temperature })
            h.get(s).setCapacity(m_capacity);
    }
}

void History::append(const Snapshot& snapshot)
{
    QSet<quint64> present;
    for (const AdapterSample& a : snapshot.adapters) {
        present.insert(a.luid);
        auto it = m_adapters.find(a.luid);
        if (it == m_adapters.end()) {
            it = m_adapters.insert(a.luid, AdapterHistory{});
            for (Series s : { Series::Utilization, Series::DedicatedMemory, Series::SharedMemory, Series::Temperature })
                it->get(s).setCapacity(m_capacity);
        }
        const auto point = [&](const MetricValue& v) { return HistoryPoint{ snapshot.steadyMs, float(v.valueOrNaN()) }; };
        it->utilization.push(point(a.utilization));
        it->dedicated.push(point(a.dedicatedUsed));
        it->shared.push(point(a.sharedUsed));
        it->temperature.push(point(a.temperature));
    }
    for (auto it = m_adapters.begin(); it != m_adapters.end();) {
        if (!present.contains(it.key()))
            it = m_adapters.erase(it);
        else
            ++it;
    }
}

const RingBuffer<HistoryPoint>* History::series(quint64 luid, Series s) const
{
    const auto it = m_adapters.constFind(luid);
    if (it == m_adapters.constEnd())
        return nullptr;
    return &it->get(s);
}

} // namespace gf::mon
