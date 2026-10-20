#include "platform/windows/DxgiProbe.h"

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace gf::hw::win {

namespace {

QString featureLevelName(D3D_FEATURE_LEVEL fl)
{
    return QStringLiteral("%1_%2").arg((fl >> 12) & 0xF).arg((fl >> 8) & 0xF);
}

QString guidString(const GUID& g)
{
    wchar_t buffer[64] = {};
    const int n = StringFromGUID2(g, buffer, int(std::size(buffer)));
    return n > 1 ? QString::fromWCharArray(buffer, n - 1).toUpper() : QString();
}

ApiProbe failed(HRESULT hr)
{
    ApiProbe p;
    p.state = ApiProbe::State::Failed;
    p.hresult = hr;
    p.error = describeHresult(hr);
    return p;
}

void queryKernelInfo(DxgiAdapter& a, LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0)
        return;

    D3DKMT_DRIVERVERSION version{};
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_DRIVERVERSION;
    q.pPrivateDriverData = &version;
    q.PrivateDriverDataSize = sizeof(version);
    if (D3DKMTQueryAdapterInfo(&q) == 0)
        a.wddmVersion = int(version);

    D3DKMT_ADAPTERADDRESS address{};
    q.Type = KMTQAITYPE_ADAPTERADDRESS;
    q.pPrivateDriverData = &address;
    q.PrivateDriverDataSize = sizeof(address);
    if (D3DKMTQueryAdapterInfo(&q) == 0)
        a.location = PciLocation::fromRaw(address.BusNumber, address.DeviceNumber, address.FunctionNumber);

    D3DKMT_ADAPTERTYPE type{};
    q.Type = KMTQAITYPE_ADAPTERTYPE;
    q.pPrivateDriverData = &type;
    q.PrivateDriverDataSize = sizeof(type);
    if (D3DKMTQueryAdapterInfo(&q) == 0) {
        KernelAdapterType k;
        k.renderSupported = type.RenderSupported;
        k.displaySupported = type.DisplaySupported;
        k.softwareDevice = type.SoftwareDevice;
        k.postDevice = type.PostDevice;
        k.hybridDiscrete = type.HybridDiscrete;
        k.hybridIntegrated = type.HybridIntegrated;
        k.indirectDisplay = type.IndirectDisplayDevice;
        a.kernelType = k;
    }

    D3DKMT_CLOSEADAPTER close{ open.hAdapter };
    D3DKMTCloseAdapter(&close);
}

QList<DisplayOutput> enumerateOutputs(IDXGIAdapter1* adapter)
{
    QList<DisplayOutput> outputs;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i, output.Reset()) {
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc)))
            continue;
        DisplayOutput o;
        o.deviceName = QString::fromWCharArray(desc.DeviceName);
        o.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        o.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        o.attachedToDesktop = desc.AttachedToDesktop;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (desc.Monitor && GetMonitorInfoW(desc.Monitor, &mi))
            o.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(desc.DeviceName, 0, &monitor, 0))
            o.monitor = QString::fromWCharArray(monitor.DeviceString);
        outputs.append(o);
    }
    return outputs;
}

D3D11Caps probeD3D11(IDXGIAdapter1* adapter)
{
    D3D11Caps caps;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, UINT(std::size(levels)),
                                   D3D11_SDK_VERSION, &device, &got, nullptr);
    if (hr == E_INVALIDARG) // runtimes without 11_1 reject the whole list
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, UINT(std::size(levels) - 1),
                               D3D11_SDK_VERSION, &device, &got, nullptr);
    if (FAILED(hr)) {
        caps.device = failed(hr);
        return caps;
    }
    caps.device.state = ApiProbe::State::Succeeded;
    caps.device.featureLevel = featureLevelName(got);

    if (got >= D3D_FEATURE_LEVEL_11_0) {
        caps.computeShaders = true;
    } else {
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hw{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hw, sizeof(hw))))
            caps.computeShaders = hw.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
    }
    D3D11_FEATURE_DATA_DOUBLES doubles{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles))))
        caps.doublePrecision = doubles.DoublePrecisionFloatShaderOps;
    D3D11_FEATURE_DATA_THREADING threading{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading))))
        caps.driverCommandLists = threading.DriverCommandLists;

    ComPtr<ID3D11VideoDevice> video;
    if (SUCCEEDED(device.As(&video))) {
        caps.videoDevice = true;
        const UINT count = video->GetVideoDecoderProfileCount();
        for (UINT i = 0; i < count; ++i) {
            GUID profile{};
            if (SUCCEEDED(video->GetVideoDecoderProfile(i, &profile)))
                caps.decoderProfiles.append(guidString(profile));
        }
    }
    return caps;
}

D3D12Caps probeD3D12(IDXGIAdapter1* adapter)
{
    D3D12Caps caps;
    // Loaded dynamically so a system without the Direct3D 12 runtime still runs GPU Fusion.
    static const PFN_D3D12_CREATE_DEVICE create = [] {
        HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice")) : nullptr;
    }();
    if (!create) {
        caps.device.state = ApiProbe::State::Failed;
        caps.device.error = QStringLiteral("The Direct3D 12 runtime is not installed on this system.");
        return caps;
    }

    ComPtr<ID3D12Device> device;
    const HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        caps.device = failed(hr);
        return caps;
    }
    caps.device.state = ApiProbe::State::Succeeded;

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ UINT(std::size(levels)), levels, D3D_FEATURE_LEVEL_11_0 };
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        caps.maxFeatureLevel = featureLevelName(fl.MaxSupportedFeatureLevel);
    else
        caps.maxFeatureLevel = QStringLiteral("11_0");
    caps.device.featureLevel = caps.maxFeatureLevel;

    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))))
        caps.uma = bool(arch.UMA);
    return caps;
}

} // namespace

DxgiEnumeration enumerateDxgi(const QList<DxgiAdapter>* previous)
{
    DxgiEnumeration result;
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        result.errors << QStringLiteral("Could not start DXGI adapter enumeration: %1").arg(describeHresult(hr));
        return result;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        if (const HRESULT hr = adapter->GetDesc1(&desc); FAILED(hr)) {
            result.errors << QStringLiteral("Adapter %1: description unavailable: %2").arg(i).arg(describeHresult(hr));
            continue;
        }
        DxgiAdapter a;
        a.index = int(i);
        a.description = QString::fromWCharArray(desc.Description);
        a.vendorId = desc.VendorId;
        a.deviceId = desc.DeviceId;
        a.subSysId = desc.SubSysId;
        a.revision = desc.Revision;
        a.luid = (quint64(quint32(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
        a.softwareFlag = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        a.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        a.dedicatedSystemMemory = desc.DedicatedSystemMemory;
        a.sharedSystemMemory = desc.SharedSystemMemory;

        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            a.umdVersion = QStringLiteral("%1.%2.%3.%4").arg(HIWORD(umd.HighPart)).arg(LOWORD(umd.HighPart))
                                                       .arg(HIWORD(umd.LowPart)).arg(LOWORD(umd.LowPart));
        }
        queryKernelInfo(a, desc.AdapterLuid);
        a.outputs = enumerateOutputs(adapter.Get());

        if (isSoftwareAdapter(a)) {
            result.adapters.append(a); // no device probes: not a physical GPU
            continue;
        }

        const DxgiAdapter* cached = nullptr;
        if (previous) {
            for (const DxgiAdapter& p : *previous) {
                if (p.luid == a.luid && p.umdVersion == a.umdVersion && p.d3d11.device.state != ApiProbe::State::NotAttempted) {
                    cached = &p;
                    break;
                }
            }
        }
        if (cached) {
            a.d3d11 = cached->d3d11;
            a.d3d12 = cached->d3d12;
            a.capsReused = true;
        } else {
            a.d3d11 = probeD3D11(adapter.Get());
            a.d3d12 = probeD3D12(adapter.Get());
        }
        result.adapters.append(a);
    }
    return result;
}

} // namespace gf::hw::win
