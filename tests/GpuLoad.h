#pragma once

// Test helper: keeps one adapter busy from the calling process by clearing a large render target
// in a loop on a background thread (a known, simple GPU load). Works on feature level 10_0+.

#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <thread>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

class GpuLoad {
public:
    explicit GpuLoad(quint64 luid)
    {
        m_thread = std::thread([this, luid] { run(luid); });
        while (!m_ready && !m_failed)
            QThread::msleep(10);
    }
    ~GpuLoad()
    {
        m_stop = true;
        m_thread.join();
    }
    GpuLoad(const GpuLoad&) = delete;
    GpuLoad& operator=(const GpuLoad&) = delete;

    bool ok() const { return m_ready && !m_failed; }

private:
    void run(quint64 luid)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter, found;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (((quint64(quint32(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart) == luid)
                found = adapter;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        if (!found || FAILED(D3D11CreateDevice(found.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                               &device, nullptr, &context))) {
            m_failed = true;
            return;
        }
        D3D11_TEXTURE2D_DESC td{ 4096, 4096, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) || FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
            m_failed = true;
            return;
        }
        m_ready = true;
        float color[4] = { 0, 0, 0, 1 };
        while (!m_stop) {
            for (int i = 0; i < 20; ++i) {
                color[0] = float(i) / 20.0f;
                context->ClearRenderTargetView(rtv.Get(), color);
            }
            context->Flush();
        }
    }

    std::thread m_thread;
    std::atomic<bool> m_stop{ false }, m_ready{ false }, m_failed{ false };
};
