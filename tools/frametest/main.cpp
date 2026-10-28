// GPUFusionFrameTest — a small window that presents frames with one graphics API at a known pace and
// reports its own present times, so GPU Fusion's frame-time capture can be checked against ground
// truth for each API. No Qt.
//
//   GPUFusionFrameTest.exe --api d3d11|d3d12|d3d9|opengl [--seconds S] [--interval-ms N] [--adapter LUID]
//                          [--vsync] [--stall-every K --stall-ms M]
// --interval-ms paces the frames (0 = as fast as the API allows). --adapter (Direct3D 11/12 only)
// renders on that adapter. --stall-every adds a stall of M ms every K frames (known stutters).
// Prints JSON: the API, the adapter, the number of presents and their own interval statistics.
// Exit code: 0 = ran, 1 = the API could not be used here, 2 = bad arguments.
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <gl/GL.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

double g_qpcPerMs = 1;
double nowMs()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return double(q.QuadPart) / g_qpcPerMs;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    for (char& c : s)
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
            c = ' ';
    return s;
}

std::string luidText(const LUID& l)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08lX_%08lX", static_cast<unsigned long>(l.HighPart), static_cast<unsigned long>(l.LowPart));
    return buf;
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND makeWindow(const wchar_t* title)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GPUFusionFrameTest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_OWNDC;
    RegisterClassW(&wc);
    RECT r{ 0, 0, 640, 360 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                           wc.hInstance, nullptr);
    return h;
}

bool pump()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT)
            return false;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return true;
}

ComPtr<IDXGIAdapter1> findAdapter(IDXGIFactory1* f, const std::wstring& luid)
{
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        const std::string t = luidText(d.AdapterLuid);
        if (luid.empty() ? !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) : _wcsicmp(std::wstring(t.begin(), t.end()).c_str(), luid.c_str()) == 0)
            return a;
    }
    return nullptr;
}

// One API: set up on the window, then present() once per frame.
struct Renderer {
    virtual ~Renderer() = default;
    virtual bool present(int frame) = 0;
    std::string adapter, error;
};

struct D3D11Renderer : Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT sync = 0;
    bool init(HWND w, const std::wstring& luid, bool vsync)
    {
        sync = vsync ? 1 : 0;
        ComPtr<IDXGIFactory2> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
            return (error = "no DXGI"), false;
        ComPtr<IDXGIAdapter1> a = findAdapter(f.Get(), luid);
        if (!a)
            return (error = "adapter not found"), false;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        adapter = utf8(d.Description);
        if (FAILED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return (error = "no Direct3D 11 device"), false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 640;
        sd.Height = 360;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(f->CreateSwapChainForHwnd(device.Get(), w, &sd, nullptr, nullptr, &swap))) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // older systems: blt model
            sd.BufferCount = 1;
            if (FAILED(f->CreateSwapChainForHwnd(device.Get(), w, &sd, nullptr, nullptr, &swap)))
                return (error = "no swap chain"), false;
        }
        ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(&back));
        device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        return true;
    }
    bool present(int frame) override
    {
        const float c[4] = { 0.1f, float(frame % 60) / 60.0f, 0.3f, 1.0f };
        ID3D11RenderTargetView* v = rtv.Get();
        context->OMSetRenderTargets(1, &v, nullptr);
        context->ClearRenderTargetView(rtv.Get(), c);
        return SUCCEEDED(swap->Present(sync, 0));
    }
};

struct D3D12Renderer : Renderer {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> buffers[2];
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
    UINT rtvSize = 0, sync = 0;
    ~D3D12Renderer() override
    {
        if (event)
            CloseHandle(event);
    }
    bool init(HWND w, const std::wstring& luid, bool vsync)
    {
        sync = vsync ? 1 : 0;
        ComPtr<IDXGIFactory4> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
            return (error = "no DXGI 1.4"), false;
        ComPtr<IDXGIAdapter1> a = findAdapter(f.Get(), luid);
        if (!a)
            return (error = "adapter not found"), false;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        adapter = utf8(d.Description);
        if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return (error = "no Direct3D 12 device on this adapter"), false;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
            return (error = "no command queue"), false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 640;
        sd.Height = 360;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> s1;
        if (FAILED(f->CreateSwapChainForHwnd(queue.Get(), w, &sd, nullptr, nullptr, &s1)) || FAILED(s1.As(&swap)))
            return (error = "no swap chain"), false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 2;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < 2; ++i) {
            swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * rtvSize;
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
        }
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return list && fence && event;
    }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }
    bool present(int frame) override
    {
        const UINT i = swap->GetCurrentBackBufferIndex();
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        barrier(buffers[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtvSize;
        const float c[4] = { 0.3f, float(frame % 60) / 60.0f, 0.1f, 1.0f };
        list->ClearRenderTargetView(h, c, 0, nullptr);
        barrier(buffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        const bool ok = SUCCEEDED(swap->Present(sync, 0));
        queue->Signal(fence.Get(), ++fenceValue); // simple: wait for the frame before reusing the allocator
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, event);
            WaitForSingleObject(event, 5000);
        }
        return ok;
    }
};

struct D3D9Renderer : Renderer {
    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> device;
    bool init(HWND w, bool vsync)
    {
        d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!d3d)
            return (error = "no Direct3D 9"), false;
        D3DADAPTER_IDENTIFIER9 id{};
        d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
        adapter = id.Description;
        for (char& c : adapter)
            if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
                c = ' ';
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.hDeviceWindow = w;
        pp.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
        if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)))
            return (error = "no Direct3D 9 device"), false;
        return true;
    }
    bool present(int frame) override
    {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(30, (frame % 60) * 4, 120), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        return SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr));
    }
};

struct OpenGlRenderer : Renderer {
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    ~OpenGlRenderer() override
    {
        if (rc) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc);
        }
    }
    bool init(HWND w, bool vsync)
    {
        dc = GetDC(w);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        const int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd) || !(rc = wglCreateContext(dc)) || !wglMakeCurrent(dc, rc))
            return (error = "no OpenGL context"), false;
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        adapter = std::string(r ? r : "?") + " (OpenGL " + (v ? v : "?") + ")";
        for (char& c : adapter)
            if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
                c = ' ';
        using SwapInterval = BOOL(WINAPI*)(int);
        if (auto si = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT")))
            si(vsync ? 1 : 0);
        return true;
    }
    bool present(int frame) override
    {
        glClearColor(0.2f, float(frame % 60) / 60.0f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return SwapBuffers(dc) != FALSE;
    }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring api, luid;
    double seconds = 10, intervalMs = 0, stallMs = 0;
    int stallEvery = 0;
    bool vsync = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--api" && more) api = argv[++i];
        else if (a == L"--seconds" && more) seconds = std::clamp(std::wcstod(argv[++i], nullptr), 1.0, 600.0);
        else if (a == L"--interval-ms" && more) intervalMs = std::clamp(std::wcstod(argv[++i], nullptr), 0.0, 1000.0);
        else if (a == L"--adapter" && more) luid = argv[++i];
        else if (a == L"--vsync") vsync = true;
        else if (a == L"--stall-every" && more) stallEvery = std::max(0, _wtoi(argv[++i]));
        else if (a == L"--stall-ms" && more) stallMs = std::clamp(std::wcstod(argv[++i], nullptr), 0.0, 1000.0);
        else {
            std::fprintf(stderr, "usage: GPUFusionFrameTest --api d3d11|d3d12|d3d9|opengl [--seconds S] [--interval-ms N] [--adapter LUID] [--vsync] "
                                 "[--stall-every K --stall-ms M]\n");
            return 2;
        }
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerMs = double(f.QuadPart) / 1000.0;
    timeBeginPeriod(1);
    HWND w = makeWindow((L"GPU Fusion frame test - " + api).c_str());
    std::unique_ptr<Renderer> r;
    bool ok = false;
    if (api == L"d3d11") { auto x = std::make_unique<D3D11Renderer>(); ok = x->init(w, luid, vsync); r = std::move(x); }
    else if (api == L"d3d12") { auto x = std::make_unique<D3D12Renderer>(); ok = x->init(w, luid, vsync); r = std::move(x); }
    else if (api == L"d3d9") { auto x = std::make_unique<D3D9Renderer>(); ok = x->init(w, vsync); r = std::move(x); }
    else if (api == L"opengl") { auto x = std::make_unique<OpenGlRenderer>(); ok = x->init(w, vsync); r = std::move(x); }
    else return 2;
    const std::string head = "{\"tool\":\"GPUFusionFrameTest\",\"api\":\"" + utf8(api) + "\",\"pid\":" + std::to_string(GetCurrentProcessId())
                           + ",\"adapter\":\"" + r->adapter + "\"";
    if (!ok) {
        std::printf("%s,\"ok\":false,\"error\":\"%s\"}\n", head.c_str(), r->error.c_str());
        return 1;
    }
    // Present times, taken just before each present call (the time the capture should see).
    std::vector<double> t;
    t.reserve(100000);
    const double start = nowMs();
    double next = start;
    int failures = 0;
    for (int frame = 0; pump() && nowMs() - start < seconds * 1000; ++frame) {
        if (intervalMs > 0) {
            next += intervalMs;
            for (double left = next - nowMs(); left > 0; left = next - nowMs()) {
                if (left > 2)
                    Sleep(DWORD(left - 1.5));
            }
        }
        if (stallEvery > 0 && frame > 0 && frame % stallEvery == 0) {
            const double until = nowMs() + stallMs;
            while (nowMs() < until)
                Sleep(0);
            next = nowMs(); // the stall delays later frames rather than being caught up
        }
        t.push_back(nowMs() - start);
        failures += r->present(frame) ? 0 : 1;
    }
    std::vector<double> iv;
    for (std::size_t i = 1; i < t.size(); ++i)
        iv.push_back(t[i] - t[i - 1]);
    std::vector<double> sorted = iv;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, std::size_t(p * (sorted.size() - 1) + 0.5))]; };
    double mean = 0;
    for (double x : iv)
        mean += x;
    mean = iv.empty() ? 0 : mean / iv.size();
    int stalls = 0;
    for (double x : iv)
        stalls += x > 2 * pct(0.5) && x > pct(0.5) + 4 ? 1 : 0;
    std::printf("%s,\"ok\":true,\"vsync\":%s,\"intervalMs\":%.3f,\"presents\":%zu,\"failedPresents\":%d,\"firstPresentMs\":%.3f,"
                "\"frameTimeMs\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f},\"longFrames\":%d}\n",
                head.c_str(), vsync ? "true" : "false", intervalMs, t.size(), failures, t.empty() ? 0.0 : t.front(), mean, pct(0.5), pct(0.95),
                pct(0.99), sorted.empty() ? 0.0 : sorted.back(), stalls);
    timeEndPeriod(1);
    DestroyWindow(w);
    return 0;
}
