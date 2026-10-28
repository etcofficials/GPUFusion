# Adaptive multi-GPU scheduler (1.12)

GPU Fusion 1.12 adds a scheduler that divides **one workload made of independent units** between
the GPUs. The fastest GPU does most of the work, and the slower GPU helps only as much as
measurably pays off. It is the SDK's [`scheduler.h`](../sdk/include/gpufusion/scheduler.h), and
GPU Fusion uses the same code in the Workload Lab. It works for GPU Fusion's own workloads and for
programs that integrate the SDK. **It does not change how any other program, game or browser uses
the GPUs.**

Every conclusion below is labelled:

- **[Test]**: measured on the reference PC (i5-2400, GT 730 on driver 391.35, HD Graphics 2000),
  2026-10-28.
- **[Source]**: confirmed from the source code.
- **[HW]**: unsupported on this hardware.

## What exists, and where

| Mode | What it does | Where |
|---|---|---|
| A Automatic | Windows and the program choose the GPU | GPU Manager |
| B Integrated GPU | Intel for the whole program, where a mechanism exists (the program's own option, or the SDK). Never claimed when the program still runs on NVIDIA. | GPU Manager |
| C Discrete GPU | NVIDIA for the whole program (Windows' preference, verified) | GPU Manager |
| D Both GPUs (SDK programs) | both GPUs are *offered* to a program that integrates the SDK. Other programs run on one GPU. | GPU Manager, SDK |
| E Independent job batches | separate jobs, each on one GPU; the shared queue gives the faster GPU more jobs and keeps the slower one off the last jobs | Workload Lab → Job batches, `--dispatch` |
| F Split workload | independent units of one workload over both GPUs, primary first, adaptive | Workload Lab → Adaptive multi-GPU scheduler, `--run-lab adaptive`, SDK |

## How it works [Source]

- **Devices.** One Direct3D 11 device per GPU, created once and reused. The workload is a pixel
  shader (shader model 4.0), because the HD 2000 (feature level 10_1) has no compute shaders; a GPU
  that cannot run it is skipped, with the reason. Render targets are reused for any batch size.
  Waiting for a GPU sleeps on a DXGI event (`IDXGIDevice2::EnqueueSetEvent`) instead of spinning.
- **Units.** The units are rows of the hash image (GPU Fusion's verifiable test workload: integer
  hashing, bit-exact on CPU and GPU). Each batch is copied to system memory into one image. That
  copy is measured, and nothing is copied between the GPUs.
- **Scheduling.** The primary (the fastest GPU alone, the GT 730 here) takes batches from the top.
  The helper takes small batches from the bottom, up to its planned share, and only when it would
  finish before the primary could finish everything left. Otherwise the batch is shrunk or the
  helper stops. The remaining range is one atomic word: no locks, no extra worker processes, one
  thread per helper per run. A helper's failed batch is re-run on the primary, and every row is
  computed exactly once (checked).
- **Adapting.** The `Balancer` measures the primary alone first (two runs). It then gives the
  helper the policy's starting share (10, 15 or 20 %), never more than 1.1 × its measured share of
  the combined throughput, and compares splits with the primary alone using the fastest of the last
  3 runs on each side. Other activity can only slow a run down, so one disturbed run decides
  nothing.
  - A gain raises the helper's share, up to the policy's cap (20, 35 or 50 %).
  - A miss halves it.
  - Repeated misses, or any failure, switch the helper off.
  - Every 8 splits it re-measures the primary alone.
- **Verification.** Every image is compared bit for bit with the CPU reference. The SDK demo
  compares with the primary GPU's own image, and that image with the CPU on sample rows.

Fixed during development, measured:

- Waiting by spinning cost a whole CPU core. The SDK demo used 25 % of all cores; with the event
  wait it used 2.5 %, with the same times. [Test]
- (1.12.1) The SDK demo reported as "transfer" the whole wait for the GPU: about 50 ms of a 53 ms
  run. It now times the copy with GPU timestamp queries: about 3 ms per run for the GT 730 and
  0.9 ms for the HD 2000 at standard size. A second wait before the copy would also have separated
  them, but it made runs 3–4 % slower, so it was not kept. The Lab already timed the copy apart from
  the shader work. [Test]
- An exponential average let one disturbed run count as a miss, and the conservative policy once
  switched the Intel GPU off at the standard size although splits were about 12 % faster. Comparing
  the fastest recent runs fixed it. [Test]

## Results [Test]

Workload Lab, `--run-lab adaptive`, 10 measured runs per mode after at least 1 s of warm-up. Times
run from start to the whole image in CPU memory, with transfers, synchronisation and baseline runs
included. Images were correct in every run. "Speed-up" is GPU Fusion's own verdict: means compared,
"faster" only when the difference is beyond twice the combined standard error and at least 1 %.

| Size | GT 730 alone, median | Intel alone, median | Adaptive, median (conservative / balanced / performance) | Intel's rows | Verdict (best policy vs GT 730 alone) |
|---|---|---|---|---|---|
| Quick 512², 256 iterations | 9.85 ms | 44.5 ms | 10.23 / 9.84 / **9.29 ms** | 0 / 0 / 11.5 % | 1.06× faster (performance). Conservative and balanced **switched the Intel GPU off**: the gain was below their thresholds, so they ran the GT 730 alone. |
| Standard 1024², 512 iterations | 63.8 ms | 320.6 ms | 56.2 / 55.7 / **55.5 ms** | 13.0 / 13.4 / 12.0 % | **1.14× faster**; 95 % of a perfect, overhead-free split (53.1 ms) |
| Heavy 2048², 512 iterations | 243.4 ms | 1276.6 ms | 211.8 / **210.7** / 211.1 ms | 12.7 / 12.8 / 12.4 % | **1.14× faster**; 95 % of the ideal (204.1 ms) |

- **Scheduler overhead.** Its own time was ≤ 0.02 ms per run. Time on the critical path that was
  not GPU work (thread start, waiting) was 0.07–0.14 ms per run, under 0.3 % at standard size.
- **Transfers.** Copying the GT 730's rows to system memory took about 4 ms at standard size and
  11 ms at heavy. For the Intel GPU's share it took 3–7 ms, overlapped with the GT 730's work.
- **CPU and memory.** During the adaptive modes GPU Fusion used 2–8 % of all CPU cores (window
  included), with one exception: the balanced quick mode measured 19 %, cause not determined. Peak
  working set was 45 MB (quick), 51 MB (standard) and 78 MB (heavy; the image is 16 MB). Idle and
  minimized, while still monitoring every second, it used 0.02 % of all cores, a 50 MB working set
  and 7 threads.
- **Fallback.** Every helper batch failed on purpose. The GT 730 re-ran the helper's rows (8, 16 or
  32 per run) and the image was correct in every run, in 10.5, 65.3 and 245.0 ms: the same as or up
  to 1.03× slower than the GT 730 alone.
- **SDK demo.** `GPUFusionSplitDemo --all-adapters --size standard --runs 8`, conservative policy,
  5 repeats: 1.14–1.16× faster than the GT 730 alone, with Intel doing 12.6–13.5 % of the rows and
  the demo using 1.9–2.5 % of all cores. After the 1.12.1 fixes, 3 runs alternating with the 1.12.0
  demo: 1.13–1.14× (1.12.0: 1.13–1.16×), Intel 13.6 % of the rows, 4–6 % of all cores for both
  versions in that session. In one further run, inside the test suite, the Balancer
  ended with the Intel GPU switched off ("no significant difference", 1.00×). That run used the old
  exponential averaging, and its cause was not determined.
- **Both profile.** The demo started from a Both profile (`--no-baselines`) was verified by GPU
  Fusion's own per-process observation: GT 730 3D up to 93 %, Intel up to 77 %, "Both GPUs used".
- **Independent job batches** (8 H.264 decode jobs, 3 runs per configuration):

  | Configuration | Mean |
  |---|---|
  | GT 730 alone | 13.73 s |
  | Intel alone | 3.23 s |
  | Intel, 2 jobs at a time | 3.04 s |
  | Both GPUs | 2.90 s |

  That is 1.05× faster than the best single configuration, with identical outputs. The faster
  decoder here is the **Intel GPU**, and the dispatcher gave it 7 of the 8 jobs. "NVIDIA first"
  holds for the hash workload, not for every workload; the scheduler always uses the measured
  fastest device.

## Limits

- **Only some workloads benefit.** The gain applies to workloads of independent units whose results
  are copied back anyway. The workload must be written for it: GPU Fusion's own, or an SDK program's.
- **Small workloads gain little or nothing,** because the fixed costs dominate. The scheduler then
  correctly uses the GT 730 alone (quick size, conservative and balanced). [Test]
- **Games cannot be split.** Splitting one rendering workload (a game frame) needs explicit
  multi-adapter support in the game and Direct3D 12 on both GPUs; the HD 2000 has none. [HW]
- **Roblox and Minecraft** do not integrate the SDK. Both mode leaves them on one GPU, and the
  Applications page says "One GPU only". See [GPU_SELECTION.md](GPU_SELECTION.md#roblox-and-minecraft).
- **Some APIs are not used:** CUDA and OpenCL (the Intel GPU has neither here, and they are not
  needed), and compute shaders (the HD 2000 has none). [HW]

## Reproducing

```
GPUFusion.exe --data-dir <folder on G:> --run-lab adaptive --lab-size quick|standard|heavy [--lab-policy conservative,balanced,performance]
GPUFusionSplitDemo.exe --all-adapters --size standard --runs 12 --policy conservative
GPUFusion.exe --dispatch decode --dispatch-inputs <folder of H.264 clips> --dispatch-compare 3
ctest --preset release -R "tst_sdk|tst_labmodel|tst_lab_live|tst_splitdemo_live"
```

Run counts come from Settings → Benchmarks; the results above used 10 measured runs. None of these
commands change Windows settings.
