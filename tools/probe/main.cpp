// GPUFusionProbe — a harmless helper that reports which graphics adapter Windows gives it.
//
// GPU Fusion launches it (and only it) under different per-app GPU preferences to verify what a
// preference actually does on this PC. It creates a Direct3D 11 device the way a typical
// application does (default adapter), reports that adapter, the DXGI enumeration order and what
// IDXGIFactory6::EnumAdapterByGpuPreference returns, prints one JSON object to stdout and exits.
// It writes no files and changes no settings, and opens no window unless asked to (--window).
//
//   GPUFusionProbe.exe                      report and exit (used by the preference verification)
//   ... --window <seconds>                  also show a small window and keep running until it is closed
//                                           (or the time is up) — a stand-in for a running program that
//                                           GPU Fusion may ask to close; ... --ignore-close refuses to close
//   GPUFusionProbe.exe --render <seconds>   also render for <seconds> on the device it got, so GPU
//                                           Fusion can observe which GPU's 3D engine does the work
//   ... --adapter <HHHHHHHH_LLLLLLLL>       create the device on that adapter (LUID) instead of the
//                                           default — what an application with its own GPU setting does
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cwchar>

#include <thread>
#include <vector>

#include <gpufusion/gpufusion.h> // the GPU Fusion SDK (sdk/include)

// The workload shaders, compiled at build time (see src/CMakeLists.txt).
#include "shaders/fullscreen_vs.h"
#include "shaders/hash_ps.h"

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

std::string luidText(const LUID& l)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%08lX_%08lX", static_cast<unsigned long>(l.HighPart), static_cast<unsigned long>(l.LowPart));
    return buf;
}

// Draws a full-screen pixel-shader pass repeatedly for `seconds`, waiting for the GPU every few
// draws so work keeps flowing without an unbounded queue. Returns the number of draws.
long long render(ID3D11Device* device, double seconds, std::string* error)
{
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 512;
    td.Height = 512;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    const unsigned constants[4] = { 64, 12345, 0, 0 };
    D3D11_SUBRESOURCE_DATA init{ constants, 0, 0 };
    if (FAILED(device->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(g_hash_ps, sizeof(g_hash_ps), nullptr, &ps)) ||
        FAILED(device->CreateBuffer(&bd, &init, &cb)) || FAILED(device->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv))) {
        *error = "could not create the rendering resources";
        return 0;
    }
    ID3D11RenderTargetView* views[] = { rtv.Get() };
    ctx->OMSetRenderTargets(1, views, nullptr);
    const D3D11_VIEWPORT vp{ 0, 0, 512, 512, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { cb.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);
    D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> done;
    device->CreateQuery(&qd, &done);

    const auto start = std::chrono::steady_clock::now();
    long long draws = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        for (int i = 0; i < 4; ++i, ++draws)
            ctx->Draw(3, 0);
        ctx->End(done.Get());
        ctx->Flush();
        BOOL finished = FALSE;
        while (ctx->GetData(done.Get(), &finished, sizeof(finished), 0) == S_FALSE)
            Sleep(0);
    }
    return draws;
}

bool g_ignoreClose = false;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        if (!g_ignoreClose)
            DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows a small window (minimised, not activated) and pumps messages until it is closed or `seconds`
// pass. Returns true if it was closed by a close request.
bool runWindow(double seconds)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GPUFusionProbeWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GPU Fusion probe (test window)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                320, 120, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        return false;
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    const auto start = std::chrono::steady_clock::now();
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= seconds) {
            DestroyWindow(hwnd);
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    double renderSeconds = 0, windowSeconds = 0;
    std::wstring wantedLuid;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--render") == 0 && i + 1 < argc)
            renderSeconds = std::wcstod(argv[++i], nullptr);
        else if (wcscmp(argv[i], L"--adapter") == 0 && i + 1 < argc)
            wantedLuid = argv[++i];
        else if (wcscmp(argv[i], L"--window") == 0 && i + 1 < argc)
            windowSeconds = std::wcstod(argv[++i], nullptr);
        else if (wcscmp(argv[i], L"--ignore-close") == 0)
            g_ignoreClose = true;
    }
    if (windowSeconds > 0) {
        const bool closed = runWindow(windowSeconds);
        printf("{\"probeVersion\":2,\"pid\":%lu,\"window\":{\"closedByRequest\":%s}}\n", GetCurrentProcessId(), closed ? "true" : "false");
        return 0;
    }

    std::string json = "{\"probeVersion\":2";
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

    // What a typical application gets: a D3D11 device on the default adapter — or, with --adapter,
    // on the adapter it asked for (as applications with their own GPU setting do).
    ComPtr<IDXGIAdapter1> chosen;
    if (!wantedLuid.empty()) {
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            const std::string l = luidText(d.AdapterLuid);
            if (_wcsicmp(std::wstring(l.begin(), l.end()).c_str(), wantedLuid.c_str()) == 0)
                chosen = a;
        }
        if (!chosen) {
            json += ",\"error\":\"requested adapter not found\"}";
            puts(json.c_str());
            return 3;
        }
    }
    // Reference integration of the GPU Fusion SDK: launched from a GPU Fusion profile, use the adapters
    // the profile chose (several in "Both" mode); otherwise behave like any program (default adapter).
    std::vector<ComPtr<IDXGIAdapter1>> sdkAdapters;
    if (!chosen && !gpufusion::requestedLuids().empty()) {
        sdkAdapters = gpufusion::adapters(factory.Get());
        if (!sdkAdapters.empty())
            chosen = sdkAdapters.front();
        json += std::string(",\"sdk\":{\"mode\":") + std::to_string(int(gpufusion::mode())) + ",\"adapters\":" + std::to_string(sdkAdapters.size()) + "}";
    }
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &device, &fl, nullptr);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> used;
        if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&used)))
            json += std::string(chosen ? ",\"d3d11Chosen\":" : ",\"d3d11Default\":") + adapterJson(used.Get());
        char flText[16];
        snprintf(flText, sizeof flText, "%u_%u", (fl >> 12) & 0xFu, (fl >> 8) & 0xFu);
        json += std::string(",\"featureLevel\":\"") + flText + "\"";
        if (renderSeconds > 0) {
            // "Both" via the SDK: the same independent rendering work on every additional requested
            // adapter at the same time (each GPU does its own share; nothing is split within a frame).
            std::vector<ComPtr<ID3D11Device>> extraDevices;
            if (gpufusion::mode() == gpufusion::Mode::Both) {
                for (size_t i = 1; i < sdkAdapters.size(); ++i) {
                    ComPtr<ID3D11Device> d;
                    if (SUCCEEDED(D3D11CreateDevice(sdkAdapters[i].Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, nullptr)))
                        extraDevices.push_back(d);
                }
            }
            std::vector<long long> extraDraws(extraDevices.size(), 0);
            std::vector<std::thread> threads;
            for (size_t i = 0; i < extraDevices.size(); ++i)
                threads.emplace_back([&, i] {
                    std::string ignored;
                    extraDraws[i] = render(extraDevices[i].Get(), renderSeconds, &ignored);
                });
            std::string error;
            const long long draws = render(device.Get(), renderSeconds, &error);
            for (std::thread& t : threads)
                t.join();
            json += ",\"render\":{\"seconds\":" + std::to_string(renderSeconds) + ",\"draws\":" + std::to_string(draws);
            for (size_t i = 0; i < extraDraws.size(); ++i)
                json += ",\"drawsAdapter" + std::to_string(i + 2) + "\":" + std::to_string(extraDraws[i]);
            if (!error.empty())
                json += ",\"error\":\"" + escape(error) + "\"";
            json += "}";
        }
    } else {
        char err[64];
        snprintf(err, sizeof err, ",\"d3d11Error\":\"0x%08lX\"", static_cast<unsigned long>(hr));
        json += err;
    }
    json += "}";
    puts(json.c_str());
    return 0;
}
