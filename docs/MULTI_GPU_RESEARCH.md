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

## Generic job dispatcher: batches of independent jobs (1.2, 2026-10-25)

The Workload Lab results above are for GPU Fusion's own workloads. Version 1.2 adds a **generic**
dispatcher (capability class C, `src/dispatch/`): a batch of independent jobs, one per input file, run
on the chosen GPUs through a shared queue. A job is any command whose program accepts a GPU on its
command line (template placeholders `{dxgi_index}` `{luid}` `{vendor_id}` `{vendor_hex}` `{gpu_name}`
`{input}` `{output}` `{stem}` `{job}`; `GPUFUSION_ADAPTERS` is set per job for SDK programs), or the
built-in Media Foundation H.264 decoder. Available on the Workload Lab page ("Your own jobs on several
GPUs") and as `--dispatch`.

**How a comparison is judged** (`--dispatch-compare n`, or "Compare runs" in the panel):

- n runs of the batch on each GPU alone, then on **the fastest GPU alone running as many jobs at once
  as the multi-GPU run does** (control), then on all chosen GPUs.
- The control matters. In the first FFmpeg measurement, all GPUs looked 1.55× faster than the best
  GPU running one job at a time. Against the control the gain was 1.14×. Most of the apparent gain
  came from running two processes at once (CPU work, I/O), not from the second GPU.
- Every job must succeed, and each job's output in the multi-GPU run must be identical to the same
  job's output on the same GPU alone (file SHA-1, or the frame count for built-in decoding).
- "Faster" / "Slower" only with the benchmark significance rule (≥ 2 runs each).
- Stragglers are avoided: a GPU does not take another job when a faster GPU would finish all remaining
  jobs before it finished that one (times measured on each GPU alone, then in the run itself).

**Measured on the reference PC** (6 copies of the 720p test clip, 3 runs per configuration; two
sessions on the same day):

| Job | GT 730 alone | HD 2000 alone | HD 2000, 2 at a time (control) | Both GPUs | Verdict |
|---|---|---|---|---|---|
| FFmpeg d3d11va decode → framemd5 | 11.5–12.0 s | 9.5–10.9 s | 6.6–6.7 s | 5.9 s / 6.6 s | 1.14× faster / no significant difference |
| Built-in MF decode | 10.3–10.4 s | 2.4 s | 2.3 s | 2.6 s / 2.1 s | no significant difference / 1.10× faster |

FFmpeg's frame hashes were identical on both GPUs. **Conclusion:** for these decode batches the
second GPU adds at most about 10–15 %, and the result is not stable from one session to the next. On
this PC the HD 2000 is the faster decoder and the GT 730 mainly helps with the last few jobs. With 4
short clips, both GPUs were measurably *slower* than the HD 2000 running two jobs at once
(`tst_jobpanel_ui`). The dispatcher is a measurement tool that reports these results as they
are. It does not promise a speed-up.

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
- A smarter dynamic scheduler for the Lab's compute split (e.g. guided/decreasing item sizes). The
  static split by measured speed already reaches 95–100 % of the ideal. (The 1.2 job dispatcher does
  keep slow workers off the last jobs.)
- Hardware video *encoding* (not available on either GPU), graphics rendering split (AFR/SFR) — no
  shared surfaces between these GPUs, so every frame would cross system memory.
