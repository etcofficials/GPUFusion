// The GPU Fusion SDK engine behind the Godot extension, with plain C++ types only, so that Windows'
// headers (used by the SDK) and Godot's headers never meet in one source file.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gfgodot {

struct RunInfo {
    bool ok = false, multi = false;
    double wallMs = 0, overheadMs = 0;
    std::vector<std::string> devices;      // names, the primary first
    std::vector<uint32_t> rows;            // rows per device
    std::vector<double> workMs, transferMs;
    std::string plan, error;
};

class Engine {
public:
    Engine();
    ~Engine();
    // policy: 0 conservative, 1 balanced, 2 performance; maxDevices: 1 = the primary GPU only.
    bool configure(int policy, int maxDevices, std::string* error);
    std::vector<std::string> adapters();   // "name (feature level)", in use order
    // Tent blur of radius r over an RGBA8 image (width * height * 4 bytes).
    bool blur(const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height, uint32_t radius, RunInfo* info);
    static void blurReference(const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height, uint32_t radius);
    std::string learned() const;
    bool restore(const std::string& state);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace gfgodot
