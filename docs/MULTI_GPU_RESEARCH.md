# Multi-GPU research

**Status: experiments done (Phase 8, 2026-10-16).** Results below were measured on the reference PC
with the Workload Lab (`GPUFusion.exe --run-lab all`); every number comes from repeated runs after
warm-up, with the output checked for correctness.

## Question

Can the GeForce GT 730 (Fermi, FL 11_0) and Intel HD Graphics 2000 (FL 10_1, WDDM 1.2) do useful
work at the same time, and does that make any task faster than the GT 730 alone?

## Answer

| Workload | Best single device | Both GPUs together | Verdict |
|---|---|---|---|
| Hash image 1024 × 1024 (compute, integer) | GT 730 alone 60.6–61.6 ms | split by measured speed 51.4–53.8 ms | **1.14–1.18× faster** (3 sessions) |
| Hash image 2048 × 2048 | GT 730 alone 240.7 ms | split by measured speed 202.6 ms | **1.19× faster** (100 % of the ideal) |
| Two H.264 720p streams | both on the **HD 2000**: 703–712 ms | one stream per GPU: 1583–1584 ms | **2.2× slower** (3 sessions) |

- **Compute can gain up to about 1.2×, and only when the work is divided in proportion to each GPU's
  speed** (here ≈ 84 % GT 730 / 16 % HD 2000). The measured gain matches the theoretical limit
  `1 / (1/t_GT + 1/t_HD)`: the HD 2000 is 5.1× slower than the GT 730 on this shader, so it can only
  ever take about a sixth of the work. Splitting evenly is 2.5× *slower* (the HD 2000 must do half).
- **For video decoding the "weak" GPU is the strong one.** The HD 2000's fixed-function decoder (Intel
  Quick Sync generation 1) decodes the test video at ~850 frames/s; the GT 730's (NVIDIA PureVideo,
  VP4-era) at ~190 frames/s — slower than decoding in software on the CPU (~465 frames/s). Putting
  one stream on each GPU is therefore slower than putting both on the HD 2000: the GT 730 sets the pace.
- **Adding the CPU did not help** the compute split (1.33–1.35× slower than the GT 730 alone): a CPU
  thread needs ~70 ms for a 32-row item, so the CPU's last item finishes long after the GPUs.

## Constraints established (Phase 0)

| Approach | Result | Reason |
|---|---|---|
| D3D12 explicit multi-adapter, cross-adapter heaps | not possible | Intel driver has no D3D12 (`D3D12CreateDevice` → DXGI_ERROR_UNSUPPORTED) |
| Shared D3D11 resources between the two adapters | not possible | different vendors, WDDM 1.2; data must go through system memory |
| D3D11 compute shaders on both | not possible | HD 2000 has no CS 4.x/5.0 support |
| OpenCL | not available | no `OpenCL.dll` loader; HD 2000 has no GPU OpenCL |
| CUDA | rejected | NVIDIA only; Fermi requires an obsolete CUDA ≤ 8 toolkit |
| Hardware encode on either | not available | no NVENC on Fermi; no Quick Sync MFT/runtime installed |
| **D3D11 pixel-shader work on separate devices, CPU staging transfer** | **feasible — used** | both create D3D11 devices |
| **Parallel H.264 decode via D3D11 Video** | **feasible — used** | both decode in hardware through Media Foundation (verified: frames arrive as textures on that GPU, bit-identical to software decoding) |

## Method

**Split compute** (`work::runSplit`): one thread and one Direct3D 11 device per GPU (and optionally
CPU threads). Each worker renders its rows of the hash image with the SM 4.0 pixel shader, copies them
to CPU memory, and the image is assembled in place; the timed interval runs from a common start (all
threads released together) until the whole image is in CPU memory. Every run's image is compared with
the CPU reference bit for bit. Modes: each worker alone (same code path), even split, split by this
session's measured single-GPU times, shared queue of 32-row items, shared queue including 3 CPU threads.

**Video decoding** (`work::VideoDecoder`): a deterministic 10 s, 1280 × 720, 30 fps, 4 Mbit/s H.264
clip is generated once with Windows' software encoder. Media Foundation's Source Reader decodes it as
fast as possible to NV12, either in software or with a `IMFDXGIDeviceManager` bound to a Direct3D 11
device created on one specific adapter. Before timing, every decoder's frames are hashed and compared
with the software decoder's (H.264 decoding is exact, so they must be identical — they were, on both
GPUs). Modes: one stream per decoder, two streams on the same decoder, one stream per GPU.

**Evidence recorded per mode:** D3DKMT busiest engines of each GPU during the measured runs (e.g. GT
730 "3D 95 %" while computing; the HD 2000's driver does not name its engines: "Engine 0 99 %"), and
NVIDIA performance state at start and end of the measured runs (NVML).

## Pitfall found: GPU power states

The first run showed the balanced split 1.4× *slower* than the GT 730 alone. Per-worker timings showed
the GT 730 needing 83 ms for rows it computes in 46 ms on its own; the HD 2000 was on schedule. A
controlled re-test with continuously busy devices showed no slowdown, and NVML showed why: after the
CPU-only mode the GT 730 had dropped to **P8** (low-power clocks), and one warm-up run was not enough
to bring it back to P0. Every lab mode and every GPU benchmark now warms up for **at least one
second**; the P-state is recorded so this is visible rather than assumed. In the even split the GT 730
legitimately stays in P8 throughout (it is busy only a third of the time, waiting for the HD 2000) —
that is part of that mode's real cost and is reported as such.

## What was not tried

- Splitting a *single* video stream between GPUs (by GOP): decoding is sequential within a stream;
  with the HD 2000 4.5× faster than the GT 730 at decoding, the expected gain is small.
- A smarter dynamic scheduler (e.g. guided/decreasing item sizes, or keeping slow workers off the last
  items). The static split by measured speed already reaches 95–100 % of the ideal.
- Hardware video *encoding* (not available on either GPU), graphics rendering split (AFR/SFR) — no
  shared surfaces between these GPUs, so every frame would cross system memory.
