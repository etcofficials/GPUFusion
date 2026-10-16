#pragma once

#include "monitoring/MetricModel.h"

#include <QHash>
#include <QSet>

namespace gf::mon::win {

// NVIDIA sensors through NVML (nvml.dll, shipped with the NVIDIA driver). Loaded at run time:
// if the DLL or a function is missing, or initialisation fails, the provider stays inactive and
// GPU Fusion runs without it. Metrics NVML reports as NOT_SUPPORTED are marked Unsupported once
// and never queried again.
class NvmlProvider {
public:
    NvmlProvider();
    ~NvmlProvider();
    NvmlProvider(const NvmlProvider&) = delete;
    NvmlProvider& operator=(const NvmlProvider&) = delete;

    bool isActive() const { return m_active; }
    void setTargets(const QList<MonitorTarget>& targets);
    void sample(QList<AdapterSample>& samples);
    ProviderStatus status() const;

private:
    struct Api;
    enum Metric { Temperature, Fan, Power, CoreClock, MemoryClock, PState, MetricCount };

    MetricValue read(void* device, Metric metric);

    Api* m_api = nullptr;
    void* m_module = nullptr;
    bool m_active = false;
    QString m_detail;
    QHash<quint64, void*> m_devices;          // LUID -> nvmlDevice_t
    QHash<quint64, QSet<int>> m_unsupported;  // LUID -> metrics reported NOT_SUPPORTED
    QStringList m_unsupportedNames;
};

} // namespace gf::mon::win
