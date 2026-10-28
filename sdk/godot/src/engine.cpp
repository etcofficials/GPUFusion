// The SDK side of the Godot extension: devices, the row kernel and the adaptive scheduler.
#include "engine.h"

#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion.h>
#include <gpufusion/rows.h>
#include <gpufusion/scheduler.h>

#include "shaders/blur_ps.h"

#include <algorithm>

namespace gfgodot {

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
} // namespace

struct Engine::Impl {
    std::vector<gpufusion::Device> devices;
    std::vector<gpufusion::RowExecutor> executors;
    std::unique_ptr<gpufusion::Balancer> balancer;
    int policy = 0;
};

Engine::Engine() : d(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

bool Engine::configure(int policy, int maxDevices, std::string* error)
{
    d->executors.clear();
    d->devices.clear();
    d->balancer.reset();
    d->policy = policy;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        *error = "DXGI is not available";
        return false;
    }
    // The GPUs GPU Fusion offered when it started Godot (Both GPUs profile), else every hardware GPU
    // with the one that has the most dedicated memory (the discrete GPU) first.
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> list;
    if (!gpufusion::requestedLuids().empty()) {
        list = gpufusion::adapters(factory.Get());
    } else {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                list.push_back(a);
        }
        std::stable_sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
            DXGI_ADAPTER_DESC1 a{}, b{};
            x->GetDesc1(&a);
            y->GetDesc1(&b);
            return a.DedicatedVideoMemory > b.DedicatedVideoMemory;
        });
    }
    std::wstring errors;
    for (auto& dev : gpufusion::createDevices(list, &errors)) {
        if (!dev.caps.pixelShaders || int(d->executors.size()) >= std::max(1, maxDevices))
            continue;
        gpufusion::RowExecutor ex;
        std::string e;
        if (!ex.init(dev, g_blur_ps, sizeof(g_blur_ps), 15, &e))
            continue;
        d->executors.push_back(std::move(ex));
        d->devices.push_back(std::move(dev));
    }
    if (d->executors.empty()) {
        *error = "no GPU can run the kernel" + (errors.empty() ? std::string() : ": " + utf8(errors));
        return false;
    }
    const gpufusion::Policy p = policy == 1 ? gpufusion::Policy::Balanced : policy == 2 ? gpufusion::Policy::Performance : gpufusion::Policy::Conservative;
    d->balancer = std::make_unique<gpufusion::Balancer>(d->executors.size(), p, 8);
    return true;
}

std::vector<std::string> Engine::adapters()
{
    std::vector<std::string> out;
    for (const auto& dev : d->devices)
        out.push_back(utf8(dev.caps.name) + " (feature level " + utf8(gpufusion::featureLevelText(dev.caps.featureLevel)) + ")");
    return out;
}

bool Engine::blur(const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height, uint32_t radius, RunInfo* info)
{
    if (!d->balancer) {
        std::string e;
        if (!configure(d->policy, 8, &e)) {
            info->error = e;
            return false;
        }
    }
    gpufusion::RowJob job;
    job.input = in;
    job.inPitch = size_t(width) * 4;
    job.output = out;
    job.outPitch = size_t(width) * 4;
    job.width = width;
    job.height = height;
    job.params[0] = std::min(radius, 15u);
    const gpufusion::RowRun r = gpufusion::runRows(d->executors, *d->balancer, job);
    info->ok = r.report.ok;
    info->multi = r.plan.multi();
    info->wallMs = r.report.wallMs;
    info->overheadMs = r.report.overheadMs();
    info->plan = r.plan.reason;
    info->error = r.report.ok ? std::string() : r.report.error + (r.error.empty() ? "" : " (" + r.error + ")");
    info->devices.clear();
    info->rows.clear();
    info->workMs.clear();
    info->transferMs.clear();
    for (std::size_t i = 0; i < d->devices.size() && i < r.report.devices.size(); ++i) {
        info->devices.push_back(utf8(d->devices[i].caps.name));
        info->rows.push_back(r.report.devices[i].units);
        info->workMs.push_back(r.report.devices[i].workMs);
        info->transferMs.push_back(r.report.devices[i].transferMs);
    }
    return r.report.ok;
}

void Engine::blurReference(const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height, uint32_t radius)
{
    gpufusion::blurReference(in, size_t(width) * 4, out, size_t(width) * 4, width, height, radius);
}

std::string Engine::learned() const { return d->balancer ? d->balancer->save() : std::string(); }
bool Engine::restore(const std::string& state) { return d->balancer && d->balancer->restore(state); }

} // namespace gfgodot
