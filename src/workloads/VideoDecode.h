#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace gf::work {

// H.264 decoding with Media Foundation, either on one specific GPU (DXVA through a Direct3D 11
// device created on that adapter) or in software on the CPU. Used to test whether two GPUs decoding
// one stream each beat one GPU decoding both.

// Media Foundation must be started once per process before anything here is used; this does so on
// first call (and leaves it running). COM must be initialised on the calling thread (see ComScope).
bool startMediaFoundation(QString* error);

// Initialises COM (multithreaded) for the current thread for its lifetime, if not already done.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
private:
    bool m_uninit = false;
};

// A synthetic test clip, generated once with Media Foundation's software H.264 encoder (no
// download, no hardware encoder needed). Content is deterministic: moving patterns and a square.
struct TestClipSpec {
    std::uint32_t width = 1280, height = 720; // multiples of 16: no cropping, simple plane layout
    std::uint32_t frames = 300, fps = 30;
    std::uint32_t bitrate = 4'000'000;
};
QString testClipFileName(const TestClipSpec& spec);
// Returns the clip's path inside `dir`, creating it first if needed (`progress(percent)` while
// encoding). The file is written under a temporary name and renamed when complete.
QString ensureTestClip(const QString& dir, const TestClipSpec& spec, QString* error, const std::function<void(int)>& progress = {});

class VideoDecoder {
public:
    ~VideoDecoder();
    // `luid` = adapter to decode on; nullopt = software decoding (no Direct3D device at all).
    static std::unique_ptr<VideoDecoder> open(const QString& path, std::optional<quint64> luid, QString* error);

    QString adapterName() const;

    struct Result {
        bool ok = false;
        QString error;
        int frames = 0;
        double wallMs = 0;           // first request -> last frame decoded (GPU work finished)
        bool gpuOutput = false;      // frames arrived as textures on this decoder's GPU (DXVA)
        std::vector<std::uint64_t> frameHashes; // hash of each frame's visible NV12 pixels
    };
    // Decodes the whole file from the start as fast as possible. `hashFrames` also reads every frame
    // back and hashes it (much slower: for correctness checks, never for timing).
    Result decodeAll(bool hashFrames);

private:
    VideoDecoder();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace gf::work
