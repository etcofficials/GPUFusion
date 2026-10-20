#pragma once

// Plain data describing graphics adapters, plus the platform-independent logic that turns raw
// DXGI / Device Manager observations into a merged, classified, explained adapter list.
// Nothing here calls Windows graphics APIs, so all of it is unit-tested with synthetic data.

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace gf::hw {

struct PciLocation {
    quint32 bus = 0;
    quint32 device = 0;
    quint32 function = 0;
    bool operator==(const PciLocation&) const = default;
    QString toString() const; // "bus 1, device 0, function 0"

    // PCI limits: bus 0-255, device 0-31, function 0-7. Non-PCI adapters report all-ones sentinels.
    static std::optional<PciLocation> fromRaw(quint32 bus, quint32 device, quint32 function);
};

struct PciIds {
    quint32 vendor = 0, device = 0, subsystem = 0, revision = 0;
    bool valid = false;
    bool sameHardware(const PciIds& o) const { return valid && o.valid && vendor == o.vendor && device == o.device && subsystem == o.subsystem && revision == o.revision; }
};

// "PCI\VEN_10DE&DEV_0F02&SUBSYS_098F10DE&REV_A1\4&465B5C5&0&0008" -> ids
PciIds parsePciInstanceId(const QString& instanceId);

// Outcome of trying to create a device for an API on an adapter.
struct ApiProbe {
    enum class State { NotAttempted, Succeeded, Failed };
    State state = State::NotAttempted;
    QString featureLevel; // e.g. "11_0" on success
    qint32 hresult = 0;
    QString error;        // plain-language explanation on failure

    bool ok() const { return state == State::Succeeded; }
};

struct D3D11Caps {
    ApiProbe device;
    bool computeShaders = false;   // CS 5.0 (FL 11_0+) or CS 4.x with raw/structured buffers
    bool doublePrecision = false;
    bool driverCommandLists = false;
    bool videoDevice = false;      // ID3D11VideoDevice available
    QStringList decoderProfiles;   // GUID strings "{XXXXXXXX-...}" upper case
};

struct D3D12Caps {
    ApiProbe device;
    QString maxFeatureLevel;
    std::optional<bool> uma;       // unified memory architecture (integrated-style)
};

struct DisplayOutput {
    QString deviceName;            // "\\.\DISPLAY1"
    QString monitor;               // monitor description, if available
    int width = 0, height = 0;
    bool attachedToDesktop = false;
    bool primary = false;          // the primary display (where the taskbar is by default)
};

// How the Windows graphics kernel classifies an adapter (D3DKMT KMTQAITYPE_ADAPTERTYPE). Windows'
// per-app GPU preference ("power saving" / "high performance") relies on this classification, which
// the driver reports — older drivers report fewer flags.
struct KernelAdapterType {
    bool renderSupported = false;
    bool displaySupported = false;
    bool softwareDevice = false;
    bool postDevice = false;       // the adapter the firmware (BIOS/UEFI) used at boot
    bool hybridDiscrete = false;   // discrete GPU of a Windows-recognised hybrid system
    bool hybridIntegrated = false; // integrated GPU of a Windows-recognised hybrid system
    bool indirectDisplay = false;
    QStringList flags() const;     // names of the set flags, for display
};

// What DXGI and Direct3D report for one adapter.
struct DxgiAdapter {
    int index = -1;                // EnumAdapters1 index
    QString description;
    quint32 vendorId = 0, deviceId = 0, subSysId = 0, revision = 0;
    quint64 luid = 0;
    bool softwareFlag = false;     // DXGI_ADAPTER_FLAG_SOFTWARE
    quint64 dedicatedVideoMemory = 0, dedicatedSystemMemory = 0, sharedSystemMemory = 0;
    QString umdVersion;            // user-mode driver version (CheckInterfaceSupport)
    int wddmVersion = 0;           // KMT_DRIVERVERSION value, e.g. 1200, 2300; 0 = unknown
    std::optional<PciLocation> location;
    std::optional<KernelAdapterType> kernelType; // nullopt: the query was not answered
    D3D11Caps d3d11;
    D3D12Caps d3d12;
    QList<DisplayOutput> outputs;
    bool capsReused = false;       // probes copied from a previous detection (no device recreated)
};

// What Device Manager (SetupAPI / Configuration Manager) reports for one display-class device.
struct PnpDevice {
    QString instanceId;
    QString description;
    bool present = false;          // currently on the bus (false = installed earlier, not detected now)
    bool hasProblem = false;
    quint32 problemCode = 0;
    bool started = false;
    QString driverVersion;
    QDate driverDate;
    QString driverProvider;
    QString infName;
    QString locationInfo;
    std::optional<PciLocation> location;
};

enum class AdapterClass { Discrete, Integrated, Software, Unknown };

struct Classification {
    AdapterClass value = AdapterClass::Unknown;
    QString reason;
};

struct Finding {
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Info;
    QString title;
    QString detail;
};

// One logical adapter after merging DXGI and Device Manager information.
struct Adapter {
    std::optional<DxgiAdapter> dxgi;
    std::optional<PnpDevice> pnp;
    QString matchMethod;           // how dxgi and pnp were paired ("PCI location", "PCI IDs", ...)
    Classification classification;
    QList<Finding> findings;

    QString name() const;
    bool isSoftware() const { return classification.value == AdapterClass::Software; }
    bool availableToDirectX() const { return dxgi.has_value(); }
    PciIds pciIds() const;
    QString stableKey() const;     // for matching across refreshes
};

struct DetectionResult {
    QList<Adapter> adapters;       // hardware DXGI adapters (by index), Device-Manager-only, software
    QStringList errors;            // problems that prevented part of detection
    QDateTime finishedAt;
    qint64 durationMs = 0;
    int staleDeviceEntries = 0;    // non-present Device Manager duplicates that were hidden

    int hardwareCount() const;     // physical adapters usable through DirectX
    int softwareCount() const;
};

// ---- logic ----------------------------------------------------------------------------------

QString vendorName(quint32 vendorId);
QString formatLuid(quint64 luid);
QString formatBytes(quint64 bytes);
QString formatWddm(int kmtDriverVersion); // 1200 -> "1.2"; 0 -> "unknown"

bool isSoftwareAdapter(const DxgiAdapter& a);
Classification classify(const DxgiAdapter& a);

// Pairs DXGI adapters with Device Manager entries, removes duplicates and orders the result.
DetectionResult mergeAdapters(const QList<DxgiAdapter>& dxgi, const QList<PnpDevice>& pnp);

// Explanations and warnings for an adapter (driver age is judged against `today`).
QList<Finding> findingsFor(const Adapter& adapter, const QDate& today);

QString describeHresult(qint32 hr);
QString describeProblemCode(quint32 code);
QString decoderProfileName(const QString& guid); // empty if unknown
QStringList decoderCodecs(const QStringList& guids); // de-duplicated codec families, e.g. "H.264"

} // namespace gf::hw
