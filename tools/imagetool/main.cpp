// GPUFusionImage — a real workload for both GPUs: filters photographs with the GPU Fusion SDK's row
// kernels (public headers only, no Qt) and measures it against the best single GPU.
//
// The filter is the SDK's sample kernel, a tent blur of radius R in exact integer arithmetic. Every
// output image is compared bit for bit with the CPU reference (gpufusion::blurReference).
//
// Two ways to use both GPUs, measured separately:
//   split  one image at a time; its rows are divided between the GPUs by the adaptive scheduler
//          (capability: independent work units of one task on different GPUs)
//   batch  whole images as independent jobs; each GPU takes whole images from a shared queue, the
//          slower GPU only when it would finish before the faster one finishes the rest
//          (capability: independent jobs running concurrently on both GPUs)
// Each is compared with the same work on the fastest GPU alone, using GPU Fusion's significance rule
// (difference > twice the combined standard error and > 1 %); otherwise "no significant difference".
//
//   GPUFusionImage.exe [--in <file|folder> ...] [--out <folder>] [--radius R] [--runs N]
//                      [--mode split|batch|both] [--policy conservative|balanced|performance]
// Without --in: the photographs in C:\Windows\Web\Wallpaper (read only). --out writes the filtered
// images as PNG. Prints JSON. Exit code: 0 = every output correct, 1 = a wrong output or a failure,
// 2 = bad arguments.
#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion.h>
#include <gpufusion/rows.h>
#include <gpufusion/scheduler.h>

#include "shaders/blur_ps.h"

#include <windows.h>
#include <psapi.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Image {
    std::wstring path;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba, reference, out;
};

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            o += c;
    }
    return o;
}

std::string num(double v, int d = 3)
{
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

bool load(IWICImagingFactory* wic, const std::wstring& path, Image* img, std::string* error)
{
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    HRESULT h = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(h))
        h = dec->GetFrame(0, &frame);
    if (SUCCEEDED(h))
        h = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(h))
        h = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(h))
        h = conv->GetSize(&img->width, &img->height);
    if (SUCCEEDED(h) && (img->width == 0 || img->height == 0 || img->width > 16384 || img->height > 16384))
        h = E_INVALIDARG;
    if (SUCCEEDED(h)) {
        img->rgba.resize(size_t(img->width) * img->height * 4);
        h = conv->CopyPixels(nullptr, img->width * 4, UINT(img->rgba.size()), img->rgba.data());
    }
    if (FAILED(h)) {
        char b[64];
        std::snprintf(b, sizeof b, "cannot decode (0x%08lX)", static_cast<unsigned long>(h));
        *error = b;
        return false;
    }
    img->path = path;
    img->out.assign(img->rgba.size(), 0);
    return true;
}

bool savePng(IWICImagingFactory* wic, const std::wstring& path, const Image& img)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT h = wic->CreateStream(&stream);
    if (SUCCEEDED(h))
        h = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(h))
        h = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(h))
        h = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(h))
        h = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(h))
        h = frame->Initialize(nullptr);
    if (SUCCEEDED(h))
        h = frame->SetSize(img.width, img.height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(h))
        h = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(h))
        h = frame->WritePixels(img.height, img.width * 4, UINT(img.out.size()), const_cast<BYTE*>(img.out.data()));
    if (SUCCEEDED(h))
        h = frame->Commit();
    if (SUCCEEDED(h))
        h = enc->Commit();
    return SUCCEEDED(h);
}

void collect(const std::wstring& arg, std::vector<std::wstring>* files)
{
    const DWORD attr = GetFileAttributesW(arg.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        files->push_back(arg);
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((arg + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring full = arg + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            collect(full, files);
        else {
            std::wstring lower = name;
            for (wchar_t& c : lower)
                c = wchar_t(towlower(c));
            for (const wchar_t* ext : { L".jpg", L".jpeg", L".png", L".bmp" })
                if (lower.size() > wcslen(ext) && lower.compare(lower.size() - wcslen(ext), wcslen(ext), ext) == 0)
                    files->push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// CPU reference, rows divided over the CPU's cores (only for checking).
void reference(Image& img, uint32_t radius)
{
    img.reference.assign(img.rgba.size(), 0);
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    const uint32_t per = (img.height + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            const uint32_t first = t * per;
            if (first < img.height)
                gpufusion::blurReference(img.rgba.data(), img.width * 4, img.reference.data(), img.width * 4, img.width, img.height, radius, first, per);
        });
    for (auto& t : pool)
        t.join();
}

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
    return "{\"n\":" + std::to_string(s.n) + ",\"mean\":" + num(s.mean) + ",\"sd\":" + num(s.sd) + ",\"median\":" + num(s.median) + ",\"min\":"
         + num(s.min) + ",\"max\":" + num(s.max) + "}";
}
// GPU Fusion's rule: faster/slower only beyond twice the combined standard error and 1 %.
std::string verdict(const Stats& a, const Stats& base)
{
    if (a.n < 2 || base.n < 2)
        return "not enough runs";
    const double se = std::sqrt(a.sd * a.sd / a.n + base.sd * base.sd / base.n);
    const double diff = base.mean - a.mean;
    if (std::abs(diff) > 2 * se && std::abs(diff) > 0.01 * base.mean)
        return diff > 0 ? "faster" : "slower";
    return "no significant difference";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    uint32_t radius = 6;
    int runs = 8;
    std::wstring mode = L"both";
    gpufusion::Policy policy = gpufusion::Policy::Conservative;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--in" && more) inputs.push_back(argv[++i]);
        else if (a == L"--out" && more) outDir = argv[++i];
        else if (a == L"--radius" && more) radius = std::min(15u, uint32_t(_wtoi(argv[++i])));
        else if (a == L"--runs" && more) runs = std::clamp(_wtoi(argv[++i]), 2, 200);
        else if (a == L"--mode" && more) mode = argv[++i];
        else if (a == L"--policy" && more) {
            std::string p;
            for (wchar_t c : std::wstring(argv[++i]))
                p += char(c);
            if (!gpufusion::parsePolicy(p, &policy))
                return 2;
        } else {
            std::fprintf(stderr, "usage: GPUFusionImage [--in <file|folder> ...] [--out <folder>] [--radius R] [--runs N] [--mode split|batch|both] "
                                 "[--policy conservative|balanced|performance]\n");
            return 2;
        }
    }
    if (mode != L"split" && mode != L"batch" && mode != L"both")
        return 2;
    if (inputs.empty())
        inputs.push_back(L"C:\\Windows\\Web\\Wallpaper");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        std::puts("{\"tool\":\"GPUFusionImage\",\"error\":\"Windows Imaging Component unavailable\"}");
        return 1;
    }
    std::vector<std::wstring> files;
    for (const std::wstring& in : inputs)
        collect(in, &files);
    std::sort(files.begin(), files.end());
    std::vector<Image> images;
    std::string json = "{\"tool\":\"GPUFusionImage\",\"sdkVersion\":" + std::to_string(gpufusion::kSdkVersion) + ",\"rowsVersion\":"
                     + std::to_string(gpufusion::kRowsVersion) + ",\"kernel\":\"tent blur\",\"radius\":" + std::to_string(radius)
                     + ",\"policy\":\"" + gpufusion::policyName(policy) + "\",\"images\":[";
    bool firstImage = true;
    for (const std::wstring& f : files) {
        Image img;
        std::string error;
        const bool ok = load(wic.Get(), f, &img, &error);
        json += std::string(firstImage ? "" : ",") + "{\"file\":\"" + utf8(f) + "\"" + (ok ? ",\"width\":" + std::to_string(img.width) + ",\"height\":"
                + std::to_string(img.height) : ",\"skipped\":\"" + error + "\"") + "}";
        firstImage = false;
        if (ok)
            images.push_back(std::move(img));
    }
    json += "]";
    if (images.empty()) {
        std::puts((json + ",\"error\":\"no image could be read\"}").c_str());
        return 1;
    }
    const double refStart = nowMs();
    for (Image& img : images)
        reference(img, radius);
    json += ",\"cpuReferenceMs\":" + num(nowMs() - refStart, 1);

    // Every hardware adapter (or the ones GPU Fusion offered), one row executor each.
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    std::vector<ComPtr<IDXGIAdapter1>> offered = gpufusion::adapters(factory.Get());
    if (gpufusion::requestedLuids().empty()) {
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
    std::vector<gpufusion::RowExecutor> all;
    std::vector<std::wstring> names;
    json += ",\"adapters\":[";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        gpufusion::RowExecutor ex;
        std::string error;
        const bool ok = devices[i].caps.pixelShaders && ex.init(devices[i], g_blur_ps, sizeof(g_blur_ps), radius, &error);
        json += std::string(i ? "," : "") + "{\"name\":\"" + utf8(devices[i].caps.name) + "\",\"featureLevel\":\""
              + utf8(gpufusion::featureLevelText(devices[i].caps.featureLevel)) + "\",\"usable\":" + (ok ? "true" : "false")
              + (ok ? "" : ",\"skipped\":\"" + error + "\"") + "}";
        if (ok) {
            all.push_back(std::move(ex));
            names.push_back(devices[i].caps.name);
        }
    }
    json += "]";
    if (all.empty()) {
        std::puts((json + ",\"error\":\"no adapter can run the kernel\"}").c_str());
        return 1;
    }

    bool allCorrect = true;
    auto check = [&](const Image& img) { return img.out == img.reference; };
    auto clearOut = [&](Image& img) { std::fill(img.out.begin(), img.out.end(), uint8_t(0xCD)); };
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());

    // ---- each GPU alone, one image after another (the baseline for both modes) ----
    // Executors are used through a one-device Balancer so the code path equals the split's.
    std::vector<Stats> single(all.size());
    json += ",\"single\":[";
    for (std::size_t d = 0; d < all.size(); ++d) {
        std::vector<gpufusion::RowExecutor> one;
        one.push_back(std::move(all[d]));
        gpufusion::Balancer b(1, policy, 8);
        auto pass = [&](bool measure) {
            const double t0 = nowMs();
            for (Image& img : images) {
                clearOut(img);
                gpufusion::RowJob job{ img.rgba.data(), size_t(img.width) * 4, img.out.data(), size_t(img.width) * 4, img.width, img.height, { radius } };
                const gpufusion::RowRun r = gpufusion::runRows(one, b, job);
                if (measure)
                    allCorrect = allCorrect && r.report.ok && check(img);
            }
            return nowMs() - t0;
        };
        pass(false); // warm-up: wakes the GPU from its idle clocks, creates the textures
        std::vector<double> t;
        const double cpu0 = processCpuMs(), w0 = nowMs();
        for (int k = 0; k < runs; ++k)
            t.push_back(pass(true));
        const double cpuPct = 100.0 * (processCpuMs() - cpu0) / std::max(1.0, nowMs() - w0) / cores;
        single[d] = stats(t);
        all[d] = std::move(one[0]);
        json += std::string(d ? "," : "") + "{\"adapter\":\"" + utf8(names[d]) + "\",\"batchMs\":" + statsJson(single[d])
              + ",\"processCpuPercentOfAllCores\":" + num(cpuPct, 1) + "}";
    }
    json += "]";
    std::vector<std::size_t> order(all.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return single[a].median < single[b].median; });
    std::vector<gpufusion::RowExecutor> ordered;
    for (std::size_t i : order)
        ordered.push_back(std::move(all[i]));
    const Stats best = single[order[0]];
    json += ",\"fastestAlone\":\"" + utf8(names[order[0]]) + "\"";

    // ---- split: each image's rows divided by the adaptive scheduler ----
    if ((mode == L"split" || mode == L"both") && ordered.size() > 1) {
        gpufusion::Balancer b(ordered.size(), policy, 8);
        std::vector<double> rowsPct(ordered.size(), 0);
        double overhead = 0, transfer = 0;
        int splits = 0, batches = 0;
        auto pass = [&](bool measure) {
            const double t0 = nowMs();
            for (Image& img : images) {
                clearOut(img);
                gpufusion::RowJob job{ img.rgba.data(), size_t(img.width) * 4, img.out.data(), size_t(img.width) * 4, img.width, img.height, { radius } };
                const gpufusion::RowRun r = gpufusion::runRows(ordered, b, job);
                if (!measure)
                    continue;
                allCorrect = allCorrect && r.report.ok && check(img);
                ++batches;
                splits += r.plan.multi() ? 1 : 0;
                overhead += r.report.overheadMs();
                for (std::size_t d = 0; d < ordered.size(); ++d) {
                    rowsPct[d] += 100.0 * r.report.devices[d].units / img.height;
                    transfer += r.report.devices[d].transferMs;
                }
            }
            return nowMs() - t0;
        };
        const double warmUntil = nowMs() + 1500; // the Balancer measures the primary alone first, then learns
        while (nowMs() < warmUntil)
            pass(false);
        std::vector<double> t;
        const double cpu0 = processCpuMs(), w0 = nowMs();
        for (int k = 0; k < runs; ++k)
            t.push_back(pass(true));
        const double cpuPct = 100.0 * (processCpuMs() - cpu0) / std::max(1.0, nowMs() - w0) / cores;
        const Stats s = stats(t);
        json += ",\"split\":{\"batchMs\":" + statsJson(s) + ",\"imagesSplit\":" + std::to_string(splits) + ",\"imagesTotal\":" + std::to_string(batches)
              + ",\"rowsPercent\":[";
        for (std::size_t d = 0; d < ordered.size(); ++d)
            json += std::string(d ? "," : "") + "{\"adapter\":\"" + utf8(names[order[d]]) + "\",\"percent\":" + num(batches ? rowsPct[d] / batches : 0, 2) + "}";
        json += "],\"overheadMsPerImage\":" + num(batches ? overhead / batches : 0) + ",\"transferMsPerImage\":" + num(batches ? transfer / batches : 0)
              + ",\"processCpuPercentOfAllCores\":" + num(cpuPct, 1) + ",\"helper\":\"" + b.helper(1).note + "\",\"balancerState\":\"" + b.save()
              + "\",\"verdict\":\"" + verdict(s, best) + "\",\"speedup\":" + num(s.mean > 0 ? best.mean / s.mean : 0) + "}";
    }

    // ---- batch: whole images as independent jobs on both GPUs at once ----
    if ((mode == L"batch" || mode == L"both") && ordered.size() > 1 && images.size() > 1) {
        // Each image is one unit; a device runs whole images (all rows) on its own thread. Rates for
        // the "would it delay the end" rule come from the single-GPU baseline (images per ms).
        std::vector<double> rate(ordered.size());
        for (std::size_t d = 0; d < ordered.size(); ++d)
            rate[d] = double(images.size()) / std::max(1e-3, single[order[d]].median);
        gpufusion::Plan plan;
        plan.enabled.assign(ordered.size(), 1);
        plan.batch.assign(ordered.size(), 1u);
        plan.share.assign(ordered.size(), 0.0);
        double rateSum = 0;
        for (double r : rate)
            rateSum += r;
        for (std::size_t d = 1; d < ordered.size(); ++d)
            plan.share[d] = std::min(0.5, 1.1 * rate[d] / rateSum);
        plan.share[0] = 1.0;
        std::vector<gpufusion::Balancer> one;
        for (std::size_t d = 0; d < ordered.size(); ++d)
            one.emplace_back(1, policy, 8);
        std::vector<double> imagesPct(ordered.size(), 0);
        auto pass = [&](bool measure) {
            for (Image& img : images)
                clearOut(img);
            const double t0 = nowMs();
            const gpufusion::RunReport rep = gpufusion::run(uint32_t(images.size()), plan, [&](gpufusion::Batch& bt) {
                bool ok = true;
                std::vector<gpufusion::RowExecutor> mine; // run() gives each device to one thread
                mine.push_back(std::move(ordered[bt.device]));
                for (uint32_t i = bt.first; i < bt.first + bt.count && ok; ++i) {
                    Image& img = images[i];
                    gpufusion::RowJob job{ img.rgba.data(), size_t(img.width) * 4, img.out.data(), size_t(img.width) * 4, img.width, img.height, { radius } };
                    ok = gpufusion::runRows(mine, one[bt.device], job).report.ok;
                }
                ordered[bt.device] = std::move(mine[0]);
                return ok;
            }, rate);
            const double ms = nowMs() - t0;
            if (measure) {
                for (const Image& img : images)
                    allCorrect = allCorrect && check(img);
                allCorrect = allCorrect && rep.ok;
                for (std::size_t d = 0; d < ordered.size(); ++d)
                    imagesPct[d] += 100.0 * rep.devices[d].units / images.size();
            }
            return ms;
        };
        pass(false);
        std::vector<double> t;
        const double cpu0 = processCpuMs(), w0 = nowMs();
        for (int k = 0; k < runs; ++k)
            t.push_back(pass(true));
        const double cpuPct = 100.0 * (processCpuMs() - cpu0) / std::max(1.0, nowMs() - w0) / cores;
        const Stats s = stats(t);
        json += ",\"batch\":{\"batchMs\":" + statsJson(s) + ",\"imagesPercent\":[";
        for (std::size_t d = 0; d < ordered.size(); ++d)
            json += std::string(d ? "," : "") + "{\"adapter\":\"" + utf8(names[order[d]]) + "\",\"percent\":" + num(imagesPct[d] / runs, 1) + "}";
        json += "],\"processCpuPercentOfAllCores\":" + num(cpuPct, 1) + ",\"verdict\":\"" + verdict(s, best) + "\",\"speedup\":"
              + num(s.mean > 0 ? best.mean / s.mean : 0) + "}";
    }

    if (!outDir.empty()) {
        CreateDirectoryW(outDir.c_str(), nullptr);
        int written = 0;
        for (Image& img : images) {
            img.out = img.reference; // the checked result (every GPU output equalled it)
            const std::size_t slash = img.path.find_last_of(L"\\/");
            std::wstring name = img.path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            name = name.substr(0, name.find_last_of(L'.')) + L"-blur.png";
            written += savePng(wic.Get(), outDir + L"\\" + name, img) ? 1 : 0;
        }
        json += ",\"written\":" + std::to_string(written);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    json += ",\"peakWorkingSetMB\":" + num(double(pmc.PeakWorkingSetSize) / 1048576.0, 1) + ",\"allOutputCorrect\":" + (allCorrect ? "true" : "false")
          + "}";
    std::puts(json.c_str());
    return allCorrect ? 0 : 1;
}
