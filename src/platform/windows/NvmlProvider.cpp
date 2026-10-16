#include "platform/windows/NvmlProvider.h"

#include "core/Logging.h"

#include <QDir>

#include <windows.h>

namespace gf::mon::win {

namespace {

// Minimal subset of the public NVML C API (nvml.h), declared here so no SDK is needed.
using nvmlReturn_t = int;
using nvmlDevice_t = void*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr nvmlReturn_t NVML_ERROR_NOT_SUPPORTED = 3;
constexpr unsigned NVML_TEMPERATURE_GPU = 0;
constexpr unsigned NVML_CLOCK_GRAPHICS = 0;
constexpr unsigned NVML_CLOCK_MEM = 2;

// nvmlPciInfo_t: the fields used here have the same offsets in every version of the struct
// (16-byte bus-id string, then domain, bus, device, pciDeviceId, pciSubSystemId); the tail is padding.
struct NvmlPciInfo {
    char busIdLegacy[16];
    unsigned domain, bus, device, pciDeviceId, pciSubSystemId;
    char tail[128];
};

} // namespace

struct NvmlProvider::Api {
    nvmlReturn_t (*init)() = nullptr;
    nvmlReturn_t (*shutdown)() = nullptr;
    const char* (*errorString)(nvmlReturn_t) = nullptr;
    nvmlReturn_t (*getCount)(unsigned*) = nullptr;
    nvmlReturn_t (*getHandleByIndex)(unsigned, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*getPciInfo)(nvmlDevice_t, NvmlPciInfo*) = nullptr;
    nvmlReturn_t (*getTemperature)(nvmlDevice_t, unsigned, unsigned*) = nullptr;
    nvmlReturn_t (*getFanSpeed)(nvmlDevice_t, unsigned*) = nullptr;
    nvmlReturn_t (*getPowerUsage)(nvmlDevice_t, unsigned*) = nullptr;
    nvmlReturn_t (*getClockInfo)(nvmlDevice_t, unsigned, unsigned*) = nullptr;
    nvmlReturn_t (*getPerformanceState)(nvmlDevice_t, int*) = nullptr;
    nvmlReturn_t (*systemGetDriverVersion)(char*, unsigned) = nullptr;
    nvmlReturn_t (*systemGetNvmlVersion)(char*, unsigned) = nullptr;
};

namespace {

template <typename F>
bool resolve(HMODULE module, F& fn, std::initializer_list<const char*> names)
{
    for (const char* n : names) {
        if (FARPROC p = GetProcAddress(module, n)) {
            fn = reinterpret_cast<F>(p);
            return true;
        }
    }
    return false;
}

QString metricName(int m)
{
    static const char* const names[] = { "temperature", "fan speed", "power draw", "core clock", "memory clock", "performance state" };
    return QString::fromLatin1(names[m]);
}

} // namespace

NvmlProvider::NvmlProvider()
{
    // The driver installs nvml.dll in System32 (newer drivers) or in NVSMI (older drivers such as R391).
    QStringList candidates;
    wchar_t system[MAX_PATH] = {};
    if (GetSystemDirectoryW(system, MAX_PATH))
        candidates << QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("nvml.dll"));
    const QString programFiles = qEnvironmentVariable("ProgramW6432", qEnvironmentVariable("ProgramFiles"));
    if (!programFiles.isEmpty())
        candidates << QDir(programFiles).filePath(QStringLiteral("NVIDIA Corporation/NVSMI/nvml.dll"));

    HMODULE module = nullptr;
    QString loadedFrom;
    for (const QString& path : candidates) {
        module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), nullptr,
                                LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) {
            loadedFrom = QDir::toNativeSeparators(path);
            break;
        }
    }
    if (!module) {
        m_detail = QStringLiteral("nvml.dll not found (NVIDIA driver not installed, or a driver without NVML).");
        return;
    }
    m_module = module;

    auto* api = new Api;
    const bool required =
        resolve(module, api->init, { "nvmlInit_v2", "nvmlInit" }) &&
        resolve(module, api->shutdown, { "nvmlShutdown" }) &&
        resolve(module, api->getCount, { "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount" }) &&
        resolve(module, api->getHandleByIndex, { "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex" }) &&
        resolve(module, api->getPciInfo, { "nvmlDeviceGetPciInfo_v3", "nvmlDeviceGetPciInfo_v2", "nvmlDeviceGetPciInfo" });
    resolve(module, api->errorString, { "nvmlErrorString" });
    resolve(module, api->getTemperature, { "nvmlDeviceGetTemperature" });
    resolve(module, api->getFanSpeed, { "nvmlDeviceGetFanSpeed" });
    resolve(module, api->getPowerUsage, { "nvmlDeviceGetPowerUsage" });
    resolve(module, api->getClockInfo, { "nvmlDeviceGetClockInfo" });
    resolve(module, api->getPerformanceState, { "nvmlDeviceGetPerformanceState" });
    resolve(module, api->systemGetDriverVersion, { "nvmlSystemGetDriverVersion" });
    resolve(module, api->systemGetNvmlVersion, { "nvmlSystemGetNVMLVersion" });
    m_api = api;

    if (!required) {
        m_detail = QStringLiteral("%1 is missing required functions.").arg(loadedFrom);
        return;
    }
    if (const nvmlReturn_t rc = m_api->init(); rc != NVML_SUCCESS) {
        m_detail = QStringLiteral("nvmlInit failed: %1 (%2).")
                       .arg(m_api->errorString ? QString::fromLatin1(m_api->errorString(rc)) : QStringLiteral("error")).arg(rc);
        m_api->init = nullptr; // do not call shutdown
        return;
    }
    char driver[96] = {}, nvml[96] = {};
    if (m_api->systemGetDriverVersion)
        m_api->systemGetDriverVersion(driver, sizeof(driver));
    if (m_api->systemGetNvmlVersion)
        m_api->systemGetNvmlVersion(nvml, sizeof(nvml));
    m_active = true;
    m_detail = QStringLiteral("Loaded from %1; driver %2, NVML %3.").arg(loadedFrom, QString::fromLatin1(driver), QString::fromLatin1(nvml));
    qCInfo(lcMonitor).noquote() << "NVML:" << m_detail;
}

NvmlProvider::~NvmlProvider()
{
    if (m_api && m_active && m_api->init && m_api->shutdown)
        m_api->shutdown();
    delete m_api;
    // nvml.dll stays loaded: unloading driver DLLs at run time is not worth the risk.
}

void NvmlProvider::setTargets(const QList<MonitorTarget>& targets)
{
    m_devices.clear();
    m_unsupported.clear();
    if (!m_active)
        return;
    unsigned count = 0;
    if (m_api->getCount(&count) != NVML_SUCCESS)
        return;
    struct NvmlDev { nvmlDevice_t handle; NvmlPciInfo pci; };
    QList<NvmlDev> devices;
    for (unsigned i = 0; i < count; ++i) {
        NvmlDev d{};
        if (m_api->getHandleByIndex(i, &d.handle) == NVML_SUCCESS && m_api->getPciInfo(d.handle, &d.pci) == NVML_SUCCESS)
            devices << d;
    }
    for (const MonitorTarget& t : targets) {
        if (t.vendorId != 0x10DE)
            continue;
        // Pair by PCI bus/device (exact), falling back to the PCI device ID when it is unique.
        const NvmlDev* match = nullptr;
        if (t.location) {
            for (const NvmlDev& d : devices) {
                if (d.pci.bus == t.location->bus && d.pci.device == t.location->device)
                    match = &d;
            }
        }
        if (!match) {
            const unsigned wanted = (t.deviceId << 16) | t.vendorId;
            int hits = 0;
            for (const NvmlDev& d : devices) {
                if (d.pci.pciDeviceId == wanted) {
                    match = &d;
                    ++hits;
                }
            }
            if (hits != 1)
                match = nullptr;
        }
        if (match)
            m_devices.insert(t.luid, match->handle);
        else
            qCWarning(lcMonitor).noquote() << "NVML: no NVML device matches" << t.name;
    }
}

MetricValue NvmlProvider::read(void* device, Metric metric)
{
    unsigned u = 0;
    int i = 0;
    nvmlReturn_t rc = NVML_ERROR_NOT_SUPPORTED;
    double value = 0;
    switch (metric) {
    case Temperature:
        if (m_api->getTemperature) { rc = m_api->getTemperature(device, NVML_TEMPERATURE_GPU, &u); value = u; }
        break;
    case Fan:
        if (m_api->getFanSpeed) { rc = m_api->getFanSpeed(device, &u); value = u; }
        break;
    case Power:
        if (m_api->getPowerUsage) { rc = m_api->getPowerUsage(device, &u); value = u / 1000.0; } // mW -> W
        break;
    case CoreClock:
        if (m_api->getClockInfo) { rc = m_api->getClockInfo(device, NVML_CLOCK_GRAPHICS, &u); value = u; }
        break;
    case MemoryClock:
        if (m_api->getClockInfo) { rc = m_api->getClockInfo(device, NVML_CLOCK_MEM, &u); value = u; }
        break;
    case PState:
        if (m_api->getPerformanceState) { rc = m_api->getPerformanceState(device, &i); value = i; }
        if (rc == NVML_SUCCESS && i == 32) // NVML_PSTATE_UNKNOWN
            return MetricValue::unknown(QStringLiteral("NVML reports an unknown performance state"));
        break;
    case MetricCount:
        break;
    }
    if (rc == NVML_SUCCESS)
        return MetricValue::ok(value, QStringLiteral("NVML"));
    if (rc == NVML_ERROR_NOT_SUPPORTED)
        return MetricValue::unsupported(QStringLiteral("NVML reports this is not supported on this GPU/driver"));
    return MetricValue::unavailable(QStringLiteral("NVML error %1").arg(m_api->errorString ? QString::fromLatin1(m_api->errorString(rc)) : QString::number(rc)));
}

void NvmlProvider::sample(QList<AdapterSample>& samples)
{
    for (AdapterSample& out : samples) {
        const auto it = m_devices.constFind(out.luid);
        if (it == m_devices.constEnd())
            continue;
        QSet<int>& unsupported = m_unsupported[out.luid];
        MetricValue* fields[MetricCount] = { &out.temperature, &out.fanPercent, &out.powerWatts,
                                             &out.coreClockMHz, &out.memoryClockMHz, &out.pstate };
        for (int m = 0; m < MetricCount; ++m) {
            if (unsupported.contains(m)) {
                *fields[m] = MetricValue::unsupported(QStringLiteral("NVML reports this is not supported on this GPU/driver"));
                continue;
            }
            *fields[m] = read(*it, Metric(m));
            if (fields[m]->state == MetricValue::State::Unsupported) {
                unsupported.insert(m);
                if (!m_unsupportedNames.contains(metricName(m)))
                    m_unsupportedNames << metricName(m);
                qCInfo(lcMonitor).noquote() << "NVML:" << metricName(m) << "is not supported for" << out.name << "- not polled again";
            }
        }
        out.sources << QStringLiteral("NVML");
    }
}

ProviderStatus NvmlProvider::status() const
{
    return { QStringLiteral("NVIDIA NVML"), m_active && !m_devices.isEmpty(), m_detail, m_unsupportedNames };
}

} // namespace gf::mon::win
