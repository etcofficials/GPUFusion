#include "workloads/VideoDecode.h"

#include "hardware/AdapterModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace gf::work {

namespace {

QString hr(HRESULT h) { return hw::describeHresult(qint32(h)); }

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::uint64_t fnv1a(std::uint64_t h, const std::uint8_t* p, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// Hash of the visible part of an NV12 image: `visW` x `visH` luma, then `visW` x `visH / 2` chroma.
std::uint64_t hashNv12(const std::uint8_t* base, std::size_t pitch, std::uint32_t planeRows, std::uint32_t visW, std::uint32_t visH)
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::uint32_t y = 0; y < visH; ++y)
        h = fnv1a(h, base + y * pitch, visW);
    const std::uint8_t* chroma = base + std::size_t(planeRows) * pitch;
    for (std::uint32_t y = 0; y < visH / 2; ++y)
        h = fnv1a(h, chroma + y * pitch, visW);
    return h;
}

ComPtr<IDXGIAdapter1> findAdapter(quint64 luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (((quint64(quint32(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart) == luid)
            return adapter;
    }
    return {};
}

// Deterministic synthetic frame: diagonal XOR texture moving each frame, a bright square crossing
// the picture, slowly shifting colours. Enough detail and motion that decoding is real work.
void fillFrame(std::uint8_t* nv12, std::uint32_t w, std::uint32_t h, std::uint32_t t)
{
    const std::uint32_t cx = (t * 7) % w, cy = (t / 30) % 2 ? h / 2 + h / 8 : h / 2 - h / 8;
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = nv12 + std::size_t(y) * w;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t v = std::uint8_t(((x + 3 * t) ^ (y + 2 * t)) & 0xFF);
            if (x + 80 > cx && x < cx + 80 && y + 80 > cy && y < cy + 80)
                v = 235;
            row[x] = std::uint8_t(16 + (v * 219) / 255);
        }
    }
    std::uint8_t* uv = nv12 + std::size_t(w) * h;
    for (std::uint32_t y = 0; y < h / 2; ++y) {
        for (std::uint32_t x = 0; x < w / 2; ++x) {
            uv[std::size_t(y) * w + 2 * x] = std::uint8_t(96 + ((x + t) & 63));
            uv[std::size_t(y) * w + 2 * x + 1] = std::uint8_t(96 + ((y + 2 * t) & 63));
        }
    }
}

} // namespace

bool startMediaFoundation(QString* error)
{
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, [] { result = MFStartup(MF_VERSION, MFSTARTUP_FULL); });
    if (FAILED(result) && error)
        *error = QStringLiteral("Media Foundation is not available (%1). On Windows \"N\" editions it comes with the Media Feature Pack.").arg(hr(result));
    return SUCCEEDED(result);
}

ComScope::ComScope()
{
    const HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_uninit = SUCCEEDED(h); // S_FALSE (already initialised) also needs a matching uninit
}

ComScope::~ComScope()
{
    if (m_uninit)
        CoUninitialize();
}

QString testClipFileName(const TestClipSpec& s)
{
    return QStringLiteral("gpufusion-h264-%1x%2-%3f-%4fps-%5kbps-v1.mp4").arg(s.width).arg(s.height).arg(s.frames).arg(s.fps).arg(s.bitrate / 1000);
}

QString ensureTestClip(const QString& dir, const TestClipSpec& spec, QString* error, const std::function<void(int)>& progress)
{
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(testClipFileName(spec));
    if (QFileInfo(path).size() > 0)
        return path;
    if (!startMediaFoundation(error))
        return {};
    const QString partial = path + QStringLiteral(".partial");
    QFile::remove(partial);

    auto fail = [&](const QString& what, HRESULT h) {
        if (error)
            *error = QStringLiteral("Creating the test video failed (%1): %2").arg(what, hr(h));
        return QString();
    };
    ComPtr<IMFAttributes> attrs;
    HRESULT h = MFCreateAttributes(&attrs, 3);
    if (SUCCEEDED(h)) h = attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(h)) h = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE); // the software encoder, everywhere
    if (SUCCEEDED(h)) h = attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    if (SUCCEEDED(h)) h = MFCreateSinkWriterFromURL(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(partial).utf16()), nullptr, attrs.Get(), &writer);
    if (FAILED(h))
        return fail(QStringLiteral("sink writer"), h);

    ComPtr<IMFMediaType> out, in;
    DWORD stream = 0;
    h = MFCreateMediaType(&out);
    if (SUCCEEDED(h)) h = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(h)) h = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(h)) h = out->SetUINT32(MF_MT_AVG_BITRATE, spec.bitrate);
    if (SUCCEEDED(h)) h = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(h)) h = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    if (SUCCEEDED(h)) h = MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, spec.width, spec.height);
    if (SUCCEEDED(h)) h = MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, spec.fps, 1);
    if (SUCCEEDED(h)) h = MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(h)) h = writer->AddStream(out.Get(), &stream);
    if (SUCCEEDED(h)) h = MFCreateMediaType(&in);
    if (SUCCEEDED(h)) h = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(h)) h = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(h)) h = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(h)) h = MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, spec.width, spec.height);
    if (SUCCEEDED(h)) h = MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, spec.fps, 1);
    if (SUCCEEDED(h)) h = MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(h)) h = writer->SetInputMediaType(stream, in.Get(), nullptr);
    if (SUCCEEDED(h)) h = writer->BeginWriting();
    if (FAILED(h))
        return fail(QStringLiteral("H.264 encoder setup"), h);

    const DWORD frameBytes = spec.width * spec.height * 3 / 2;
    const LONGLONG duration = 10'000'000LL / spec.fps;
    for (std::uint32_t t = 0; t < spec.frames; ++t) {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE* data = nullptr;
        h = MFCreateMemoryBuffer(frameBytes, &buffer);
        if (SUCCEEDED(h)) h = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(h)) {
            fillFrame(data, spec.width, spec.height, t);
            buffer->Unlock();
            h = buffer->SetCurrentLength(frameBytes);
        }
        if (SUCCEEDED(h)) h = MFCreateSample(&sample);
        if (SUCCEEDED(h)) h = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(h)) h = sample->SetSampleTime(LONGLONG(t) * duration);
        if (SUCCEEDED(h)) h = sample->SetSampleDuration(duration);
        if (SUCCEEDED(h)) h = writer->WriteSample(stream, sample.Get());
        if (FAILED(h)) {
            writer.Reset();
            QFile::remove(partial);
            return fail(QStringLiteral("frame %1").arg(t), h);
        }
        if (progress && t % 15 == 0)
            progress(int(100 * t / spec.frames));
    }
    h = writer->Finalize();
    writer.Reset();
    if (FAILED(h)) {
        QFile::remove(partial);
        return fail(QStringLiteral("finalize"), h);
    }
    if (!QFile::rename(partial, path)) {
        if (error)
            *error = QStringLiteral("Could not rename the finished test video to %1.").arg(QDir::toNativeSeparators(path));
        return {};
    }
    return path;
}

struct VideoDecoder::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFSourceReader> reader;
    ComPtr<ID3D11Texture2D> staging;
    QString adapterName;
    UINT32 frameWidth = 0, frameHeight = 0; // buffer layout (software path)
    UINT32 visibleWidth = 0, visibleHeight = 0;

    HRESULT readOutputType()
    {
        ComPtr<IMFMediaType> type;
        HRESULT h = reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
        if (FAILED(h))
            return h;
        h = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight);
        visibleWidth = frameWidth;
        visibleHeight = frameHeight;
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            visibleWidth = UINT32(area.Area.cx);
            visibleHeight = UINT32(area.Area.cy);
        }
        return h;
    }

    // Waits until the GPU has finished all submitted work (decoding included).
    void waitGpu()
    {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> q;
        if (FAILED(device->CreateQuery(&qd, &q)))
            return;
        context->End(q.Get());
        context->Flush();
        BOOL done = FALSE;
        const auto start = std::chrono::steady_clock::now();
        while (context->GetData(q.Get(), &done, sizeof(done), 0) == S_FALSE && msSince(start) < 10'000)
            SwitchToThread();
    }

    // Returns false with *error when the frame cannot be read.
    bool hashSample(IMFSample* sample, std::uint64_t* out, QString* error)
    {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT h = sample->GetBufferByIndex(0, &buffer);
        if (FAILED(h)) {
            *error = hr(h);
            return false;
        }
        ComPtr<IMFDXGIBuffer> dx;
        if (SUCCEEDED(buffer.As(&dx))) {
            ComPtr<ID3D11Texture2D> texture;
            UINT sub = 0;
            h = dx->GetResource(IID_PPV_ARGS(&texture));
            if (SUCCEEDED(h)) h = dx->GetSubresourceIndex(&sub);
            if (FAILED(h)) {
                *error = hr(h);
                return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC sd{};
            if (staging)
                staging->GetDesc(&sd);
            if (!staging || sd.Width != desc.Width || sd.Height != desc.Height || sd.Format != desc.Format) {
                staging.Reset();
                sd = desc;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                if (FAILED(h = device->CreateTexture2D(&sd, nullptr, &staging))) {
                    *error = QStringLiteral("staging texture: %1").arg(hr(h));
                    return false;
                }
            }
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), sub, nullptr);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(h = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                *error = QStringLiteral("map: %1").arg(hr(h));
                return false;
            }
            *out = hashNv12(static_cast<const std::uint8_t*>(m.pData), m.RowPitch, desc.Height, visibleWidth, visibleHeight);
            context->Unmap(staging.Get(), 0);
            return true;
        }
        ComPtr<IMF2DBuffer> b2;
        if (SUCCEEDED(buffer.As(&b2))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (FAILED(h = b2->Lock2D(&scan0, &pitch)) || pitch <= 0) {
                *error = QStringLiteral("Lock2D: %1").arg(hr(h));
                return false;
            }
            *out = hashNv12(scan0, std::size_t(pitch), frameHeight, visibleWidth, visibleHeight);
            b2->Unlock2D();
            return true;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(h = buffer->Lock(&data, nullptr, &length))) {
            *error = QStringLiteral("Lock: %1").arg(hr(h));
            return false;
        }
        *out = hashNv12(data, frameWidth, frameHeight, visibleWidth, visibleHeight);
        buffer->Unlock();
        return true;
    }
};

VideoDecoder::VideoDecoder() : d(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder()
{
    d->reader.Reset(); // release the decoder before its device
}

QString VideoDecoder::adapterName() const { return d->adapterName; }

std::unique_ptr<VideoDecoder> VideoDecoder::open(const QString& path, std::optional<quint64> luid, QString* error)
{
    if (!startMediaFoundation(error))
        return nullptr;
    std::unique_ptr<VideoDecoder> self(new VideoDecoder);
    Impl& i = *self->d;
    HRESULT h = S_OK;
    if (luid) {
        ComPtr<IDXGIAdapter1> adapter = findAdapter(*luid);
        if (!adapter) {
            *error = QStringLiteral("The adapter is no longer present (LUID %1).").arg(hw::formatLuid(*luid));
            return nullptr;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        i.adapterName = QString::fromWCharArray(desc.Description);
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
        h = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, UINT(std::size(levels)),
                              D3D11_SDK_VERSION, &i.device, nullptr, &i.context);
        if (FAILED(h)) {
            *error = QStringLiteral("Could not create a Direct3D 11 video device on %1: %2").arg(i.adapterName, hr(h));
            return nullptr;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(i.device.As(&mt)))
            mt->SetMultithreadProtected(TRUE); // the decoder uses the device from its own threads
        UINT token = 0;
        h = MFCreateDXGIDeviceManager(&token, &i.manager);
        if (SUCCEEDED(h))
            h = i.manager->ResetDevice(i.device.Get(), token);
        if (FAILED(h)) {
            *error = QStringLiteral("Media Foundation could not use the Direct3D device: %1").arg(hr(h));
            return nullptr;
        }
    } else {
        i.adapterName = QStringLiteral("CPU (software decoder)");
    }

    ComPtr<IMFAttributes> attrs;
    h = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(h) && i.manager)
        h = attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, i.manager.Get());
    // No hidden format converters: the decoder's own NV12 output is what is timed.
    if (SUCCEEDED(h))
        h = attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    if (SUCCEEDED(h))
        h = MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), attrs.Get(), &i.reader);
    if (FAILED(h)) {
        *error = QStringLiteral("Could not open %1: %2").arg(QDir::toNativeSeparators(path), hr(h));
        return nullptr;
    }
    ComPtr<IMFMediaType> type;
    h = i.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(h)) h = i.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (SUCCEEDED(h)) h = MFCreateMediaType(&type);
    if (SUCCEEDED(h)) h = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(h)) h = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(h)) h = i.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    if (SUCCEEDED(h)) h = i.readOutputType();
    if (FAILED(h)) {
        *error = QStringLiteral("No H.264 decoder producing NV12 is available: %1").arg(hr(h));
        return nullptr;
    }
    return self;
}

VideoDecoder::Result VideoDecoder::decodeAll(bool hashFrames)
{
    Result r;
    Impl& i = *d;
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = 0;
    HRESULT h = i.reader->SetCurrentPosition(GUID_NULL, pos);
    if (FAILED(h)) {
        r.error = QStringLiteral("Could not rewind: %1").arg(hr(h));
        return r;
    }
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        h = i.reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(h)) {
            r.error = QStringLiteral("Decoding failed after %1 frames: %2").arg(r.frames).arg(hr(h));
            return r;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            i.readOutputType();
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
            break;
        if (!sample)
            continue;
        if (r.frames == 0 && i.device) {
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dx;
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11Device> owner;
            if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&dx)) && SUCCEEDED(dx->GetResource(IID_PPV_ARGS(&texture)))) {
                texture->GetDevice(&owner);
                r.gpuOutput = owner.Get() == i.device.Get();
            }
        }
        ++r.frames;
        if (hashFrames) {
            std::uint64_t value = 0;
            QString e;
            if (!i.hashSample(sample.Get(), &value, &e)) {
                r.error = QStringLiteral("Reading frame %1 failed: %2").arg(r.frames).arg(e);
                return r;
            }
            r.frameHashes.push_back(value);
        }
    }
    if (i.device)
        i.waitGpu();
    r.wallMs = msSince(start);
    r.ok = r.frames > 0;
    if (!r.ok)
        r.error = QStringLiteral("The decoder produced no frames.");
    return r;
}

} // namespace gf::work
