#include "hardware/AdapterModel.h"

#include <QHash>
#include <QRegularExpression>

#include <windows.h>

namespace gf::hw {

// ---- small helpers --------------------------------------------------------------------------

QString PciLocation::toString() const
{
    return QStringLiteral("bus %1, device %2, function %3").arg(bus).arg(device).arg(function);
}

std::optional<PciLocation> PciLocation::fromRaw(quint32 bus, quint32 device, quint32 function)
{
    if (bus > 255 || device > 31 || function > 7)
        return std::nullopt;
    return PciLocation{ bus, device, function };
}

QStringList KernelAdapterType::flags() const
{
    QStringList out;
    if (renderSupported) out << QStringLiteral("render");
    if (displaySupported) out << QStringLiteral("display");
    if (postDevice) out << QStringLiteral("boot (POST) device");
    if (hybridDiscrete) out << QStringLiteral("hybrid discrete");
    if (hybridIntegrated) out << QStringLiteral("hybrid integrated");
    if (indirectDisplay) out << QStringLiteral("indirect display");
    if (softwareDevice) out << QStringLiteral("software");
    return out;
}

PciIds parsePciInstanceId(const QString& instanceId)
{
    static const QRegularExpression re(QStringLiteral(R"(^PCI\\VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})(?:&SUBSYS_([0-9A-F]{8}))?(?:&REV_([0-9A-F]{2}))?)"),
                                       QRegularExpression::CaseInsensitiveOption);
    PciIds ids;
    const auto m = re.match(instanceId);
    if (!m.hasMatch())
        return ids;
    ids.vendor = m.captured(1).toUInt(nullptr, 16);
    ids.device = m.captured(2).toUInt(nullptr, 16);
    ids.subsystem = m.captured(3).toUInt(nullptr, 16);
    ids.revision = m.captured(4).toUInt(nullptr, 16);
    ids.valid = true;
    return ids;
}

QString vendorName(quint32 vendorId)
{
    switch (vendorId) {
    case 0x10DE: return QStringLiteral("NVIDIA");
    case 0x8086: return QStringLiteral("Intel");
    case 0x1002: case 0x1022: return QStringLiteral("AMD");
    case 0x1414: return QStringLiteral("Microsoft");
    case 0x5143: return QStringLiteral("Qualcomm");
    case 0x15AD: return QStringLiteral("VMware");
    case 0x80EE: return QStringLiteral("VirtualBox");
    case 0x1AB8: return QStringLiteral("Parallels");
    case 0x1234: return QStringLiteral("QEMU");
    default:     return QStringLiteral("Unknown vendor");
    }
}

QString formatLuid(quint64 luid)
{
    return QStringLiteral("%1_%2").arg(quint32(luid >> 32), 8, 16, QLatin1Char('0')).arg(quint32(luid), 8, 16, QLatin1Char('0')).toUpper();
}

QString formatBytes(quint64 bytes)
{
    if (bytes >= (quint64(1) << 30))
        return QStringLiteral("%1 GB").arg(double(bytes) / double(quint64(1) << 30), 0, 'f', 2);
    if (bytes >= (quint64(1) << 20))
        return QStringLiteral("%1 MB").arg(bytes >> 20);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(bytes >> 10);
    return QStringLiteral("%1 bytes").arg(bytes);
}

QString formatWddm(int v)
{
    if (v <= 0)
        return QStringLiteral("unknown");
    return QStringLiteral("%1.%2").arg(v / 1000).arg((v % 1000) / 100);
}

// ---- classification --------------------------------------------------------------------------

bool isSoftwareAdapter(const DxgiAdapter& a)
{
    // The Microsoft Basic Render Driver (WARP) is 1414:008C; it may not always carry the flag.
    return a.softwareFlag || (a.vendorId == 0x1414 && a.deviceId == 0x008C);
}

Classification classify(const DxgiAdapter& a)
{
    if (isSoftwareAdapter(a))
        return { AdapterClass::Software, QStringLiteral("DXGI reports a software adapter (runs on the CPU)") };
    if (a.d3d12.uma.has_value() && *a.d3d12.uma)
        return { AdapterClass::Integrated, QStringLiteral("Direct3D 12 reports a unified memory architecture") };
    constexpr quint64 kSmallCarveOut = 512ull << 20;
    if (a.vendorId == 0x8086 && a.dedicatedVideoMemory <= kSmallCarveOut)
        return { AdapterClass::Integrated, QStringLiteral("Intel adapter whose dedicated memory is only a small reserved portion of system RAM") };
    if (a.d3d12.uma.has_value() && !*a.d3d12.uma && a.dedicatedVideoMemory > kSmallCarveOut)
        return { AdapterClass::Discrete, QStringLiteral("Own video memory and Direct3D 12 reports a non-unified memory architecture") };
    if (a.dedicatedVideoMemory > kSmallCarveOut)
        return { AdapterClass::Discrete, QStringLiteral("More than 512 MB of dedicated video memory") };
    return { AdapterClass::Unknown, QStringLiteral("Not enough information to tell whether this adapter is integrated or discrete") };
}

// ---- Adapter / DetectionResult ---------------------------------------------------------------

QString Adapter::name() const
{
    if (dxgi && !dxgi->description.isEmpty())
        return dxgi->description;
    if (pnp && !pnp->description.isEmpty())
        return pnp->description;
    return QStringLiteral("Unnamed display device");
}

PciIds Adapter::pciIds() const
{
    if (pnp) {
        if (const PciIds ids = parsePciInstanceId(pnp->instanceId); ids.valid)
            return ids;
    }
    PciIds ids;
    if (dxgi && !isSoftwareAdapter(*dxgi)) {
        ids = { dxgi->vendorId, dxgi->deviceId, dxgi->subSysId, dxgi->revision, true };
    }
    return ids;
}

QString Adapter::stableKey() const
{
    if (pnp)
        return pnp->instanceId;
    if (dxgi)
        return isSoftwareAdapter(*dxgi) ? QStringLiteral("software:%1").arg(dxgi->description) : QStringLiteral("luid:%1").arg(formatLuid(dxgi->luid));
    return {};
}

int DetectionResult::hardwareCount() const
{
    int n = 0;
    for (const Adapter& a : adapters)
        n += (a.dxgi && !a.isSoftware()) ? 1 : 0;
    return n;
}

int DetectionResult::softwareCount() const
{
    int n = 0;
    for (const Adapter& a : adapters)
        n += a.isSoftware() ? 1 : 0;
    return n;
}

// ---- merging ---------------------------------------------------------------------------------

DetectionResult mergeAdapters(const QList<DxgiAdapter>& dxgiIn, const QList<PnpDevice>& pnp)
{
    DetectionResult result;

    // DXGI may list the same adapter more than once (e.g. after a mode change); the LUID identifies it.
    QList<DxgiAdapter> hardware, software;
    QList<quint64> seen;
    for (const DxgiAdapter& a : dxgiIn) {
        if (seen.contains(a.luid))
            continue;
        seen.append(a.luid);
        (isSoftwareAdapter(a) ? software : hardware).append(a);
    }

    QList<bool> used(pnp.size(), false);
    auto findUnique = [&](auto&& predicate) {
        int found = -1;
        for (int i = 0; i < pnp.size(); ++i) {
            if (used[i] || !pnp[i].present || !predicate(pnp[i]))
                continue;
            if (found >= 0)
                return -2; // ambiguous
            found = i;
        }
        return found;
    };

    for (const DxgiAdapter& a : hardware) {
        Adapter adapter;
        adapter.dxgi = a;
        const PciIds ids{ a.vendorId, a.deviceId, a.subSysId, a.revision, true };
        int match = -1;
        if (a.location)
            match = findUnique([&](const PnpDevice& d) { return d.location && *d.location == *a.location; });
        if (match >= 0) {
            adapter.matchMethod = QStringLiteral("PCI location");
        } else {
            match = findUnique([&](const PnpDevice& d) { return parsePciInstanceId(d.instanceId).sameHardware(ids); });
            if (match >= 0) {
                adapter.matchMethod = QStringLiteral("PCI vendor, device, subsystem and revision IDs");
            } else {
                match = findUnique([&](const PnpDevice& d) {
                    const PciIds p = parsePciInstanceId(d.instanceId);
                    return p.valid && p.vendor == a.vendorId && p.device == a.deviceId;
                });
                if (match >= 0)
                    adapter.matchMethod = QStringLiteral("PCI vendor and device IDs");
            }
        }
        if (match >= 0) {
            used[match] = true;
            adapter.pnp = pnp[match];
        } else {
            adapter.matchMethod = QStringLiteral("no matching Device Manager entry");
        }
        adapter.classification = classify(a);
        result.adapters.append(adapter);
    }

    // Device Manager entries that DirectX does not expose.
    QList<PciIds> presentIds;
    for (const PnpDevice& d : pnp) {
        if (d.present)
            presentIds.append(parsePciInstanceId(d.instanceId));
    }
    QList<PciIds> addedAbsentIds;
    for (int i = 0; i < pnp.size(); ++i) {
        if (used[i])
            continue;
        const PnpDevice& d = pnp[i];
        if (!d.present) {
            // A stale record of hardware that is present under another instance (driver reinstall,
            // slot change) is a duplicate, not a second GPU.
            const PciIds ids = parsePciInstanceId(d.instanceId);
            const auto same = [&](const PciIds& o) { return ids.sameHardware(o); };
            if (std::any_of(presentIds.begin(), presentIds.end(), same) ||
                std::any_of(addedAbsentIds.begin(), addedAbsentIds.end(), same)) {
                ++result.staleDeviceEntries;
                continue;
            }
            if (ids.valid)
                addedAbsentIds.append(ids);
        }
        Adapter adapter;
        adapter.pnp = d;
        adapter.matchMethod = QStringLiteral("Device Manager only");
        adapter.classification = { AdapterClass::Unknown, QStringLiteral("Not available to DirectX, so its capabilities cannot be queried") };
        result.adapters.append(adapter);
    }

    for (const DxgiAdapter& a : software) {
        Adapter adapter;
        adapter.dxgi = a;
        adapter.matchMethod = QStringLiteral("software adapter (no device)");
        adapter.classification = classify(a);
        result.adapters.append(adapter);
    }
    return result;
}

// ---- findings --------------------------------------------------------------------------------

namespace {

Finding finding(Finding::Severity s, QString title, QString detail)
{
    return { s, std::move(title), std::move(detail) };
}

bool featureLevelBelow11(const QString& fl)
{
    return fl.startsWith(QLatin1StringView("10_")) || fl.startsWith(QLatin1StringView("9_"));
}

} // namespace

QList<Finding> findingsFor(const Adapter& a, const QDate& today)
{
    using S = Finding::Severity;
    QList<Finding> out;

    if (a.isSoftware()) {
        out << finding(S::Info, QStringLiteral("Software renderer"),
                       QStringLiteral("This adapter runs Direct3D on the CPU (WARP). It is not a physical GPU and is not counted as one."));
        return out;
    }

    if (a.pnp && a.pnp->hasProblem) {
        out << finding(S::Error, QStringLiteral("Device Manager reports a problem (code %1)").arg(a.pnp->problemCode),
                       describeProblemCode(a.pnp->problemCode));
    }

    if (!a.dxgi) {
        if (!a.pnp->present) {
            QString detail = QStringLiteral("Windows has a driver record for this device, but the device is not on the PCI bus now, "
                                            "so nothing can use it. Common causes: it was disabled in the BIOS/UEFI setup, or the "
                                            "firmware switches it off automatically when a discrete graphics card is installed.");
            if (parsePciInstanceId(a.pnp->instanceId).vendor == 0x8086)
                detail += QStringLiteral(" For Intel integrated graphics, look for a firmware option such as \"iGPU\", \"Internal Graphics\", "
                                         "\"IGD Multi-Monitor\" or \"Primary Display\".");
            detail += QStringLiteral(" GPU Fusion does not change firmware settings.");
            out << finding(S::Warning, QStringLiteral("Not currently detected"), detail);
        } else if (!a.pnp->hasProblem) {
            out << finding(S::Warning, QStringLiteral("Not available to DirectX"),
                           QStringLiteral("Device Manager lists this device as working, but DXGI does not enumerate it as a graphics adapter. "
                                          "It may be a display-only or virtual device, or its driver may not provide a WDDM render adapter. "
                                          "Reinstalling the vendor driver or restarting Windows can help if this is a real GPU."));
        } else {
            out << finding(S::Info, QStringLiteral("Not available to DirectX"),
                           QStringLiteral("Because of the problem above, applications cannot use this device."));
        }
        if (!a.pnp->instanceId.startsWith(QLatin1StringView("PCI\\"), Qt::CaseInsensitive))
            out << finding(S::Info, QStringLiteral("Not a PCI graphics device"),
                           QStringLiteral("This is a virtual or software display device (%1), not a GPU.").arg(a.pnp->instanceId.section(u'\\', 0, 0)));
        return out;
    }

    const DxgiAdapter& d = *a.dxgi;

    if (!a.pnp) {
        out << finding(S::Info, QStringLiteral("No matching Device Manager entry"),
                       QStringLiteral("Driver date, provider and device status are unavailable for this adapter."));
    }

    if (a.pnp && a.pnp->driverProvider.compare(QLatin1StringView("Microsoft"), Qt::CaseInsensitive) == 0 &&
        a.pnp->description.contains(QLatin1StringView("Basic Display"), Qt::CaseInsensitive)) {
        out << finding(S::Warning, QStringLiteral("No vendor driver installed"),
                       QStringLiteral("Windows is using its generic Basic Display driver. Hardware acceleration, video decoding and "
                                      "monitoring are limited until the GPU vendor's driver is installed. GPU Fusion does not install drivers."));
    }

    if (d.d3d11.device.state == ApiProbe::State::Failed) {
        out << finding(S::Error, QStringLiteral("Direct3D 11 device creation failed"), d.d3d11.device.error);
    } else if (d.d3d11.device.ok() && featureLevelBelow11(d.d3d11.device.featureLevel)) {
        out << finding(S::Info, QStringLiteral("Direct3D feature level %1").arg(d.d3d11.device.featureLevel),
                       QStringLiteral("Software that requires Direct3D 11-class hardware cannot use this adapter.%1")
                           .arg(d.d3d11.computeShaders ? QString() : QStringLiteral(" It has no compute-shader support.")));
    }

    if (d.d3d12.device.state == ApiProbe::State::Failed) {
        out << finding(S::Info, QStringLiteral("No Direct3D 12 support"),
                       QStringLiteral("%1 Direct3D 12 applications and explicit multi-adapter techniques cannot use this adapter.").arg(d.d3d12.device.error));
    }

    if (d.wddmVersion > 0 && d.wddmVersion < 2000) {
        out << finding(S::Warning, QStringLiteral("Legacy WDDM %1 driver").arg(formatWddm(d.wddmVersion)),
                       QStringLiteral("Windows GPU performance counters, and therefore Task Manager's GPU graphs, require WDDM 2.0 or "
                                      "newer, so they do not show this adapter. GPU Fusion reads its activity through kernel graphics "
                                      "statistics (D3DKMT) instead, which do cover WDDM 1.x drivers."));
    }

    if (a.pnp && a.pnp->driverDate.isValid() && today.isValid()) {
        const qint64 days = a.pnp->driverDate.daysTo(today);
        if (days > 5 * 365) {
            out << finding(S::Warning, QStringLiteral("Driver from %1").arg(a.pnp->driverDate.year()),
                           QStringLiteral("This driver is about %1 years old. For older GPUs it is often the last driver the vendor "
                                          "released; it may lack newer API, video and monitoring features.").arg(days / 365));
        }
    }

    if (d.outputs.isEmpty()) {
        out << finding(S::Info, QStringLiteral("No display connected"),
                       QStringLiteral("No monitor is attached to this adapter. Anything it renders for the screen has to be copied to a "
                                      "display driven by another adapter."));
    } else if (std::any_of(d.outputs.begin(), d.outputs.end(), [](const DisplayOutput& o) { return o.primary; })) {
        out << finding(S::Info, QStringLiteral("Drives the primary display"),
                       QStringLiteral("Programs that do not choose a GPU themselves normally get the adapter driving the primary display."));
    }
    if (d.kernelType) {
        const KernelAdapterType& k = *d.kernelType;
        const QString flags = k.flags().isEmpty() ? QStringLiteral("none") : k.flags().join(QStringLiteral(", "));
        if (!k.hybridDiscrete && !k.hybridIntegrated) {
            out << finding(S::Info, QStringLiteral("Not part of a Windows hybrid-GPU pair"),
                           QStringLiteral("Windows classifies this adapter as: %1. Without a hybrid integrated/discrete classification, "
                                          "Windows' per-app \"Power saving\" / \"High performance\" setting may not move programs between "
                                          "GPUs — GPU Fusion verifies what it actually does on the GPU Preferences page.").arg(flags));
        }
    }
    return out;
}

// ---- explanations -----------------------------------------------------------------------------

QString describeHresult(qint32 hr)
{
    const QString code = QStringLiteral("0x") + QString::number(quint32(hr), 16).rightJustified(8, u'0').toUpper();
    QString text;
    switch (static_cast<HRESULT>(hr)) {
    case S_OK:                                text = QStringLiteral("Success."); break;
    case DXGI_ERROR_UNSUPPORTED:              text = QStringLiteral("The driver does not support this API or feature level on this adapter."); break;
    case DXGI_ERROR_NOT_FOUND:                text = QStringLiteral("The adapter or object was not found."); break;
    case DXGI_ERROR_DEVICE_REMOVED:           text = QStringLiteral("The GPU was removed or its driver was reset or upgraded."); break;
    case DXGI_ERROR_DEVICE_HUNG:              text = QStringLiteral("The GPU stopped responding to commands."); break;
    case DXGI_ERROR_DEVICE_RESET:             text = QStringLiteral("The GPU was reset because of a badly formed command."); break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:    text = QStringLiteral("The graphics driver reported an internal error."); break;
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:  text = QStringLiteral("The resource is not available right now (for example, in use by another process)."); break;
    case DXGI_ERROR_SDK_COMPONENT_MISSING:    text = QStringLiteral("A required Windows graphics component is missing."); break;
    case DXGI_ERROR_INVALID_CALL:             text = QStringLiteral("The call was invalid for this adapter or driver."); break;
    case DXGI_ERROR_ACCESS_DENIED:            text = QStringLiteral("Access to the adapter was denied."); break;
    case E_INVALIDARG:                        text = QStringLiteral("An argument was rejected (often a feature level the runtime or driver does not know)."); break;
    case E_OUTOFMEMORY:                       text = QStringLiteral("Not enough memory to create the device."); break;
    case E_NOTIMPL:                           text = QStringLiteral("The driver does not implement this function."); break;
    case E_NOINTERFACE:                       text = QStringLiteral("The requested interface is not available on this system."); break;
    case E_FAIL:                              text = QStringLiteral("The operation failed without a specific reason (often a driver problem)."); break;
    default: {
        wchar_t* buffer = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        if (n && buffer)
            text = QString::fromWCharArray(buffer, int(n)).trimmed();
        LocalFree(buffer);
        if (text.isEmpty())
            text = QStringLiteral("Unrecognised error.");
    }
    }
    return QStringLiteral("%1 (HRESULT %2)").arg(text, code);
}

QString describeProblemCode(quint32 code)
{
    switch (code) {
    case 1:  return QStringLiteral("The device is not configured correctly. Reinstalling the driver usually fixes this.");
    case 3:  return QStringLiteral("The driver may be corrupted, or Windows is low on memory.");
    case 10: return QStringLiteral("The device cannot start. Updating or reinstalling the driver usually fixes this.");
    case 12: return QStringLiteral("The device cannot find enough free resources. Another device or a firmware setting may conflict with it.");
    case 14: return QStringLiteral("Windows must be restarted before the device works.");
    case 18: return QStringLiteral("The driver must be reinstalled.");
    case 19: return QStringLiteral("The device's configuration in the registry is incomplete or damaged.");
    case 21: return QStringLiteral("Windows is removing this device.");
    case 22: return QStringLiteral("The device is disabled in Device Manager. You can enable it there; GPU Fusion does not enable devices.");
    case 24: return QStringLiteral("The device is not present, not working properly, or has no driver installed.");
    case 28: return QStringLiteral("No driver is installed for this device.");
    case 29: return QStringLiteral("The firmware did not give the device the resources it needs; it may be disabled in the BIOS/UEFI setup.");
    case 31: return QStringLiteral("Windows cannot load the driver for this device.");
    case 32: return QStringLiteral("The driver's service is disabled.");
    case 37: return QStringLiteral("The driver failed to initialise.");
    case 39: return QStringLiteral("The driver is missing or corrupted.");
    case 43: return QStringLiteral("Windows stopped the device because it reported a problem (code 43). This often points to a driver or hardware fault.");
    case 45: return QStringLiteral("The device is not connected to the computer.");
    case 47: return QStringLiteral("The device was prepared for safe removal.");
    case 48: return QStringLiteral("The driver is blocked because of known compatibility problems.");
    case 52: return QStringLiteral("Windows cannot verify the driver's digital signature.");
    default: return QStringLiteral("Device Manager problem code %1.").arg(code);
    }
}

namespace {

struct DecoderInfo { const char* name; const char* codec; };

const QHash<QString, DecoderInfo>& decoderTable()
{
    // GUIDs from the Windows SDK (d3d11.h, D3D11_DECODER_PROFILE_*).
    static const QHash<QString, DecoderInfo> table{
        { QStringLiteral("{E6A9F44B-61B0-4563-9EA4-63D2A3C6FE66}"), { "MPEG-2 motion compensation", "MPEG-2" } },
        { QStringLiteral("{BF22AD00-03EA-4690-8077-473346209B7E}"), { "MPEG-2 IDCT", "MPEG-2" } },
        { QStringLiteral("{EE27417F-5E28-4E65-BEEA-1D26B508ADC9}"), { "MPEG-2 VLD", "MPEG-2" } },
        { QStringLiteral("{6F3EC719-3735-42CC-8063-65CC3CB36616}"), { "MPEG-1 VLD", "MPEG-1" } },
        { QStringLiteral("{86695F12-340E-4F04-9FD3-9253DD327460}"), { "MPEG-2 and MPEG-1 VLD", "MPEG-2" } },
        { QStringLiteral("{1B81BE64-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 motion compensation", "H.264" } },
        { QStringLiteral("{1B81BE65-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 motion compensation (film grain)", "H.264" } },
        { QStringLiteral("{1B81BE66-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 IDCT", "H.264" } },
        { QStringLiteral("{1B81BE67-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 IDCT (film grain)", "H.264" } },
        { QStringLiteral("{1B81BE68-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 VLD", "H.264" } },
        { QStringLiteral("{1B81BE69-A0C7-11D3-B984-00C04F2E73C5}"), { "H.264 VLD (film grain)", "H.264" } },
        { QStringLiteral("{D5F04FF9-3418-45D8-9561-32A76AAE2DDD}"), { "H.264 VLD with FMO/ASO", "H.264" } },
        { QStringLiteral("{D79BE8DA-0CF1-4C81-B82A-69A4E236F43D}"), { "H.264 stereo progressive VLD", "H.264" } },
        { QStringLiteral("{F9AACCBB-C2B6-4CFC-8779-5707B1760552}"), { "H.264 stereo VLD", "H.264" } },
        { QStringLiteral("{705B9D82-76CF-49D6-B7E6-AC8872DB013C}"), { "H.264 multiview VLD", "H.264" } },
        { QStringLiteral("{1B81BE80-A0C7-11D3-B984-00C04F2E73C5}"), { "WMV8 post-processing", "WMV" } },
        { QStringLiteral("{1B81BE81-A0C7-11D3-B984-00C04F2E73C5}"), { "WMV8 motion compensation", "WMV" } },
        { QStringLiteral("{1B81BE90-A0C7-11D3-B984-00C04F2E73C5}"), { "WMV9 post-processing", "WMV" } },
        { QStringLiteral("{1B81BE91-A0C7-11D3-B984-00C04F2E73C5}"), { "WMV9 motion compensation", "WMV" } },
        { QStringLiteral("{1B81BE94-A0C7-11D3-B984-00C04F2E73C5}"), { "WMV9 IDCT", "WMV" } },
        { QStringLiteral("{1B81BEA0-A0C7-11D3-B984-00C04F2E73C5}"), { "VC-1 post-processing", "VC-1" } },
        { QStringLiteral("{1B81BEA1-A0C7-11D3-B984-00C04F2E73C5}"), { "VC-1 motion compensation", "VC-1" } },
        { QStringLiteral("{1B81BEA2-A0C7-11D3-B984-00C04F2E73C5}"), { "VC-1 IDCT", "VC-1" } },
        { QStringLiteral("{1B81BEA3-A0C7-11D3-B984-00C04F2E73C5}"), { "VC-1 VLD", "VC-1" } },
        { QStringLiteral("{1B81BEA4-A0C7-11D3-B984-00C04F2E73C5}"), { "VC-1 VLD (2010)", "VC-1" } },
        { QStringLiteral("{EFD64D74-C9E8-41D7-A5E9-E9B0E39FA319}"), { "MPEG-4 Part 2 Simple VLD", "MPEG-4 Part 2" } },
        { QStringLiteral("{ED418A9F-010D-4EDA-9AE3-9A65358D8D2E}"), { "MPEG-4 Part 2 Advanced Simple VLD", "MPEG-4 Part 2" } },
        { QStringLiteral("{AB998B5B-4258-44A9-9FEB-94E597A6BAAE}"), { "MPEG-4 Part 2 Advanced Simple VLD (GMC)", "MPEG-4 Part 2" } },
        { QStringLiteral("{5B11D51B-2F4C-4452-BCC3-09F2A1160CC0}"), { "HEVC Main", "HEVC" } },
        { QStringLiteral("{107AF0E0-EF1A-4D19-ABA8-67A163073D13}"), { "HEVC Main 10", "HEVC" } },
        { QStringLiteral("{1A72925F-0C2C-4F15-96FB-B17D1473603F}"), { "HEVC Main 12", "HEVC" } },
        { QStringLiteral("{463707F8-A1D0-4585-876D-83AA6D60B89E}"), { "VP9 Profile 0", "VP9" } },
        { QStringLiteral("{A4C749EF-6ECF-48AA-8448-50A7A1165FF7}"), { "VP9 10-bit Profile 2", "VP9" } },
        { QStringLiteral("{90B899EA-3A62-4705-88B3-8DF04B2744E7}"), { "VP8", "VP8" } },
        { QStringLiteral("{B8BE4CCB-CF53-46BA-8D59-D6B8A6DA5D2A}"), { "AV1 Profile 0", "AV1" } },
        { QStringLiteral("{6936FF0F-45B1-4163-9CC1-646EF6946108}"), { "AV1 Profile 1", "AV1" } },
        { QStringLiteral("{0C5F2AA1-E541-4089-BB7B-98110A19D7C8}"), { "AV1 Profile 2", "AV1" } },
        { QStringLiteral("{725CB506-0C29-43C4-9440-8E9397903A04}"), { "Motion JPEG 4:2:0", "JPEG" } },
        { QStringLiteral("{CF782C83-BEF5-4A2C-87CB-6019E7B175AC}"), { "JPEG 4:2:0", "JPEG" } },
    };
    return table;
}

} // namespace

QString decoderProfileName(const QString& guid)
{
    const auto it = decoderTable().constFind(guid.toUpper());
    return it == decoderTable().constEnd() ? QString() : QString::fromLatin1(it->name);
}

QStringList decoderCodecs(const QStringList& guids)
{
    QStringList codecs;
    for (const QString& g : guids) {
        const auto it = decoderTable().constFind(g.toUpper());
        if (it != decoderTable().constEnd() && !codecs.contains(QLatin1StringView(it->codec)))
            codecs.append(QString::fromLatin1(it->codec));
    }
    return codecs;
}

} // namespace gf::hw
