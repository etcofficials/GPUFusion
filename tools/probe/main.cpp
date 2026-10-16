// GPUFusionProbe — a harmless helper that reports which graphics adapter Windows gives it.
//
// GPU Fusion launches it (and only it) under different per-app GPU preferences to verify what a
// preference actually does on this PC. It creates a Direct3D 11 device the way a typical
// application does (default adapter), reports that adapter, the DXGI enumeration order and what
// IDXGIFactory6::EnumAdapterByGpuPreference returns, prints one JSON object to stdout and exits.
// It renders nothing, opens no window, writes no files and changes no settings.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

std::string utf8(const wchar_t* w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

std::string adapterJson(IDXGIAdapter* a)
{
    if (!a)
        return "null";
    DXGI_ADAPTER_DESC d{};
    a->GetDesc(&d);
    char buf[512];
    snprintf(buf, sizeof buf, "{\"name\":\"%s\",\"vendorId\":%u,\"deviceId\":%u,\"luid\":\"%08lX_%08lX\"}",
             escape(utf8(d.Description)).c_str(), d.VendorId, d.DeviceId, static_cast<unsigned long>(d.AdapterLuid.HighPart),
             static_cast<unsigned long>(d.AdapterLuid.LowPart));
    return buf;
}

} // namespace

int main()
{
    std::string json = "{\"probeVersion\":1";
    wchar_t exe[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    json += ",\"exe\":\"" + escape(utf8(exe)) + "\",\"pid\":" + std::to_string(GetCurrentProcessId());

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        json += ",\"error\":\"CreateDXGIFactory1 failed\"}";
        puts(json.c_str());
        return 2;
    }

    // Enumeration order as this process sees it (a preference may reorder it).
    json += ",\"enumOrder\":[";
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
        json += (i ? "," : "") + adapterJson(a.Get());
    json += "]";

    // What a preference-aware application gets.
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6))) {
        const struct { const char* key; DXGI_GPU_PREFERENCE pref; } prefs[] = {
            { "unspecified", DXGI_GPU_PREFERENCE_UNSPECIFIED },
            { "minimumPower", DXGI_GPU_PREFERENCE_MINIMUM_POWER },
            { "highPerformance", DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE },
        };
        json += ",\"byPreference\":{";
        bool first = true;
        for (const auto& p : prefs) {
            ComPtr<IDXGIAdapter1> pa;
            const HRESULT hr = f6->EnumAdapterByGpuPreference(0, p.pref, IID_PPV_ARGS(&pa));
            json += std::string(first ? "" : ",") + "\"" + p.key + "\":" + (SUCCEEDED(hr) ? adapterJson(pa.Get()) : "null");
            first = false;
        }
        json += "}";
    }

    // What a typical application gets: a D3D11 device on the default adapter.
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &fl, nullptr);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> used;
        if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&used)))
            json += ",\"d3d11Default\":" + adapterJson(used.Get());
        char flText[16];
        snprintf(flText, sizeof flText, "%u_%u", (fl >> 12) & 0xFu, (fl >> 8) & 0xFu);
        json += std::string(",\"featureLevel\":\"") + flText + "\"";
    } else {
        char err[64];
        snprintf(err, sizeof err, ",\"d3d11Error\":\"0x%08lX\"", static_cast<unsigned long>(hr));
        json += err;
    }
    json += "}";
    puts(json.c_str());
    return 0;
}
