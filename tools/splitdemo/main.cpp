// GPUFusionSplitDemo — a small program that integrates the GPU Fusion SDK, as a third-party program
// would, and divides one measurable workload between the GPUs it is offered. No Qt; uses only the
// public SDK headers (sdk/include/gpufusion) and the hash pixel shader of GPU Fusion's workloads.
//
// Workload: an image of integer hashes (per pixel, `iterations` xorshift steps — the same as GPU
// Fusion's Workload Lab), computed with a Direct3D 11 pixel shader (feature level 10_0+, so the Intel
// HD 2000 can run it) in batches of rows. Rows are the SDK scheduler's work units; each batch is
// copied back to system memory into one image.
//
// What it does, and prints as JSON:
//   1. the adapters offered (GPUFUSION_ADAPTERS when GPU Fusion launched it, otherwise the default
//      adapter, or every hardware adapter with --all-adapters) and their capabilities;
//   2. each GPU alone, --runs times: the single-GPU baselines (median, spread), output checked against
//      the CPU on sample rows; the fastest GPU becomes the primary;
//   3. the adaptive scheduler with a Balancer (--policy), --runs times (or for --seconds): per run the
//      plan, rows per GPU, end-to-end time, transfer, overhead; every image compared bit for bit with
//      the primary's single-GPU image;
//   4. the verdict against the fastest single GPU, using the same significance rule as GPU Fusion
//      (difference > 1 % and > twice the combined standard error); otherwise "no significant
//      difference" — never a speed-up that was not measured.
// It writes no files and changes no settings. Exit code: 0 = all output correct, 1 = wrong output or
// a failure, 2 = bad arguments.
//
//   GPUFusionSplitDemo.exe [--size quick|standard|heavy] [--runs N] [--policy conservative|balanced|performance]
//                          [--seconds S] [--all-adapters] [--no-baselines]
// --no-baselines skips step 2 (no per-GPU runs, no verdict): only the adaptive scheduler runs, with the
// adapters in the order offered; its first (primary-alone) image is the reference. Used when GPU Fusion
// verifies a "Both GPUs" profile, so that any work on the second GPU comes from the split itself.
#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion.h>
#include <gpufusion/scheduler.h>

#include "shaders/fullscreen_vs.h"
#include "shaders/hash_ps.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Params {
    std::uint32_t width = 1024, height = 1024, iterations = 512, seed = 12345, bandRows = 128;
};

// Same as gf::work::hashPixel and hash_ps.hlsl.
std::uint32_t hashPixel(std::uint32_t x, std::uint32_t y, std::uint32_t seed, std::uint32_t iterations)
{
    std::uint32_t s = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
    if (!s)
        s = 0x9E3779B9u;
    for (std::uint32_t i = 0; i < iterations; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        s *= 0x2545F491u;
    }
    return s;
}

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string escape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    return o;
}

std::string num(double v, int decimals = 3)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// The workload on one device: pipeline objects created once; the render target grows to the largest
// batch and is reused (batches vary in size).
class HashWorker {
public:
    bool init(const gpufusion::Device& d, std::string* error)
    {
        m_device = d.device;
        m_context = d.context;
        HRESULT h = m_device->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &m_vs);
        if (SUCCEEDED(h))
            h = m_device->CreatePixelShader(g_hash_ps, sizeof(g_hash_ps), nullptr, &m_ps);
        if (SUCCEEDED(h)) {
            D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            h = m_device->CreateBuffer(&bd, nullptr, &m_constants);
        }
        if (FAILED(h))
            *error = "could not create the shaders (0x" + hex(h) + ")";
        // Waiting for the GPU without spinning a CPU core: DXGI signals an event when the queued
        // work is done (IDXGIDevice2, Windows 8+). Without it, Map waits, which some drivers do by spinning.
        if (SUCCEEDED(h) && SUCCEEDED(m_device.As(&m_dxgi)))
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return SUCCEEDED(h);
    }
    ~HashWorker()
    {
        if (m_event)
            CloseHandle(m_event);
    }
    HashWorker() = default;
    HashWorker(HashWorker&& o) noexcept { *this = std::move(o); }
    HashWorker& operator=(HashWorker&& o) noexcept
    {
        std::swap(m_device, o.m_device);
        std::swap(m_context, o.m_context);
        std::swap(m_dxgi, o.m_dxgi);
        std::swap(m_vs, o.m_vs);
        std::swap(m_ps, o.m_ps);
        std::swap(m_constants, o.m_constants);
        std::swap(m_target, o.m_target);
        std::swap(m_staging, o.m_staging);
        std::swap(m_rtv, o.m_rtv);
        std::swap(m_width, o.m_width);
        std::swap(m_rows, o.m_rows);
        std::swap(m_event, o.m_event);
        return *this;
    }

    // Rows [first, first + rows) into dest (width * rows values). transferMs: the copy to system memory.
    bool compute(const Params& p, std::uint32_t first, std::uint32_t rows, std::uint32_t* dest, double* transferMs)
    {
        if (!ensureTarget(p.width, rows))
            return false;
        const std::uint32_t cb[4] = { p.iterations, p.seed, first, 0 };
        m_context->UpdateSubresource(m_constants.Get(), 0, nullptr, cb, 0, 0);
        ID3D11RenderTargetView* view = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &view, nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11Buffer* cbuffer = m_constants.Get();
        m_context->PSSetConstantBuffers(0, 1, &cbuffer);
        for (std::uint32_t y = 0; y < rows; y += p.bandRows) { // short draws: far below the driver's timeout
            const D3D11_VIEWPORT vp{ 0.0f, float(y), float(p.width), float(std::min(p.bandRows, rows - y)), 0.0f, 1.0f };
            m_context->RSSetViewports(1, &vp);
            m_context->Draw(3, 0);
            m_context->Flush();
        }
        const auto t = std::chrono::steady_clock::now();
        const D3D11_BOX box{ 0, 0, 0, p.width, rows, 1 };
        m_context->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, m_target.Get(), 0, &box);
        if (m_event && SUCCEEDED(m_dxgi->EnqueueSetEvent(m_event)))
            WaitForSingleObject(m_event, 30000);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &m))) // waits for the GPU
            return false;
        for (std::uint32_t y = 0; y < rows; ++y)
            std::memcpy(dest + std::size_t(y) * p.width, static_cast<const std::uint8_t*>(m.pData) + std::size_t(y) * m.RowPitch,
                        p.width * sizeof(std::uint32_t));
        m_context->Unmap(m_staging.Get(), 0);
        if (transferMs)
            *transferMs = msSince(t); // includes waiting for the GPU to finish the draws
        return true;
    }

private:
    static std::string hex(HRESULT h)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08lX", static_cast<unsigned long>(h));
        return buf;
    }
    bool ensureTarget(std::uint32_t width, std::uint32_t rows)
    {
        if (m_target && m_width == width && rows <= m_rows)
            return true;
        rows = std::max(rows, m_width == width ? m_rows : 0u);
        m_target.Reset();
        m_staging.Reset();
        m_rtv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = rows;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT h = m_device->CreateTexture2D(&td, nullptr, &m_target);
        if (SUCCEEDED(h))
            h = m_device->CreateRenderTargetView(m_target.Get(), nullptr, &m_rtv);
        if (SUCCEEDED(h)) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            h = m_device->CreateTexture2D(&td, nullptr, &m_staging);
        }
        if (FAILED(h)) {
            m_target.Reset();
            return false;
        }
        m_width = width;
        m_rows = rows;
        return true;
    }

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIDevice2> m_dxgi;
    HANDLE m_event = nullptr;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11Texture2D> m_target, m_staging;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    std::uint32_t m_width = 0, m_rows = 0;
};

struct Stats {
    int n = 0;
    double mean = 0, sd = 0, median = 0, min = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
    Stats s;
    s.n = int(v.size());
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    for (double x : v)
        s.mean += x;
    s.mean /= v.size();
    for (double x : v)
        s.sd += (x - s.mean) * (x - s.mean);
    s.sd = v.size() > 1 ? std::sqrt(s.sd / (v.size() - 1)) : 0;
    return s;
}

std::string statsJson(const Stats& s)
{
    return "{\"n\":" + std::to_string(s.n) + ",\"mean\":" + num(s.mean) + ",\"sd\":" + num(s.sd) + ",\"median\":" + num(s.median)
         + ",\"min\":" + num(s.min) + ",\"max\":" + num(s.max) + "}";
}

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return double((std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Params p;
    int runs = 12;
    double seconds = 0;
    bool allAdapters = false, baselines = true;
    gpufusion::Policy policy = gpufusion::Policy::Conservative;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == L"--size" && hasValue) {
            const std::wstring v = argv[++i];
            if (v == L"quick") { p.width = p.height = 512; p.iterations = 256; }
            else if (v == L"standard") { p.width = p.height = 1024; p.iterations = 512; }
            else if (v == L"heavy") { p.width = p.height = 2048; p.iterations = 512; }
            else return 2;
        } else if (a == L"--runs" && hasValue) {
            runs = std::clamp(_wtoi(argv[++i]), 1, 1000);
        } else if (a == L"--seconds" && hasValue) {
            seconds = std::clamp(std::wcstod(argv[++i], nullptr), 0.0, 3600.0);
        } else if (a == L"--policy" && hasValue) {
            if (!gpufusion::parsePolicy(utf8(argv[++i]), &policy))
                return 2;
        } else if (a == L"--all-adapters") {
            allAdapters = true;
        } else if (a == L"--no-baselines") {
            baselines = false;
        } else {
            std::fprintf(stderr, "usage: GPUFusionSplitDemo [--size quick|standard|heavy] [--runs N] [--policy conservative|balanced|performance] "
                                 "[--seconds S] [--all-adapters] [--no-baselines]\n");
            return 2;
        }
    }

    std::string json = "{\"demo\":\"GPUFusionSplitDemo\",\"sdkVersion\":" + std::to_string(gpufusion::kSdkVersion)
                     + ",\"schedulerVersion\":" + std::to_string(gpufusion::kSchedulerVersion) + ",\"pid\":" + std::to_string(GetCurrentProcessId());
    json += ",\"launchedByGpuFusion\":" + std::string(gpufusion::mode() != gpufusion::Mode::NotLaunchedByGpuFusion ? "true" : "false");
    json += ",\"environmentVersion\":" + std::to_string(gpufusion::environmentVersion());
    json += ",\"workload\":{\"width\":" + std::to_string(p.width) + ",\"height\":" + std::to_string(p.height) + ",\"iterations\":"
          + std::to_string(p.iterations) + ",\"unit\":\"row\"}";
    json += std::string(",\"policy\":\"") + gpufusion::policyName(policy) + "\"";

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::puts((json + ",\"error\":\"DXGI is not available\"}").c_str());
        return 1;
    }
    // 1. The adapters: those GPU Fusion offered (in its order: the discrete GPU first in "Both"), or
    //    the default one, or with --all-adapters every hardware adapter.
    std::vector<ComPtr<IDXGIAdapter1>> offered = gpufusion::adapters(factory.Get());
    if (allAdapters && gpufusion::requestedLuids().empty()) {
        offered.clear();
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                offered.push_back(a);
        }
    }
    std::wstring deviceErrors;
    std::vector<gpufusion::Device> devices = gpufusion::createDevices(offered, &deviceErrors);
    std::vector<HashWorker> workers(devices.size());
    std::vector<gpufusion::Device> usable;
    std::vector<HashWorker> usableWorkers;
    json += ",\"adapters\":[";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        std::string error;
        const gpufusion::AdapterCaps& c = devices[i].caps;
        const bool ok = c.pixelShaders && workers[i].init(devices[i], &error);
        if (!c.pixelShaders)
            error = "feature level below 10_0: no shader model 4 pixel shaders";
        json += std::string(i ? "," : "") + "{\"name\":\"" + escape(utf8(c.name)) + "\",\"luid\":\"" + utf8(c.luid) + "\",\"vendorId\":"
              + std::to_string(c.vendorId) + ",\"featureLevel\":\"" + utf8(gpufusion::featureLevelText(c.featureLevel)) + "\",\"computeShaders\":"
              + (c.computeShaders ? "true" : "false") + ",\"dedicatedMB\":" + std::to_string(c.dedicatedMB) + ",\"usable\":" + (ok ? "true" : "false")
              + (ok ? "" : ",\"skipped\":\"" + escape(error) + "\"") + "}";
        if (ok) {
            usable.push_back(devices[i]);
            usableWorkers.push_back(std::move(workers[i]));
        }
    }
    json += "]";
    if (!deviceErrors.empty())
        json += ",\"deviceErrors\":\"" + escape(utf8(deviceErrors)) + "\"";
    if (usable.empty()) {
        std::puts((json + ",\"error\":\"no adapter can run the workload\"}").c_str());
        return 1;
    }

    std::vector<std::uint32_t> image(std::size_t(p.width) * p.height), reference;
    auto runOn = [&](std::size_t device, std::uint32_t first, std::uint32_t count, double* transferMs) {
        return usableWorkers[device].compute(p, first, count, image.data() + std::size_t(first) * p.width, transferMs);
    };

    // 2. Each GPU alone, through the same scheduler (one device, same batch size as a primary alone),
    //    after a short warm-up that wakes it from its idle clocks.
    bool allCorrect = true;
    std::vector<Stats> single(usable.size());
    std::vector<std::vector<std::uint32_t>> singleImages(usable.size());
    json += ",\"single\":[";
    for (std::size_t d = 0; d < usable.size() && baselines; ++d) {
        gpufusion::Plan plan;
        plan.enabled = { 1 };
        plan.share = { 1.0 };
        plan.batch = { gpufusion::detail::roundUp(p.height / 8.0, 8) };
        plan.granularity = 8;
        auto once = [&] {
            std::fill(image.begin(), image.end(), 0xDEADBEEFu);
            return gpufusion::run(p.height, plan, [&](gpufusion::Batch& b) { return runOn(d, b.first, b.count, &b.transferMs); });
        };
        const auto warm = std::chrono::steady_clock::now();
        while (msSince(warm) < 1000)
            once();
        std::vector<double> wall;
        bool ok = true;
        const double cpuBefore = processCpuMs();
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < runs && ok; ++k) {
            const gpufusion::RunReport r = once();
            ok = r.ok;
            wall.push_back(r.wallMs);
        }
        const double cpuPercent = 100.0 * (processCpuMs() - cpuBefore) / std::max(1.0, msSince(t0)) / std::max(1u, std::thread::hardware_concurrency());
        // The single-GPU output must match the CPU on sample rows (first, middle, last).
        for (std::uint32_t y : { 0u, p.height / 2, p.height - 1 })
            for (std::uint32_t x = 0; x < p.width && ok; ++x)
                ok = image[std::size_t(y) * p.width + x] == hashPixel(x, y, p.seed, p.iterations);
        allCorrect = allCorrect && ok;
        single[d] = stats(wall);
        singleImages[d] = image;
        json += std::string(d ? "," : "") + "{\"adapter\":\"" + escape(utf8(usable[d].caps.name)) + "\",\"wallMs\":" + statsJson(single[d])
              + ",\"correctVsCpuSamples\":" + (ok ? "true" : "false") + ",\"processCpuPercentOfAllCores\":" + num(cpuPercent, 1) + "}";
    }
    json += "]";
    for (std::size_t d = 1; d < usable.size(); ++d)
        if (singleImages[d] != singleImages[0]) {
            allCorrect = false;
            json += ",\"warning\":\"the GPUs' single-GPU images differ\"";
        }

    // The primary: the fastest GPU alone (normally the first one GPU Fusion offered, the discrete GPU).
    std::vector<std::size_t> order(usable.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    if (baselines)
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return single[a].median < single[b].median; });
    reference = singleImages[order[0]]; // empty without baselines: set from the first adaptive run
    json += ",\"primary\":\"" + escape(utf8(usable[order[0]].caps.name)) + "\"";

    // 3. The adaptive scheduler. The Balancer measures the primary alone first, then splits, and
    //    adapts after every run; it learns during the same warm-up as the single runs.
    gpufusion::Balancer balancer(usable.size(), policy, 8);
    std::vector<double> splitWall, allWall;
    std::vector<std::uint64_t> rowsTotal(usable.size(), 0); // by position in `order`
    int splitRuns = 0, runsDone = 0;
    std::string runsJson;
    const double cpu0 = processCpuMs();
    const auto start = std::chrono::steady_clock::now();
    const auto warm = std::chrono::steady_clock::now();
    auto adaptiveOnce = [&](bool record) {
        const gpufusion::Plan plan = balancer.plan(p.height);
        std::fill(image.begin(), image.end(), 0xDEADBEEFu);
        const gpufusion::RunReport r = gpufusion::run(p.height, plan, [&](gpufusion::Batch& b) {
            return runOn(order[b.device], b.first, b.count, &b.transferMs);
        }, balancer.unitsPerMs());
        balancer.record(plan, r);
        if (reference.empty() && r.ok && !plan.multi()) { // --no-baselines: the primary alone is the reference,
            bool cpuOk = true;                             // itself checked against the CPU on sample rows
            for (std::uint32_t y : { 0u, p.height / 2, p.height - 1 })
                for (std::uint32_t x = 0; x < p.width && cpuOk; ++x)
                    cpuOk = image[std::size_t(y) * p.width + x] == hashPixel(x, y, p.seed, p.iterations);
            allCorrect = allCorrect && cpuOk;
            reference = image;
        }
        const bool correct = r.ok && image == reference;
        allCorrect = allCorrect && correct;
        if (!record)
            return;
        ++runsDone;
        allWall.push_back(r.wallMs);
        if (plan.multi()) {
            ++splitRuns;
            splitWall.push_back(r.wallMs);
        }
        std::string rows, transfer;
        double schedule = 0;
        for (std::size_t d = 0; d < r.devices.size(); ++d) {
            rowsTotal[d] += r.devices[d].units;
            rows += std::string(d ? "," : "") + std::to_string(r.devices[d].units);
            transfer += std::string(d ? "," : "") + num(r.devices[d].transferMs);
            schedule += r.devices[d].scheduleMs;
        }
        if (runsJson.size() < 200000)
            runsJson += std::string(runsJson.empty() ? "" : ",") + "{\"split\":" + (plan.multi() ? "true" : "false") + ",\"wallMs\":" + num(r.wallMs)
                      + ",\"rowsPerGpu\":[" + rows + "],\"transferMs\":[" + transfer + "],\"overheadMs\":" + num(r.overheadMs())
                      + ",\"scheduleMs\":" + num(schedule, 4) + ",\"correct\":" + (correct ? "true" : "false") + ",\"plan\":\"" + escape(plan.reason) + "\"}";
    };
    while (msSince(warm) < 1000)
        adaptiveOnce(false);
    const auto measured = std::chrono::steady_clock::now();
    if (seconds > 0) {
        while (msSince(measured) < seconds * 1000)
            adaptiveOnce(true);
    } else {
        for (int k = 0; k < runs; ++k)
            adaptiveOnce(true);
    }
    const double elapsed = msSince(start);
    const double cpu = processCpuMs() - cpu0;
    json += ",\"adaptive\":{\"runs\":" + std::to_string(runsDone) + ",\"splitRuns\":" + std::to_string(splitRuns) + ",\"wallMs\":" + statsJson(stats(allWall))
          + ",\"splitWallMs\":" + statsJson(stats(splitWall)) + ",\"runsDetail\":[" + runsJson + "]";
    json += ",\"rowsPercentPerGpu\":[";
    for (std::size_t d = 0; d < rowsTotal.size(); ++d)
        json += std::string(d ? "," : "") + "{\"adapter\":\"" + escape(utf8(usable[order[d]].caps.name)) + "\",\"percent\":"
              + num(runsDone ? 100.0 * double(rowsTotal[d]) / (double(runsDone) * p.height) : 0, 2) + "}";
    json += "]";
    for (std::size_t d = 1; d < usable.size(); ++d) {
        const gpufusion::Balancer::Helper& h = balancer.helper(d);
        json += ",\"helper" + std::to_string(d) + "\":{\"adapter\":\"" + escape(utf8(usable[order[d]].caps.name)) + "\",\"enabled\":"
              + (h.enabled ? "true" : "false") + ",\"nextShare\":" + num(h.share) + ",\"note\":\"" + escape(h.note) + "\"}";
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    json += ",\"processCpuPercentOfAllCores\":" + num(100.0 * cpu / elapsed / std::max<DWORD>(1, si.dwNumberOfProcessors), 1)
          + ",\"peakWorkingSetMB\":" + num(double(pmc.PeakWorkingSetSize) / 1048576.0, 1) + "}";

    // 4. The verdict against the fastest single GPU (all adaptive runs: what a program actually gets).
    const Stats a = stats(allWall), s = baselines ? single[order[0]] : Stats{};
    const std::size_t usedGpus = std::size_t(std::count_if(rowsTotal.begin(), rowsTotal.end(), [](std::uint64_t r) { return r > 0; }));
    std::string verdict = "no significant difference";
    if (a.n >= 2 && s.n >= 2) {
        const double se = std::sqrt(a.sd * a.sd / a.n + s.sd * s.sd / s.n);
        const double diff = s.mean - a.mean;
        if (std::abs(diff) > 2 * se && std::abs(diff) > 0.01 * s.mean)
            verdict = diff > 0 ? "faster" : "slower";
    } else {
        verdict = baselines ? "not enough runs" : "not compared (--no-baselines)";
    }
    json += ",\"verdict\":{\"comparedWith\":\"" + escape(utf8(usable[order[0]].caps.name)) + " alone\",\"result\":\"" + verdict + "\",\"speedup\":"
          + num(a.mean > 0 ? s.mean / a.mean : 0) + ",\"usedGpus\":" + std::to_string(usedGpus) + "}";
    json += ",\"allOutputCorrect\":" + std::string(allCorrect ? "true" : "false") + "}";
    std::puts(json.c_str());
    return allCorrect ? 0 : 1;
}
