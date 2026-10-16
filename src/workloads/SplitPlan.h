#pragma once

// How a row-based workload is divided between workers (GPUs, the CPU) — pure and unit-tested.

#include <cstdint>
#include <vector>

namespace gf::work {

struct RowRange {
    std::uint32_t first = 0, rows = 0;
};

// Divides `height` rows into consecutive ranges proportional to `shares` (normalised; negative
// treated as 0). Boundaries are rounded to multiples of `align` (the last range ends at `height`).
// Every row is assigned exactly once; a worker with a zero share gets an empty range.
std::vector<RowRange> staticRanges(std::uint32_t height, const std::vector<double>& shares, std::uint32_t align);

// Shares that make every worker finish at the same moment if its time is proportional to the rows
// it gets: share_i = (1 / t_i) / sum_j (1 / t_j), where t_i is worker i's time for the whole job.
std::vector<double> balancedShares(const std::vector<double>& fullJobMs);

// The time a perfectly balanced split would take with zero overhead: 1 / sum_i (1 / t_i). This is
// the upper bound on what splitting can achieve; real splits add synchronisation and transfers.
double idealSplitMs(const std::vector<double>& fullJobMs);

} // namespace gf::work
