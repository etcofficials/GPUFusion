// GPU Fusion SDK — row kernels: run one pixel-shader kernel over an image (or any 2-D grid) on several
// Direct3D 11 devices at once, rows divided by the adaptive scheduler (scheduler.h). Header-only,
// C++17, Windows; MIT License.
//
// A row kernel is an SM 4.0 pixel shader (feature level 10_0+, so it runs on GPUs without compute
// shaders, such as the Intel HD Graphics 2000) that computes output rows from input rows:
//     Texture2D<uint4> src : register(t0);   // input rows [rows.y, rows.y + height of t0)
//     cbuffer Rows : register(b0) { uint4 rows; uint4 params[4]; };
//         rows = { first output row of this batch, first input row in t0, image width, image height }
//     uint4 main(float4 pos : SV_Position) : SV_Target   // output pixel (pos.x, rows.x + pos.y)
// Pixels are 4 × 8-bit (DXGI_FORMAT_R8G8B8A8_UINT) in and out. `halo` input rows above and below a
// batch are uploaded with it (a blur of radius r needs halo = r). Integer arithmetic makes the
// result exact, so it can be checked against a CPU implementation (blurReference below).
//
// Per batch, each device: uploads its input rows, draws its output rows, copies them back to system
// memory. Devices on different adapters share nothing, so every byte goes through system memory;
// the time of each step is measured with GPU timestamps (no extra synchronisation), and the wait for
// the GPU sleeps on a DXGI event instead of spinning a CPU core.
//
//     std::vector<gpufusion::RowExecutor> ex(devices.size());
//     for (size_t i = 0; i < devices.size(); ++i) ex[i].init(devices[i], ps, psSize, /*halo*/ r, &error);
//     gpufusion::Balancer balancer(ex.size(), gpufusion::Policy::Conservative, 8);
//     gpufusion::RowJob job{ in, w * 4, out, w * 4, w, h, { r } };
//     gpufusion::RowRun run = gpufusion::runRows(ex, balancer, job);   // plan, run, learn
#pragma once

#include "d3d11.h"
#include "detail/fullscreen_vs.h"
#include "scheduler.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gpufusion {

inline constexpr int kRowsVersion = 1;

// One device's resources for one row kernel. Not thread-safe: one thread uses it at a time (run()
// guarantees that for the scheduler's batches).
class RowExecutor {
public:
    RowExecutor() = default;
    RowExecutor(const RowExecutor&) = delete;
    RowExecutor& operator=(const RowExecutor&) = delete;
    RowExecutor(RowExecutor&& o) noexcept { swap(o); }
    RowExecutor& operator=(RowExecutor&& o) noexcept
    {
        swap(o);
        return *this;
    }
    ~RowExecutor()
    {
        if (m_event)
            CloseHandle(m_event);
    }

    bool init(const Device& d, const void* pixelShader, size_t pixelShaderSize, uint32_t halo, std::string* error)
    {
        m_device = d.device;
        m_context = d.context;
        m_name = d.caps.name;
        m_halo = halo;
        HRESULT h = m_device->CreateVertexShader(detail::kFullscreenVs, sizeof(detail::kFullscreenVs), nullptr, &m_vs);
        if (SUCCEEDED(h))
            h = m_device->CreatePixelShader(pixelShader, pixelShaderSize, nullptr, &m_ps);
        if (SUCCEEDED(h)) {
            D3D11_BUFFER_DESC bd{ 80, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            h = m_device->CreateBuffer(&bd, nullptr, &m_constants);
        }
        if (FAILED(h)) {
            if (error)
                *error = "could not create the kernel's shaders on " + narrow(m_name) + " (0x" + hex(h) + ")";
            return false;
        }
        if (SUCCEEDED(m_device.As(&m_dxgi)))
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        bool ok = SUCCEEDED(m_device->CreateQuery(&q, &m_disjoint));
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& t : m_stamps)
            ok = ok && SUCCEEDED(m_device->CreateQuery(&q, &t));
        if (!ok)
            m_disjoint.Reset();
        return true;
    }
    bool ready() const { return m_ps != nullptr; }
    const std::wstring& name() const { return m_name; }

    struct Timing {
        double uploadMs = -1, computeMs = -1, readbackMs = -1; // GPU timestamps; -1 = not available
        double mapMs = 0;                                      // CPU: mapping and copying rows out
        double transferMs() const { return std::max(0.0, uploadMs) + std::max(0.0, readbackMs) + mapMs; }
    };

    // Output rows [first, first + count) of a width × height image. `input` points to row 0 of the
    // whole image (inPitch bytes per row), `output` to row 0 of the whole output image.
    bool run(const uint8_t* input, size_t inPitch, uint8_t* output, size_t outPitch, uint32_t width, uint32_t height, const uint32_t params[16],
             uint32_t first, uint32_t count, Timing* timing = nullptr, std::string* error = nullptr)
    {
        if (!ready() || count == 0 || first + count > height)
            return fail(error, "invalid batch");
        const uint32_t in0 = first > m_halo ? first - m_halo : 0;
        const uint32_t in1 = std::min(height, first + count + m_halo);
        if (!ensure(width, in1 - in0, count, error))
            return false;
        const bool stamps = m_disjoint != nullptr;
        if (stamps) {
            m_context->Begin(m_disjoint.Get());
            m_context->End(m_stamps[0].Get());
        }
        const D3D11_BOX inBox{ 0, 0, 0, width, in1 - in0, 1 };
        m_context->UpdateSubresource(m_input.Get(), 0, &inBox, input + size_t(in0) * inPitch, UINT(inPitch), 0);
        if (stamps)
            m_context->End(m_stamps[1].Get());
        uint32_t cb[20] = { first, in0, width, height };
        std::memcpy(cb + 4, params, 16 * sizeof(uint32_t));
        m_context->UpdateSubresource(m_constants.Get(), 0, nullptr, cb, 0, 0);
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = m_srv.Get();
        m_context->PSSetShaderResources(0, 1, &srv);
        ID3D11Buffer* b = m_constants.Get();
        m_context->PSSetConstantBuffers(0, 1, &b);
        for (uint32_t y = 0; y < count; y += kBandRows) { // short draws: far below the driver's timeout
            const D3D11_VIEWPORT vp{ 0.0f, float(y), float(width), float(std::min(kBandRows, count - y)), 0.0f, 1.0f };
            m_context->RSSetViewports(1, &vp);
            m_context->Draw(3, 0);
        }
        if (stamps)
            m_context->End(m_stamps[2].Get());
        const D3D11_BOX outBox{ 0, 0, 0, width, count, 1 };
        m_context->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, m_target.Get(), 0, &outBox);
        if (stamps) {
            m_context->End(m_stamps[3].Get());
            m_context->End(m_disjoint.Get());
        }
        m_context->Flush();
        if (m_event && SUCCEEDED(m_dxgi->EnqueueSetEvent(m_event)))
            WaitForSingleObject(m_event, 30000);
        const auto t0 = std::chrono::steady_clock::now();
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT h = m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(h))
            return fail(error, "reading the result back failed on " + narrow(m_name) + " (0x" + hex(h) + ")");
        for (uint32_t y = 0; y < count; ++y)
            std::memcpy(output + size_t(first + y) * outPitch, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(width) * 4);
        m_context->Unmap(m_staging.Get(), 0);
        if (timing) {
            timing->mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (stamps)
                readStamps(timing);
        }
        return true;
    }

private:
    static constexpr uint32_t kBandRows = 64;

    static std::string hex(HRESULT h)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08lX", static_cast<unsigned long>(h));
        return buf;
    }
    static std::string narrow(const std::wstring& w)
    {
        std::string s;
        for (wchar_t c : w)
            s += c < 128 ? char(c) : '?';
        return s;
    }
    static bool fail(std::string* error, const std::string& text)
    {
        if (error)
            *error = text;
        return false;
    }

    void readStamps(Timing* t)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        UINT64 s[4] = {};
        auto get = [&](ID3D11Query* q, void* out, UINT size) {
            for (int i = 0; i < 1000; ++i) { // the GPU has finished: the data is (almost) always ready
                const HRESULT h = m_context->GetData(q, out, size, 0);
                if (h == S_OK)
                    return true;
                if (FAILED(h))
                    return false;
                SwitchToThread();
            }
            return false;
        };
        if (!get(m_disjoint.Get(), &d, sizeof d) || d.Disjoint || d.Frequency == 0)
            return;
        for (int i = 0; i < 4; ++i)
            if (!get(m_stamps[i].Get(), &s[i], sizeof(UINT64)))
                return;
        const double f = 1000.0 / double(d.Frequency);
        if (s[1] >= s[0] && s[2] >= s[1] && s[3] >= s[2]) {
            t->uploadMs = double(s[1] - s[0]) * f;
            t->computeMs = double(s[2] - s[1]) * f;
            t->readbackMs = double(s[3] - s[2]) * f;
        }
    }

    // Textures grow to the largest batch and are reused.
    bool ensure(uint32_t width, uint32_t inRows, uint32_t outRows, std::string* error)
    {
        HRESULT h = S_OK;
        if (!m_input || width != m_width || inRows > m_inRows) {
            m_input.Reset();
            m_srv.Reset();
            m_inRows = std::max(inRows, width == m_width ? m_inRows : 0u);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = m_inRows;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            h = m_device->CreateTexture2D(&td, nullptr, &m_input);
            if (SUCCEEDED(h))
                h = m_device->CreateShaderResourceView(m_input.Get(), nullptr, &m_srv);
        }
        if (SUCCEEDED(h) && (!m_target || width != m_width || outRows > m_outRows)) {
            m_target.Reset();
            m_rtv.Reset();
            m_staging.Reset();
            m_outRows = std::max(outRows, width == m_width ? m_outRows : 0u);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = m_outRows;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            h = m_device->CreateTexture2D(&td, nullptr, &m_target);
            if (SUCCEEDED(h))
                h = m_device->CreateRenderTargetView(m_target.Get(), nullptr, &m_rtv);
            if (SUCCEEDED(h)) {
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                h = m_device->CreateTexture2D(&td, nullptr, &m_staging);
            }
        }
        if (FAILED(h)) {
            m_input.Reset();
            m_target.Reset();
            return fail(error, "could not create " + std::to_string(width) + "-pixel-wide textures on " + narrow(m_name) + " (0x" + hex(h) + ")");
        }
        m_width = width;
        return true;
    }

    void swap(RowExecutor& o) noexcept
    {
        std::swap(m_device, o.m_device);
        std::swap(m_context, o.m_context);
        std::swap(m_dxgi, o.m_dxgi);
        std::swap(m_vs, o.m_vs);
        std::swap(m_ps, o.m_ps);
        std::swap(m_constants, o.m_constants);
        std::swap(m_input, o.m_input);
        std::swap(m_srv, o.m_srv);
        std::swap(m_target, o.m_target);
        std::swap(m_rtv, o.m_rtv);
        std::swap(m_staging, o.m_staging);
        std::swap(m_disjoint, o.m_disjoint);
        std::swap(m_stamps, o.m_stamps);
        std::swap(m_event, o.m_event);
        std::swap(m_name, o.m_name);
        std::swap(m_halo, o.m_halo);
        std::swap(m_width, o.m_width);
        std::swap(m_inRows, o.m_inRows);
        std::swap(m_outRows, o.m_outRows);
    }

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIDevice2> m_dxgi;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_input, m_target, m_staging;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Query> m_disjoint;
    std::array<Microsoft::WRL::ComPtr<ID3D11Query>, 4> m_stamps;
    HANDLE m_event = nullptr;
    std::wstring m_name;
    uint32_t m_halo = 0, m_width = 0, m_inRows = 0, m_outRows = 0;
};

// One image to process with a row kernel.
struct RowJob {
    const uint8_t* input = nullptr;
    size_t inPitch = 0;
    uint8_t* output = nullptr;
    size_t outPitch = 0;
    uint32_t width = 0, height = 0;
    uint32_t params[16] = {};
};

struct RowRun {
    Plan plan;
    RunReport report;
    std::vector<RowExecutor::Timing> timing; // per device: summed over its batches
    std::string error;                       // first error a device reported, if any
};

// Plans with the Balancer, runs the batches on the executors (index = device, 0 = primary), and lets
// the Balancer learn from the result. `executors.size()` must equal `balancer.devices()`.
inline RowRun runRows(std::vector<RowExecutor>& executors, Balancer& balancer, const RowJob& job)
{
    RowRun r;
    r.timing.resize(executors.size());
    for (auto& t : r.timing)
        t = RowExecutor::Timing{ 0, 0, 0, 0 };
    std::vector<std::string> errors(executors.size());
    r.plan = balancer.plan(job.height);
    r.report = run(job.height, r.plan, [&](Batch& b) {
        RowExecutor::Timing t;
        const bool ok = executors[b.device].run(job.input, job.inPitch, job.output, job.outPitch, job.width, job.height, job.params, b.first, b.count, &t,
                                                errors[b.device].empty() ? &errors[b.device] : nullptr);
        b.transferMs = t.transferMs();
        RowExecutor::Timing& sum = r.timing[b.device]; // only this device's thread writes it
        sum.uploadMs += std::max(0.0, t.uploadMs);
        sum.computeMs += std::max(0.0, t.computeMs);
        sum.readbackMs += std::max(0.0, t.readbackMs);
        sum.mapMs += t.mapMs;
        return ok;
    }, balancer.unitsPerMs());
    balancer.record(r.plan, r.report);
    for (const std::string& e : errors)
        if (!e.empty() && r.error.empty())
            r.error = e;
    return r;
}

// CPU reference of the SDK's sample kernel (sdk/shaders/blur_ps.hlsl): tent blur of radius r (0..15),
// exact integer arithmetic, clamped edges. Writes rows [first, first + count).
inline void blurReference(const uint8_t* in, size_t inPitch, uint8_t* out, size_t outPitch, uint32_t width, uint32_t height, uint32_t radius,
                          uint32_t first = 0, uint32_t count = 0xFFFFFFFFu)
{
    const int r = int(std::min(radius, 15u));
    const uint32_t last = uint32_t(std::min<uint64_t>(height, uint64_t(first) + count));
    for (uint32_t y = first; y < last; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t sum[4] = {}, total = 0;
            for (int dy = -r; dy <= r; ++dy) {
                const int yy = std::clamp(int(y) + dy, 0, int(height) - 1);
                const uint32_t wy = uint32_t(r + 1 - std::abs(dy));
                const uint8_t* row = in + size_t(yy) * inPitch;
                for (int dx = -r; dx <= r; ++dx) {
                    const uint32_t w = wy * uint32_t(r + 1 - std::abs(dx));
                    const uint8_t* p = row + size_t(std::clamp(int(x) + dx, 0, int(width) - 1)) * 4;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c] * w;
                    total += w;
                }
            }
            uint8_t* o = out + size_t(y) * outPitch + size_t(x) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = uint8_t((sum[c] + total / 2) / total);
        }
    }
}

} // namespace gpufusion
