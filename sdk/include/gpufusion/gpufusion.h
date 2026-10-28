// GPU Fusion SDK — optional integration for programs that want to honour the GPU choice made in
// GPU Fusion, including "Both GPUs". Header-only, C++17, Windows, DXGI; MIT License. Version 2.
//
// How it works: when GPU Fusion launches a program from its profile (or runs it as a job of a batch)
// it sets
//     GPUFUSION_ENV_VERSION  1 — the format of these variables (missing: GPU Fusion before 1.12, same format)
//     GPUFUSION_MODE         automatic | integrated | discrete | both
//     GPUFUSION_ADAPTERS     comma-separated adapter LUIDs "HHHHHHHH_LLLLLLLL", in order of use
//                            (in "both": the discrete GPU first — the primary — then the integrated one)
//     GPUFUSION_PROFILE      the profile id
// A program that includes this header asks for its adapters and creates its devices on them. A
// program launched any other way simply gets the default adapter — nothing breaks.
//
// The SDK has three headers:
//     gpufusion.h   this one: mode, requested adapters, a weighted split
//     d3d11.h       capabilities of each adapter and one Direct3D 11 device per adapter
//     scheduler.h   NVIDIA-first adaptive scheduling of independent work units over those devices,
//                   with measured per-device times, a single-device fallback and the Balancer, which
//                   stops using a helper GPU when splitting is not measurably faster
//
//     #include <gpufusion/d3d11.h>
//     #include <gpufusion/scheduler.h>
//     Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
//     CreateDXGIFactory1(IID_PPV_ARGS(&factory));
//     if (!gpufusion::environmentSupported()) { /* newer GPU Fusion: use the default adapter */ }
//     auto devices = gpufusion::createDevices(gpufusion::adapters(factory.Get())); // at least one entry
//     gpufusion::Balancer balancer(devices.size(), gpufusion::Policy::Conservative);
//     // ... per job: balancer.plan(units) -> gpufusion::run(units, plan, work) -> balancer.record(...)
//
// Programs that integrate the SDK: GPU Fusion's own GPUFusionProbe and GPUFusionSplitDemo. No other
// program — games and browsers included — uses it unless its developers add it.
//
// What the SDK does NOT do: it cannot make one rendering workload run on two GPUs by itself, it does
// not change programs that do not include it, and it cannot share resources between adapters of
// different vendors without a copy through system memory. The program divides its own work into
// independent units; the scheduler decides which device computes which units, and measures.
#pragma once

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace gpufusion {

inline constexpr int kSdkVersion = 2;           // this header set
inline constexpr int kMaxEnvironmentVersion = 1; // newest GPUFUSION_ENV_VERSION this SDK understands

enum class Mode { NotLaunchedByGpuFusion, Automatic, Integrated, Discrete, Both };

namespace detail {
inline std::wstring env(const wchar_t* name)
{
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(name, buffer, DWORD(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : std::wstring();
}
inline std::wstring luidText(const LUID& l)
{
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%08lX_%08lX", static_cast<unsigned long>(l.HighPart), static_cast<unsigned long>(l.LowPart));
    return buf;
}
} // namespace detail

// The format version of GPU Fusion's variables: 0 when not launched by GPU Fusion, 1 for GPU Fusion
// versions before the variable existed (same format).
inline int environmentVersion()
{
    const std::wstring v = detail::env(L"GPUFUSION_ENV_VERSION");
    if (!v.empty())
        return std::wcstol(v.c_str(), nullptr, 10);
    return detail::env(L"GPUFUSION_MODE").empty() ? 0 : 1;
}
// False when a newer GPU Fusion set variables in a format this SDK does not know: then ignore them.
inline bool environmentSupported() { return environmentVersion() <= kMaxEnvironmentVersion; }

// The mode GPU Fusion launched this program with.
inline Mode mode()
{
    if (!environmentSupported())
        return Mode::NotLaunchedByGpuFusion;
    const std::wstring m = detail::env(L"GPUFUSION_MODE");
    if (m == L"automatic") return Mode::Automatic;
    if (m == L"integrated") return Mode::Integrated;
    if (m == L"discrete") return Mode::Discrete;
    if (m == L"both") return Mode::Both;
    return Mode::NotLaunchedByGpuFusion;
}

// The requested adapter LUIDs ("HHHHHHHH_LLLLLLLL"), in order of use. Empty: no specific request.
inline std::vector<std::wstring> requestedLuids()
{
    std::vector<std::wstring> out;
    if (!environmentSupported())
        return out;
    const std::wstring all = detail::env(L"GPUFUSION_ADAPTERS");
    size_t start = 0;
    while (start < all.size()) {
        const size_t comma = all.find(L',', start);
        const std::wstring item = all.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!item.empty())
            out.push_back(item);
        if (comma == std::wstring::npos)
            break;
        start = comma + 1;
    }
    return out;
}

// The adapters to use, in order: those GPU Fusion requested that exist on this PC, otherwise the
// default adapter (the first one Windows enumerates). Never empty if the system has an adapter.
inline std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> adapters(IDXGIFactory1* factory)
{
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> all, out;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
        all.push_back(a);
    for (const std::wstring& wanted : requestedLuids()) {
        for (const auto& candidate : all) {
            DXGI_ADAPTER_DESC1 d{};
            candidate->GetDesc1(&d);
            if (_wcsicmp(detail::luidText(d.AdapterLuid).c_str(), wanted.c_str()) == 0)
                out.push_back(candidate);
        }
    }
    if (out.empty() && !all.empty())
        out.push_back(all.front());
    return out;
}

// Divides `total` items into consecutive ranges proportional to `weights` (first, count). Every item
// is assigned exactly once; zero weights get empty ranges.
inline std::vector<std::pair<uint32_t, uint32_t>> split(uint32_t total, const std::vector<double>& weights)
{
    std::vector<std::pair<uint32_t, uint32_t>> out(weights.size(), { 0u, 0u });
    double sum = 0;
    for (double w : weights)
        sum += w > 0 ? w : 0;
    if (weights.empty() || sum <= 0) {
        if (!out.empty())
            out[0] = { 0u, total };
        return out;
    }
    size_t last = 0;
    for (size_t i = 0; i < weights.size(); ++i)
        if (weights[i] > 0)
            last = i;
    double cumulative = 0;
    uint32_t start = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] <= 0) {
            out[i] = { start, 0u };
            continue;
        }
        cumulative += weights[i];
        const uint32_t end = i == last ? total : static_cast<uint32_t>(double(total) * cumulative / sum + 0.5);
        out[i] = { start, end > start ? end - start : 0u };
        start = end > start ? end : start;
    }
    return out;
}

} // namespace gpufusion
