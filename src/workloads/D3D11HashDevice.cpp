#include "workloads/D3D11HashDevice.h"

#include "hardware/AdapterModel.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>

// Generated at build time by fxc (see src/CMakeLists.txt).
#include "shaders/fullscreen_vs.h"
#include "shaders/hash_ps.h"

using Microsoft::WRL::ComPtr;

namespace gf::work {

namespace {

constexpr double kTimeoutMs = 10'000; // far above any band; protects against a hung GPU

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

QString hr(HRESULT h) { return hw::describeHresult(qint32(h)); }

} // namespace

struct D3D11HashDevice::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    std::uint32_t targetWidth = 0, targetRows = 0;

    // Waits until the GPU has finished everything submitted so far.
    QString waitIdle()
    {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> q;
        if (HRESULT h = device->CreateQuery(&qd, &q); FAILED(h))
            return QStringLiteral("Could not create a GPU query: %1").arg(hr(h));
        context->End(q.Get());
        context->Flush();
        const auto start = std::chrono::steady_clock::now();
        BOOL done = FALSE;
        for (;;) {
            const HRESULT h = context->GetData(q.Get(), &done, sizeof(done), 0);
            if (h == S_OK && done)
                return {};
            if (FAILED(h))
                return QStringLiteral("The GPU stopped responding: %1").arg(hr(device->GetDeviceRemovedReason() != S_OK ? device->GetDeviceRemovedReason() : h));
            if (msSince(start) > kTimeoutMs)
                return QStringLiteral("The GPU did not finish within %1 s.").arg(kTimeoutMs / 1000);
            SwitchToThread();
        }
    }

    bool ensureTarget(std::uint32_t width, std::uint32_t rows, QString* error)
    {
        if (target && targetWidth == width && targetRows == rows)
            return true;
        target.Reset();
        staging.Reset();
        rtv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = rows;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT h = device->CreateTexture2D(&td, nullptr, &target);
        if (SUCCEEDED(h))
            h = device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        if (SUCCEEDED(h)) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            h = device->CreateTexture2D(&td, nullptr, &staging);
        }
        if (FAILED(h)) {
            *error = QStringLiteral("Could not create a %1 x %2 integer render target: %3").arg(width).arg(rows).arg(hr(h));
            target.Reset();
            return false;
        }
        targetWidth = width;
        targetRows = rows;
        return true;
    }

    void bindPipeline(const HashParams& p, std::uint32_t firstRow)
    {
        const std::uint32_t cb[4] = { p.iterations, p.seed, firstRow, 0 };
        context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
        ID3D11RenderTargetView* view = rtv.Get();
        context->OMSetRenderTargets(1, &view, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* cbuffer = constants.Get();
        context->PSSetConstantBuffers(0, 1, &cbuffer);
    }

    void drawBands(const HashParams& p, std::uint32_t rows)
    {
        const std::uint32_t band = std::max(1u, p.bandRows);
        for (std::uint32_t y = 0; y < rows; y += band) {
            const D3D11_VIEWPORT vp{ 0.0f, float(y), float(p.width), float(std::min(band, rows - y)), 0.0f, 1.0f };
            context->RSSetViewports(1, &vp);
            context->Draw(3, 0);
            context->Flush(); // submit each band separately: no single GPU packet runs long
        }
    }

    // Copies the render target to CPU memory (tightly packed rows).
    QString readInto(std::uint32_t width, std::uint32_t rows, std::uint32_t* dest)
    {
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT h = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(h))
            return QStringLiteral("Reading the result back failed: %1").arg(hr(h));
        for (std::uint32_t y = 0; y < rows; ++y)
            std::memcpy(dest + std::size_t(y) * width, static_cast<const std::uint8_t*>(m.pData) + std::size_t(y) * m.RowPitch,
                        width * sizeof(std::uint32_t));
        context->Unmap(staging.Get(), 0);
        return {};
    }
};

D3D11HashDevice::D3D11HashDevice() : d(std::make_unique<Impl>()) {}
D3D11HashDevice::~D3D11HashDevice() = default;

std::unique_ptr<D3D11HashDevice> D3D11HashDevice::create(quint64 luid, QString* error)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        *error = QStringLiteral("DXGI is not available.");
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter, found;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (((quint64(quint32(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart) == luid)
            found = adapter;
    }
    if (!found) {
        *error = QStringLiteral("The adapter is no longer present (LUID %1).").arg(hw::formatLuid(luid));
        return nullptr;
    }
    std::unique_ptr<D3D11HashDevice> self(new D3D11HashDevice);
    DXGI_ADAPTER_DESC1 desc{};
    found->GetDesc1(&desc);
    self->m_adapterName = QString::fromWCharArray(desc.Description);

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT h = D3D11CreateDevice(found.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                  &self->d->device, &got, &self->d->context);
    if (FAILED(h)) {
        *error = QStringLiteral("Direct3D 11 device creation failed: %1").arg(hr(h));
        return nullptr;
    }
    self->m_featureLevel = QStringLiteral("%1_%2").arg((got >> 12) & 0xF).arg((got >> 8) & 0xF);
    Impl& i = *self->d;
    h = i.device->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &i.vs);
    if (SUCCEEDED(h))
        h = i.device->CreatePixelShader(g_hash_ps, sizeof(g_hash_ps), nullptr, &i.ps);
    if (SUCCEEDED(h)) {
        D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        h = i.device->CreateBuffer(&bd, nullptr, &i.constants);
    }
    if (FAILED(h)) {
        *error = QStringLiteral("Could not set up the workload shaders: %1").arg(hr(h));
        return nullptr;
    }
    return self;
}

D3D11HashDevice::RunResult D3D11HashDevice::run(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, bool readback)
{
    RunResult r;
    Impl& i = *d;
    if (rows == 0 || p.width == 0) {
        r.error = QStringLiteral("Empty region.");
        return r;
    }
    if (!i.ensureTarget(p.width, rows, &r.error))
        return r;

    i.bindPipeline(p, firstRow);

    // Make sure setup work is finished before timing starts.
    if (QString e = i.waitIdle(); !e.isEmpty()) {
        r.error = e;
        return r;
    }

    D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, tsDesc{ D3D11_QUERY_TIMESTAMP, 0 };
    ComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
    const bool timestamps = SUCCEEDED(i.device->CreateQuery(&disjointDesc, &disjoint)) && SUCCEEDED(i.device->CreateQuery(&tsDesc, &tsStart)) &&
                            SUCCEEDED(i.device->CreateQuery(&tsDesc, &tsEnd));

    const auto start = std::chrono::steady_clock::now();
    if (timestamps) {
        i.context->Begin(disjoint.Get());
        i.context->End(tsStart.Get());
    }
    i.drawBands(p, rows);
    if (timestamps) {
        i.context->End(tsEnd.Get());
        i.context->End(disjoint.Get());
    }
    if (QString e = i.waitIdle(); !e.isEmpty()) {
        r.error = e;
        return r;
    }
    r.wallMs = msSince(start);

    if (timestamps) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (i.context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && dj.Frequency &&
            i.context->GetData(tsStart.Get(), &t0, sizeof(t0), 0) == S_OK && i.context->GetData(tsEnd.Get(), &t1, sizeof(t1), 0) == S_OK && t1 >= t0)
            r.gpuMs = double(t1 - t0) * 1000.0 / double(dj.Frequency);
    }

    if (readback) {
        const auto rb = std::chrono::steady_clock::now();
        r.output.resize(std::size_t(p.width) * rows);
        if (QString e = i.readInto(p.width, rows, r.output.data()); !e.isEmpty()) {
            r.error = e;
            r.output.clear();
            return r;
        }
        r.readbackMs = msSince(rb);
    }
    r.ok = true;
    return r;
}

QString D3D11HashDevice::computeInto(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, std::uint32_t* dest,
                                     double* computeMs, double* readbackMs)
{
    Impl& i = *d;
    if (rows == 0 || p.width == 0)
        return {};
    QString error;
    if (!i.ensureTarget(p.width, rows, &error))
        return error;
    const auto start = std::chrono::steady_clock::now();
    i.bindPipeline(p, firstRow);
    i.drawBands(p, rows);
    if (QString e = i.waitIdle(); !e.isEmpty())
        return e;
    if (computeMs)
        *computeMs = msSince(start);
    const auto rb = std::chrono::steady_clock::now();
    if (QString e = i.readInto(p.width, rows, dest); !e.isEmpty())
        return e;
    if (readbackMs)
        *readbackMs = msSince(rb);
    return {};
}

D3D11HashDevice::TransferResult D3D11HashDevice::transfer(std::uint64_t bytes)
{
    TransferResult r;
    Impl& i = *d;
    bytes = (bytes + 15) & ~std::uint64_t(15);
    r.bytes = bytes;
    std::vector<std::uint32_t> data(bytes / 4);
    for (std::size_t k = 0; k < data.size(); ++k)
        data[k] = hashStep(std::uint32_t(k) + 1);

    D3D11_BUFFER_DESC bd{ UINT(bytes), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0 };
    ComPtr<ID3D11Buffer> gpu, staging;
    HRESULT h = i.device->CreateBuffer(&bd, nullptr, &gpu);
    if (SUCCEEDED(h)) {
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        h = i.device->CreateBuffer(&bd, nullptr, &staging);
    }
    if (FAILED(h)) {
        r.error = QStringLiteral("Could not allocate %1 on the GPU: %2").arg(hw::formatBytes(bytes), hr(h));
        return r;
    }
    if (QString e = i.waitIdle(); !e.isEmpty()) {
        r.error = e;
        return r;
    }
    auto t = std::chrono::steady_clock::now();
    i.context->UpdateSubresource(gpu.Get(), 0, nullptr, data.data(), 0, 0);
    if (QString e = i.waitIdle(); !e.isEmpty()) {
        r.error = e;
        return r;
    }
    r.uploadMs = msSince(t);

    t = std::chrono::steady_clock::now();
    i.context->CopyResource(staging.Get(), gpu.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    h = i.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(h)) {
        r.error = QStringLiteral("Reading back failed: %1").arg(hr(h));
        return r;
    }
    r.dataIntact = std::memcmp(m.pData, data.data(), bytes) == 0;
    i.context->Unmap(staging.Get(), 0);
    r.readbackMs = msSince(t);
    r.ok = true;
    return r;
}

} // namespace gf::work
