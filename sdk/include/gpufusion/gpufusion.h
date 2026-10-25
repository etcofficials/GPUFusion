// GPU Fusion SDK — optional integration for programs that want to honour the GPU choice made in
// GPU Fusion, including "Both GPUs". Header-only, C++17, Windows, DXGI; MIT License.
//
// How it works: when GPU Fusion launches a program from its profile it sets
//     GPUFUSION_MODE      automatic | integrated | discrete | both
//     GPUFUSION_ADAPTERS  comma-separated adapter LUIDs "HHHHHHHH_LLLLLLLL", in order of use
//     GPUFUSION_PROFILE   the profile id
// A program that includes this header asks for its adapters and creates its devices on them. A
// program launched any other way simply gets the default adapter — nothing breaks.
//
//     #include <gpufusion/gpufusion.h>
//     Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
//     CreateDXGIFactory1(IID_PPV_ARGS(&factory));
//     auto adapters = gpufusion::adapters(factory.Get());      // at least one entry
//     D3D11CreateDevice(adapters[0].Get(), D3D_DRIVER_TYPE_UNKNOWN, ...);
//     if (gpufusion::mode() == gpufusion::Mode::Both && adapters.size() > 1) {
//         // create a device per adapter and divide independent work, e.g.:
//         auto parts = gpufusion::split(rows, { 5.0, 1.0 });     // weights, e.g. measured speeds
//     }
//
// What the SDK does NOT do: it cannot make one rendering workload run on two GPUs by itself, and it
// cannot share resources between adapters of different vendors without a copy through system memory.
// The program divides its own work; GPU Fusion's Workload Lab shows measured examples.
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

// The mode GPU Fusion launched this program with.
inline Mode mode()
{
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
