#pragma once

#include "monitoring/MetricModel.h"

#include <QHash>

#include <vector>

namespace gf::mon::win {

// Engine utilization and memory residency from the kernel graphics statistics interface
// (D3DKMTQueryStatistics, gdi32). Unlike Windows' PDH GPU counters it also works for WDDM 1.x
// drivers. No handles are kept open between samples; engine names are looked up once per adapter.
class KmtStatsProvider {
public:
    void setTargets(const QList<MonitorTarget>& targets);
    // Fills utilization, engines, dedicatedUsed and sharedUsed. Returns LUIDs whose queries failed
    // (adapter removed or reset), so hardware detection can be re-run.
    QList<quint64> sample(QList<AdapterSample>& samples);
    ProviderStatus status() const;

private:
    struct State {
        bool initialised = false;
        QStringList engineNames;
        int segmentCount = 0;
        std::vector<std::optional<quint64>> previousRunning;
        qint64 previousQpc100ns = 0;
    };
    bool initialise(quint64 luid, State& state);

    QHash<quint64, State> m_states;
    QString m_lastError;
};

} // namespace gf::mon::win
