// GPUFusionSDK.dll: the C interface (gpufusion_c.h) over the header-only SDK. MIT License.
#define GPUFUSION_SDK_BUILD
#include <gpufusion/gpufusion_c.h>

#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion.h>
#include <gpufusion/rows.h>
#include <gpufusion/scheduler.h>

#include "shaders/blur_ps.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct gf_context {
    std::vector<gpufusion::Device> devices;
    gpufusion::Policy policy = gpufusion::Policy::Conservative;
    struct Kernel {
        std::vector<gpufusion::RowExecutor> executors;
        std::unique_ptr<gpufusion::Balancer> balancer;
    };
    std::vector<Kernel> kernels;
    int blur = -1;
    std::string error, learned;
};

namespace {

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void copyText(char* dst, std::size_t size, const std::string& s)
{
    const std::size_t n = std::min(size - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

bool validKernel(gf_context* gf, int k) { return gf && k >= 0 && std::size_t(k) < gf->kernels.size(); }

} // namespace

extern "C" {

int gf_abi_version(void) { return GF_ABI_VERSION; }
int gf_sdk_version(void) { return gpufusion::kSdkVersion; }

gf_context* gf_create(int adapters, int policy)
{
    auto gf = std::make_unique<gf_context>();
    gf->policy = policy == GF_POLICY_BALANCED ? gpufusion::Policy::Balanced
               : policy == GF_POLICY_PERFORMANCE ? gpufusion::Policy::Performance : gpufusion::Policy::Conservative;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> list;
    if (adapters == GF_ADAPTERS_ALL) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                list.push_back(a);
        }
        // The primary (device 0) should be the fastest GPU; without measuring, the one with the most
        // dedicated memory (a discrete GPU before an integrated one).
        std::stable_sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
            DXGI_ADAPTER_DESC1 a{}, b{};
            x->GetDesc1(&a);
            y->GetDesc1(&b);
            return a.DedicatedVideoMemory > b.DedicatedVideoMemory;
        });
    } else {
        list = gpufusion::adapters(factory.Get()); // GPU Fusion's order: the discrete GPU first
    }
    std::wstring errors;
    for (auto& d : gpufusion::createDevices(list, &errors))
        if (d.caps.pixelShaders && gf->devices.size() < GF_MAX_DEVICES)
            gf->devices.push_back(std::move(d));
    if (gf->devices.empty())
        return nullptr;
    return gf.release();
}

void gf_destroy(gf_context* gf) { delete gf; }

const char* gf_last_error(gf_context* gf) { return gf ? gf->error.c_str() : "no context"; }

int gf_adapter_count(gf_context* gf) { return gf ? int(gf->devices.size()) : -1; }

int gf_adapter_info(gf_context* gf, int index, gf_adapter* out)
{
    if (!gf || !out || index < 0 || std::size_t(index) >= gf->devices.size())
        return -1;
    const gpufusion::AdapterCaps& c = gf->devices[std::size_t(index)].caps;
    std::memset(out, 0, sizeof *out);
    copyText(out->name, sizeof out->name, utf8(c.name));
    copyText(out->luid, sizeof out->luid, utf8(c.luid));
    out->vendor_id = c.vendorId;
    out->feature_level = unsigned(c.featureLevel);
    out->compute_shaders = c.computeShaders ? 1 : 0;
    out->dedicated_mb = c.dedicatedMB;
    return 0;
}

int gf_kernel_create(gf_context* gf, const void* bytecode, size_t size, unsigned halo)
{
    if (!gf || !bytecode || size == 0)
        return -1;
    gf_context::Kernel k;
    for (const gpufusion::Device& d : gf->devices) {
        gpufusion::RowExecutor ex;
        if (!ex.init(d, bytecode, size, halo, &gf->error))
            return -1;
        k.executors.push_back(std::move(ex));
    }
    k.balancer = std::make_unique<gpufusion::Balancer>(k.executors.size(), gf->policy, 8);
    gf->kernels.push_back(std::move(k));
    gf->error.clear();
    return int(gf->kernels.size() - 1);
}

int gf_kernel_blur(gf_context* gf)
{
    if (!gf)
        return -1;
    if (gf->blur < 0)
        gf->blur = gf_kernel_create(gf, g_blur_ps, sizeof(g_blur_ps), 15);
    return gf->blur;
}

int gf_run(gf_context* gf, int kernel, const unsigned char* in, size_t in_pitch, unsigned char* out, size_t out_pitch, unsigned width,
           unsigned height, const unsigned params[16], gf_report* report)
{
    if (!validKernel(gf, kernel) || !in || !out || width == 0 || height == 0 || in_pitch < size_t(width) * 4 || out_pitch < size_t(width) * 4) {
        if (gf)
            gf->error = "invalid arguments";
        return -1;
    }
    gf_context::Kernel& k = gf->kernels[std::size_t(kernel)];
    gpufusion::RowJob job;
    job.input = in;
    job.inPitch = in_pitch;
    job.output = out;
    job.outPitch = out_pitch;
    job.width = width;
    job.height = height;
    if (params)
        std::memcpy(job.params, params, sizeof job.params);
    const gpufusion::RowRun r = gpufusion::runRows(k.executors, *k.balancer, job);
    if (report) {
        std::memset(report, 0, sizeof *report);
        report->ok = r.report.ok ? 1 : 0;
        report->multi = r.plan.multi() ? 1 : 0;
        report->wall_ms = r.report.wallMs;
        report->overhead_ms = r.report.overheadMs();
        report->devices = int(std::min<std::size_t>(GF_MAX_DEVICES, r.report.devices.size()));
        for (int d = 0; d < report->devices; ++d) {
            report->rows[d] = r.report.devices[std::size_t(d)].units;
            report->work_ms[d] = r.report.devices[std::size_t(d)].workMs;
            report->transfer_ms[d] = r.report.devices[std::size_t(d)].transferMs;
        }
        copyText(report->plan, sizeof report->plan, r.plan.reason);
    }
    gf->error = r.report.ok ? std::string() : (r.report.error + (r.error.empty() ? std::string() : " (" + r.error + ")"));
    return r.report.ok ? 0 : -2;
}

const char* gf_learned(gf_context* gf, int kernel)
{
    if (!validKernel(gf, kernel))
        return nullptr;
    gf->learned = gf->kernels[std::size_t(kernel)].balancer->save();
    return gf->learned.c_str();
}

int gf_restore(gf_context* gf, int kernel, const char* learned)
{
    if (!validKernel(gf, kernel) || !learned)
        return -1;
    return gf->kernels[std::size_t(kernel)].balancer->restore(learned) ? 0 : -1;
}

void gf_blur_reference(const unsigned char* in, size_t in_pitch, unsigned char* out, size_t out_pitch, unsigned width, unsigned height, unsigned radius)
{
    if (in && out)
        gpufusion::blurReference(in, in_pitch, out, out_pitch, width, height, radius);
}

} // extern "C"
