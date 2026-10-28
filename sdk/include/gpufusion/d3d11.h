// GPU Fusion SDK — Direct3D 11 helpers: what each adapter offered by GPU Fusion can do, and one
// device per adapter for programs that divide their own work (with scheduler.h). Header-only, C++17,
// Windows; MIT License. Links d3d11.lib and dxgi.lib.
//
//     auto devices = gpufusion::createDevices(gpufusion::adapters(factory.Get()));
//     for (const auto& d : devices)
//         // d.caps.name, d.caps.featureLevel, d.caps.computeShaders ... use d.device / d.context
//
// Devices on different adapters share nothing: results that must be combined go through system
// memory (Map a staging copy on each device). Measure that copy; on PCs like an HD 2000 + GT 730 it
// is a real part of the cost.
#pragma once

#include "gpufusion.h"

#include <d3d11.h>

#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gpufusion {

struct AdapterCaps {
    std::wstring luid;                // "HHHHHHHH_LLLLLLLL", as in GPUFUSION_ADAPTERS
    std::wstring name;
    unsigned vendorId = 0;
    bool software = false;            // the Microsoft Basic Render Driver
    unsigned long long dedicatedMB = 0;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL(0); // 0: no Direct3D 11 device
    bool computeShaders = false;      // feature level 11_0+, or the optional 10.x compute support
    bool pixelShaders = false;        // shader model 4.0 pixel shaders (feature level 10_0+)
    std::wstring error;               // why no device could be created, if so
    bool usable() const { return featureLevel != D3D_FEATURE_LEVEL(0) && error.empty(); }
};

struct Device {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    AdapterCaps caps;
};

namespace detail {
inline void describeAdapter(IDXGIAdapter1* adapter, AdapterCaps& c)
{
    DXGI_ADAPTER_DESC1 d{};
    if (!adapter || FAILED(adapter->GetDesc1(&d)))
        return;
    c.luid = luidText(d.AdapterLuid);
    c.name = d.Description;
    c.vendorId = d.VendorId;
    c.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    c.dedicatedMB = static_cast<unsigned long long>(d.DedicatedVideoMemory) >> 20;
}
inline void describeDevice(ID3D11Device* device, AdapterCaps& c)
{
    c.featureLevel = device->GetFeatureLevel();
    c.pixelShaders = c.featureLevel >= D3D_FEATURE_LEVEL_10_0;
    c.computeShaders = c.featureLevel >= D3D_FEATURE_LEVEL_11_0;
    if (!c.computeShaders && c.pixelShaders) {
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS o{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &o, sizeof o)))
            c.computeShaders = o.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x != FALSE;
    }
}
} // namespace detail

// One device per adapter, in the same order. An adapter whose device cannot be created is left out
// (its reason is appended to *errors), so the result may be shorter than the input.
inline std::vector<Device> createDevices(const std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>>& adapters,
                                         std::wstring* errors = nullptr)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0 };
    std::vector<Device> out;
    for (const auto& adapter : adapters) {
        Device dev;
        dev.adapter = adapter;
        detail::describeAdapter(adapter.Get(), dev.caps);
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, UINT(std::size(levels)),
                                       D3D11_SDK_VERSION, &dev.device, nullptr, &dev.context);
        if (hr == E_INVALIDARG) // Direct3D 11.0 runtimes do not know 11_1
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, UINT(std::size(levels) - 1),
                                   D3D11_SDK_VERSION, &dev.device, nullptr, &dev.context);
        if (FAILED(hr)) {
            if (errors) {
                wchar_t buf[64];
                std::swprintf(buf, 64, L" (0x%08lX)", static_cast<unsigned long>(hr));
                *errors += dev.caps.name + L": no Direct3D 11 device" + buf + L". ";
            }
            continue;
        }
        detail::describeDevice(dev.device.Get(), dev.caps);
        out.push_back(std::move(dev));
    }
    return out;
}

// Capabilities of every adapter offered (creates and releases a device per adapter).
inline std::vector<AdapterCaps> describe(const std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>>& adapters)
{
    std::vector<AdapterCaps> out;
    for (const auto& adapter : adapters) {
        std::wstring error;
        std::vector<Device> one = createDevices({ adapter }, &error);
        if (one.empty()) {
            AdapterCaps c;
            detail::describeAdapter(adapter.Get(), c);
            c.error = error.empty() ? L"no Direct3D 11 device" : error;
            out.push_back(c);
        } else {
            out.push_back(one.front().caps);
        }
    }
    return out;
}

inline std::wstring featureLevelText(D3D_FEATURE_LEVEL fl)
{
    if (fl == D3D_FEATURE_LEVEL(0))
        return L"none";
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%u_%u", (unsigned(fl) >> 12) & 0xFu, (unsigned(fl) >> 8) & 0xFu);
    return buf;
}

} // namespace gpufusion
