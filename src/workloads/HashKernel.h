#pragma once

// The reference workload shared by the CPU benchmarks, the GPU pixel shader (shaders/hash_ps.hlsl)
// and correctness checks: per pixel, an integer hash iterated N times. Integer arithmetic wraps the
// same way on CPU and GPU, so GPU output must match the CPU bit for bit — any difference is a real
// error, not rounding. Keep this in sync with hash_ps.hlsl.

#include <cstdint>
#include <vector>

namespace gf::work {

inline std::uint32_t hashInit(std::uint32_t x, std::uint32_t y, std::uint32_t seed)
{
    std::uint32_t s = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
    return s ? s : 0x9E3779B9u;
}

inline std::uint32_t hashStep(std::uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s * 0x2545F491u;
}

inline std::uint32_t hashPixel(std::uint32_t x, std::uint32_t y, std::uint32_t seed, std::uint32_t iterations)
{
    std::uint32_t s = hashInit(x, y, seed);
    for (std::uint32_t i = 0; i < iterations; ++i)
        s = hashStep(s);
    return s;
}

struct HashParams {
    std::uint32_t width = 1024;
    std::uint32_t height = 1024;
    std::uint32_t iterations = 512;
    std::uint32_t seed = 12345;
    std::uint32_t bandRows = 128; // GPU: rows per draw (keeps each draw far below the driver timeout)
};

// Rows [firstRow, firstRow + rows) on `threads` CPU threads (0 = all logical CPUs).
// Output is row-major, width * rows values.
std::vector<std::uint32_t> hashRowsCpu(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, unsigned threads);
// Same, writing width * rows values to `out`.
void hashRowsCpuInto(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, unsigned threads, std::uint32_t* out);

// Index of the first mismatch between two equally sized buffers, or -1 if identical.
long long firstMismatch(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b);

} // namespace gf::work
