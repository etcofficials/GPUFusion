#include "workloads/SplitPlan.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gf::work {

std::vector<RowRange> staticRanges(std::uint32_t height, const std::vector<double>& shares, std::uint32_t align)
{
    std::vector<RowRange> out(shares.size());
    if (shares.empty() || height == 0)
        return out;
    align = std::max(1u, align);
    double total = 0;
    for (double s : shares)
        total += std::max(0.0, s);
    if (total <= 0) { // nothing sensible to split by: everything to the first worker
        out[0] = { 0, height };
        return out;
    }
    double cumulative = 0;
    std::uint32_t start = 0;
    // The last worker with a non-zero share takes the remainder, so every row is assigned.
    std::size_t last = 0;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        if (shares[i] > 0)
            last = i;
    }
    for (std::size_t i = 0; i < shares.size(); ++i) {
        const double s = std::max(0.0, shares[i]);
        if (s <= 0) {
            out[i] = { start, 0 };
            continue;
        }
        cumulative += s;
        std::uint32_t end = height;
        if (i != last) {
            const double exact = height * (cumulative / total);
            end = std::uint32_t(std::llround(exact / align)) * align;
            end = std::clamp(end, start, height);
        }
        out[i] = { start, end - start };
        start = end;
    }
    return out;
}

std::vector<double> balancedShares(const std::vector<double>& fullJobMs)
{
    std::vector<double> rates;
    rates.reserve(fullJobMs.size());
    for (double t : fullJobMs)
        rates.push_back(t > 0 ? 1.0 / t : 0.0);
    const double sum = std::accumulate(rates.begin(), rates.end(), 0.0);
    for (double& r : rates)
        r = sum > 0 ? r / sum : 0.0;
    return rates;
}

double idealSplitMs(const std::vector<double>& fullJobMs)
{
    double rate = 0;
    for (double t : fullJobMs) {
        if (t > 0)
            rate += 1.0 / t;
    }
    return rate > 0 ? 1.0 / rate : 0.0;
}

} // namespace gf::work
