# Compatibility

Facts below were measured on the reference machine during the Phase 0 audit (2026-10-16) with
small read-only probe programs; raw output is in `data/audit/`. They describe this machine and
driver set, not every GT 730 or HD Graphics system.

## Adapters

| | NVIDIA GeForce GT 730 | Intel HD Graphics 2000 | Microsoft Basic Render Driver |
|---|---|---|---|
| PCI ID | 10DE:0F02 (GF108, **Fermi**) | 8086:0102 (Sandy Bridge GT1) | 1414:008C (software) |
| Driver | 23.21.13.9135 (R391.35, 2018) | 9.17.10.4459 (2016) | inbox |
| WDDM | 2.3 | **1.2** | 2.7 |
| Device Manager | OK | OK | — |
| Memory (dedicated / shared) | 4031 MB / 6086 MB | 32 MB / 1632 MB | 0 / 6086 MB |
| D3D11 feature level | 11_0 | 10_1 | 11_1 (WARP) |
| Compute shaders | yes | **no** | yes |
| D3D12 | yes (FL 11_0) | **no** | yes |
| D3D11 video decode | H.264, MPEG-2, VC-1 profiles | H.264, MPEG-2 profiles | none |
| Hardware encode | no (no NVENC on Fermi) | no (no Quick Sync MFT/runtime installed) | — |
| Display attached | yes | no | — |

## Monitoring sources

| Source | GT 730 | HD 2000 |
|---|---|---|
| PDH "GPU Engine" / memory counters (Task Manager's source) | yes | **no** (needs WDDM 2.0) |
| D3DKMTQueryStatistics (engine running time, segment usage) | yes | **yes** — verified 93.9 % busy under load |
| DXGI QueryVideoMemoryInfo | yes | no |
| NVML | temperature, fan %, memory, P-state; *not* utilization, power, clocks | — |
| Temperature / clocks / power | see NVML row | not exposed |

## Not available on this machine

OpenCL (no `OpenCL.dll` loader; no Intel ICD), CUDA for both GPUs, D3D12 multi-adapter,
cross-vendor shared D3D11 resources, NVENC, Quick Sync encode.

## Application

| Check | Phase | Result |
|---|---|---|
| Builds with MSVC 19.44 / Qt 6.8.3 / CMake 4.4.3 / Ninja 1.13.2 | 1 | yes |
| Launches with PATH reduced to `C:\Windows\System32;C:\Windows` | 1 | yes (exit 0) |
| Windows 10 22H2 (19045) | 1 | verified |
| GT 730 detected, paired with Device Manager by PCI location (bus 1, dev 0, fn 0), classified discrete, D3D11 FL 11_0 + D3D12 FL 11_0 devices created | 2 | verified (2026-10-16) |
| HD Graphics 2000 detected (DXGI #1), paired by PCI location (bus 0, dev 2, fn 0), classified integrated, D3D11 FL 10_1 device created, D3D12 rejected with 0x887A0004, WDDM 1.2, no display | 2 | verified |
| Microsoft Basic Render Driver shown as software renderer, not counted | 2 | verified |
| Detection time: full probe / probes reused | 2 | 207–269 ms / 10–11 ms |
| Device-change notification triggers one debounced re-detection without recreating devices | 2 | verified (3 posted notifications → 1 re-detection) |

### Live metrics verified on this hardware (Phase 3, 2026-10-16)

| Metric | GT 730 | HD Graphics 2000 | Source |
|---|---|---|---|
| Utilization (busiest engine) | ✔ — 98–100 % under a D3D11 load; matches PDH (99 % vs 99.4 %) | ✔ — 94–100 % under load (no PDH instances exist to compare) | D3DKMT |
| Per-engine utilization | ✔ 8 named engines (3D, Compute_0, Overlay, Video Decode ×3, Copy ×2) | ✔ 6 engines, unnamed by the driver ("Engine 0–5") | D3DKMT |
| Attribution | load on GT 730 → Intel 0 % | load on Intel → GT 730 2.2 % (its desktop) | D3DKMT |
| Dedicated memory in use | ✔ matches PDH (445 MB vs 445 MB) | ✔ (0 bytes resident in its 32 MB carve-out when idle) | D3DKMT |
| Shared memory in use | ✔ | ✔ | D3DKMT |
| Temperature | ✔ (40–45 °C idle) | Unsupported — no sensor interface | NVML |
| Fan speed | ✔ (54 %) | Unsupported | NVML |
| Performance state | ✔ (P8 idle, P0 under load) | Unsupported | NVML |
| Power draw | Unsupported (NVML NOT_SUPPORTED) | Unsupported | NVML |
| Core / memory clock | Unsupported (NVML NOT_SUPPORTED) | Unsupported | NVML |

NVML: `C:\Program Files\NVIDIA Corporation\NVSMI\nvml.dll`, driver 391.35, NVML 9.391.35.

Monitoring cost (release build, Dashboard visible, i5-2400S): **0.15 % of the machine at 1 s**
(6 ms CPU/s; collection 0.41 ms avg / 1.1 ms max per sample), 0.83 % at 250 ms; working set ≈ 47 MB.
With monitoring paused: not measurable (< 1 timer tick of 15.6 ms over 60 s).

### Per-process data verified on this hardware (Phase 4, 2026-10-16)

| | GT 730 (WDDM 2.3) | HD Graphics 2000 (WDDM 1.2) | Basic Render Driver |
|---|---|---|---|
| Source | Windows performance counters (PDH) | D3DKMT per-process statistics | PDH |
| Processes covered (standard user) | all (~170), incl. dwm.exe, csrss.exe | ~64 of ~170 — the user's own processes | all |
| Known load from a test process | 99 % (busiest engine 3D), 64 MB dedicated | 94–100 % (Engine 0), 66–72 MB shared | — |
| Agreement with the other source (same process) | D3DKMT 99 % vs PDH 99 %; 64 MB vs 64 MB | n/a (no PDH instances) | — |
| Cost of one pass | ~1 ms | < 1 ms | (included) |

Observed at idle: dwm.exe 2–3 % (Compute_0), csrss.exe ~1 % (3D) on the GT 730; explorer, Brave,
Edge WebView2 etc. hold GPU memory but show no engine time ("Memory only").

Applications page cost: 0.42 % of the machine while open (1 s interval, processes every 2 s);
nothing when closed.

### Windows per-app GPU preference (Phase 5, 2026-10-16)

Launched probe under Not set / Let Windows decide / Power saving / High performance: default D3D11
device, first enumerated adapter and the `MINIMUM_POWER` query were the **GT 730 every time**. The
preference does not route programs to the Intel HD 2000 on this PC. Details: GPU_SELECTION.md.

### Benchmarks measured on this hardware (Phase 7, 2026-10-16)

Preset *standard* (1024 × 1024 hash, 512 iterations per pixel, seed 12345), 1 warm-up + 5 measured
runs, two sessions a minute apart. Every output was checked: the GPU images are bit-exact with the
CPU reference.

| Benchmark | Session 1 mean | Session 2 mean ± SD | Change |
|---|---|---|---|
| Hash — NVIDIA GeForce GT 730 (D3D11 FL 11_0) | 56.81 ms | 57.44 ± 1.51 ms | none significant |
| Hash — Intel HD Graphics 2000 (D3D11 FL 10_1) | 306.55 ms | 306.4 ± 0.21 ms | none significant |
| Hash — CPU, 4 threads (i5-2400S) | 697.93 ms | 692.5 ± 10.0 ms | none significant |
| Hash — CPU, 1 thread | 2202.94 ms | 2188.8 ± 12.0 ms | none significant |
| Upload + read-back 32 MB — GT 730 | 37.08 ms | 37.35 ± 3.71 ms | none significant |
| Upload + read-back 32 MB — HD 2000 | 39.68 ms | 38.44 ± 0.71 ms | none significant |
| Adapter detection, full probe | — | 182.6 ± 2.6 ms | none significant |
| Monitoring: one adapter sample | — | 0.164 ± 0.011 ms | none significant |
| Monitoring: one per-process pass | — | 8.24 ± 0.36 ms | 13.6 % slower (cause not determined; it scales with the number of running processes) |
| Probe launch (process + D3D11 device) | — | 137.9 ± 1.9 ms | none significant |

On this workload the GT 730 takes about 1/5.4 of the HD 2000's time and 1/12 of the time of all four
CPU cores. Combining both GPUs could therefore at best save roughly 16 % (the HD 2000 would take
~16 % of the rows in a perfectly balanced split) before any overhead — Phase 8 will measure whether
that is achieved; nothing is claimed until then.

### Workload Lab measured on this hardware (Phase 8, 2026-10-16)

Split compute, standard size (1024 × 1024, 512 iterations), mean of 5 runs after ≥ 1 s warm-up;
three sessions (a fourth, run before the warm-up fix, is excluded — see MULTI_GPU_RESEARCH.md). Every
image bit-exact with the CPU reference.

| Mode | Time | vs GT 730 alone |
|---|---|---|
| GT 730 alone | 60.6–61.6 ms | reference |
| HD 2000 alone | 311 ms | 5.1× slower |
| CPU, 3 threads alone | 850–861 ms | 14× slower |
| Even split between the GPUs | 156 ms | 2.5× slower |
| **Split by measured speed (≈ 84 % / 16 %)** | **51.4–53.8 ms** | **1.14–1.18× faster** (ideal 50.7–51.4 ms) |
| Shared queue, 32-row items | 60.2–60.3 ms | no significant difference |
| Shared queue, GPUs + 3 CPU threads | 81.6–81.9 ms | 1.33–1.35× slower |

Heavy size (2048 × 2048): GT 730 alone 240.7 ms; split by measured speed 202.6 ms = **1.19× faster**
(ideal 201.6 ms); shared queue 215.9 ms = 1.12× faster.

Video decoding, 1280 × 720 H.264, 300 frames per stream (three sessions):

| Decoder | One stream | Two streams on the same device |
|---|---|---|
| Intel HD 2000 (hardware) | 840–884 frames/s | 843–854 frames/s (703–712 ms) |
| Software (CPU) | 460–468 frames/s | 478–486 frames/s |
| GT 730 (hardware) | 189–190 frames/s | 190–191 frames/s (3146–3152 ms) |
| **One stream on each GPU** | — | **379 frames/s (1583–1584 ms) — 2.2× slower than both on the HD 2000** |

Both GPUs decode in hardware (frames delivered as textures on that GPU) and produce frames identical
to the software decoder's.

### Why the Intel GPU is missing from Task Manager

It is present and usable (a Direct3D 11 device is created on it). Task Manager's GPU view and the
Windows "GPU Engine" counters only cover WDDM 2.0+ drivers; the HD 2000's last driver (9.17.10.4459)
is WDDM 1.2. GPU Fusion lists it from DXGI and Device Manager instead.
