#include "workloads/HashKernel.h"

#include <algorithm>
#include <thread>

namespace gf::work {

void hashRowsCpuInto(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, unsigned threads, std::uint32_t* out)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned>(threads, std::max(1u, rows));
    auto work = [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t r = begin; r < end; ++r) {
            std::uint32_t* row = out + std::size_t(r) * p.width;
            for (std::uint32_t x = 0; x < p.width; ++x)
                row[x] = hashPixel(x, firstRow + r, p.seed, p.iterations);
        }
    };
    if (threads == 1) {
        work(0, rows);
        return;
    }
    std::vector<std::thread> pool;
    const std::uint32_t chunk = (rows + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        const std::uint32_t begin = t * chunk, end = std::min(rows, begin + chunk);
        if (begin < end)
            pool.emplace_back(work, begin, end);
    }
    for (std::thread& th : pool)
        th.join();
}

std::vector<std::uint32_t> hashRowsCpu(const HashParams& p, std::uint32_t firstRow, std::uint32_t rows, unsigned threads)
{
    std::vector<std::uint32_t> out(std::size_t(p.width) * rows);
    hashRowsCpuInto(p, firstRow, rows, threads, out.data());
    return out;
}

long long firstMismatch(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
{
    if (a.size() != b.size())
        return 0;
    const auto it = std::mismatch(a.begin(), a.end(), b.begin());
    return it.first == a.end() ? -1 : (it.first - a.begin());
}

} // namespace gf::work
