# GPU Fusion SDK (optional), version 2 (row kernels, C library and Godot extension since GPU Fusion 1.14)

GPU Fusion is a generic GPU manager: it works with any executable, and no program needs to know about
it. The SDK is for programs whose developers choose to integrate it, so that they can:

- **follow the GPU choice made in GPU Fusion**, even where Windows cannot enforce that choice;
- **divide their own work between GPUs** when the user picks "Both GPUs (SDK programs)", with
  GPU Fusion's adaptive, NVIDIA-first scheduler.

**Which programs use it:**

- GPU Fusion's own `GPUFusionProbe.exe`, `GPUFusionSplitDemo.exe` and `GPUFusionImage.exe`;
- Godot projects that include the GPU Fusion extension (`sdk/godot`);
- anything that loads `GPUFusionSDK.dll`.

No other program, games and browsers included, uses it unless its developers add it. In particular,
Roblox and Minecraft do not use it, and GPU Fusion cannot add it to them (see
[GPU_SELECTION.md](GPU_SELECTION.md#roblox-and-minecraft)).

The SDK is header-only (C++17, Windows, MIT License), with nothing to build and no DLL to ship:

| Header | Contents |
|---|---|
| [`gpufusion.h`](../sdk/include/gpufusion/gpufusion.h) | the mode and adapters GPU Fusion offered, the format version, a weighted split |
| [`d3d11.h`](../sdk/include/gpufusion/d3d11.h) | the capabilities of each adapter, and one Direct3D 11 device per adapter |
| [`scheduler.h`](../sdk/include/gpufusion/scheduler.h) | the adaptive scheduler (independent work units over several devices), measured per-device times, a fallback to the primary device, and the `Balancer` (with `save()`/`restore()` of what it learned) |
| [`rows.h`](../sdk/include/gpufusion/rows.h) | **row kernels** (1.14): one SM 4.0 pixel shader run over an image's rows on every GPU, inputs uploaded with halo rows, results copied back. Each step is timed with GPU timestamps; waits sleep on a DXGI event. Includes a CPU reference of the sample kernel. |
| [`gpufusion_c.h`](../sdk/include/gpufusion/gpufusion_c.h) + `GPUFusionSDK.dll` | the same engine behind a **plain C interface** (1.14), for other languages and engines' native plugins: C#, Java (FFM), Python (ctypes), Rust, Unity native plugins |
| [`sdk/shaders/blur_ps.hlsl`](../sdk/shaders/blur_ps.hlsl) | the sample row kernel: a tent blur in exact integer arithmetic |

A program that is not launched by GPU Fusion gets the default adapter only. Nothing breaks.

## The contract (format version 1)

When GPU Fusion starts a program from its profile (GPU Manager → *Launch* / *Verify on this PC*,
`--launch-profile`, `--verify-profile`), or runs it as a job of a batch, it sets these variables:

| Variable | Value |
|---|---|
| `GPUFUSION_ENV_VERSION` | `1`: the format of these variables. GPU Fusion before 1.12 did not set it; the format was the same. |
| `GPUFUSION_MODE` | `automatic`, `integrated`, `discrete` or `both` |
| `GPUFUSION_ADAPTERS` | comma-separated adapter LUIDs `HHHHHHHH_LLLLLLLL`, in order of use (Both: the discrete GPU first, the primary; then the integrated one). Empty for Automatic. |
| `GPUFUSION_PROFILE` | the profile id (not set for batch jobs) |

LUIDs are valid only until the next restart, so GPU Fusion passes them fresh at every launch. A
profile can switch this off with "Tell the program which GPU to use…". If a newer GPU Fusion sets a
format version this SDK does not know, or the value is not a version number,
`environmentSupported()` is false. `mode()` and
`requestedLuids()` then act as if the program was not launched by GPU Fusion, so the program falls
back to the default adapter.

**Limitation:** the variables reach only the process GPU Fusion starts and its children. Two cases
lose them, and GPU Fusion warns about both when it launches such a program:

- A Microsoft Store / Xbox package may be started through its package.
- A copy that is already running may take over a new start.

## Dividing work with the scheduler

```cpp
#include <gpufusion/d3d11.h>
#include <gpufusion/scheduler.h>

Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
CreateDXGIFactory1(IID_PPV_ARGS(&factory));
// The offered adapters (else the default one), one device each; adapters that cannot create a
// device are left out with the reason in `errors`.
std::wstring errors;
std::vector<gpufusion::Device> devices = gpufusion::createDevices(gpufusion::adapters(factory.Get()), &errors);
// devices[i].caps: name, LUID, feature level, compute shaders yes/no, dedicated memory

gpufusion::Balancer balancer(devices.size(), gpufusion::Policy::Conservative);
for (each job of the same kind) {
    const gpufusion::Plan plan = balancer.plan(units);              // who does how much, and why
    const gpufusion::RunReport report = gpufusion::run(units, plan, [&](gpufusion::Batch& b) {
        // compute units [b.first, b.first + b.count) on devices[b.device];
        // copy the result into your output; report the copy time in b.transferMs
        return ok;                                                  // false: re-run on the primary
    }, balancer.unitsPerMs());
    balancer.record(plan, report);
    // report: ok, exactCoverage, wallMs, overheadMs(), per device: units, batches, workMs,
    //         transferMs, scheduleMs, busyMs, why a helper stopped
}
```

How `run()` divides the work:

- **Device 0 is the primary.** In Both mode this is the discrete GPU, because GPU Fusion lists it
  first. If you measure the devices yourself, put the fastest first. The primary takes batches from
  the front of the range and always does most of the work.
- **Every other device is a helper.** It takes small batches from the back, but only up to its
  planned share. It also takes a batch only if it would finish it before the primary could finish
  everything that is left: the batch is shrunk to fit, or the helper stops. A slow GPU therefore
  cannot hold up the end of the job. This is the same rule the job dispatcher uses for whole jobs.
- **A helper's failed batch is re-run on the primary.** Every unit runs exactly once, and the report
  checks this (`exactCoverage`). If the primary fails, the run fails; nothing is silently skipped.
- **No locks are used.** The remaining range is one atomic word. Helpers run on their own threads;
  the primary runs on the calling thread. A device is only ever used by one thread at a time.

How the `Balancer` adapts across runs of one kind of workload:

1. **Baseline.** It runs the primary alone twice; the second run replaces a cold first one. These
   runs do the program's real work, so they are not extra benchmark runs.
2. **Splits.** It then gives each helper the policy's initial share, capped by the helper's measured
   speed: never more than 1.1 × its share of the combined throughput.
3. **Comparison.** It compares the time per unit of splits with the primary alone, using the
   fastest of the last 3 runs on each side. Other activity can only slow a run down, so one
   disturbed run decides nothing.
   - A gain that used the helper's whole share raises the share, up to the policy's maximum.
   - A split that was not at least the policy's minimum gain faster halves the share.
   - Repeated misses switch the helper off, and so does any failure.
4. **Refresh.** After every 8 splits it re-measures the primary alone.

| Policy | Helper's first share | Maximum share | Required gain | Misses before off |
|---|---|---|---|---|
| Conservative (default) | 10 % | 20 % | 5 % | 2 |
| Balanced | 15 % | 35 % | 2 % | 3 |
| Performance | 20 % | 50 % | any | 3 |

The shares are starting points, not promises. Where the helper GPU does not help, every policy ends
up using the primary alone.

## Measured on the reference PC

The workload is the hash image (rows as work units), a Direct3D 11 pixel shader, because the HD 2000
has no compute shaders. Each GPU's rows are copied through system memory, and that copy is included
in every time below. GT 730 and Intel are listed in that order. More detail is in
[MULTI_GPU_SCHEDULER.md](MULTI_GPU_SCHEDULER.md).

| Program | Observed |
|---|---|
| `GPUFusionSplitDemo --all-adapters --size standard --runs 8` (1.12.1, 3 runs) | GT 730 alone: 62.2–62.8 ms median. Intel alone: 316–318 ms. Adaptive (conservative): 53.5–54.1 ms median, with Intel doing 13.6 % of the rows. Verdict "faster", 1.13–1.14×. Copy to system memory (GPU timestamps): about 3 ms per run on the GT 730 and 0.9 ms on the Intel GPU. Every image was bit-exact. |
| `GPUFusionProbe` in a Both profile | both GPUs did work. The probe renders the same work on each GPU; it does not split it. |
| `GPUFusionSplitDemo` in a Both profile (`tst_splitdemo_live`) | GPU Fusion's verification: "Both GPUs used". |

## Row kernels: real work on both GPUs at once (1.14)

`rows.h` turns the scheduler into something a program can use for real data without writing the
Direct3D plumbing:

```cpp
#include <gpufusion/d3d11.h>
#include <gpufusion/rows.h>
#include "blur_ps.h"   // fxc /T ps_4_0 /Fh blur_ps.h /Vn g_blur_ps sdk/shaders/blur_ps.hlsl

auto devices = gpufusion::createDevices(adapters);                 // the fastest GPU first
std::vector<gpufusion::RowExecutor> ex(devices.size());
for (size_t i = 0; i < devices.size(); ++i)
    ex[i].init(devices[i], g_blur_ps, sizeof g_blur_ps, /*halo rows*/ 6, &error);
gpufusion::Balancer balancer(ex.size(), gpufusion::Policy::Conservative, 8);
balancer.restore(savedState);                                      // optional: start with what it learned last time
gpufusion::RowJob job{ in, w * 4, out, w * 4, w, h, { /*params[0] = radius*/ 6 } };
gpufusion::RowRun run = gpufusion::runRows(ex, balancer, job);     // plan, run on all GPUs, learn
// run.report: rows per GPU, times; run.timing: upload / compute / readback per GPU (GPU timestamps)
savedState = balancer.save();
```

**Writing a kernel:** an SM 4.0 pixel shader:

```hlsl
Texture2D<uint4> src : register(t0);
cbuffer Rows : register(b0) { uint4 rows; uint4 params[4]; };
// rows = { first output row of the batch, first input row in src, width, height }
uint4 main(float4 pos : SV_Position) : SV_Target { ... }   // output pixel (pos.x, rows.x + pos.y)
```

- Pixels are RGBA8 as `R8G8B8A8_UINT`.
- Feature level 10_0 is enough, so the HD 2000 runs it.
- Integer arithmetic makes results exact and checkable.

**What the program must do:**

- divide its work into rows (or any independent units);
- provide the kernel and a CPU reference, or another check;
- keep one Balancer per kind of work.

The SDK does the rest:

- devices, uploads and readbacks;
- the division by measured speed;
- re-running a failed helper batch on the primary;
- exactly-once accounting and timing.

**The Balancer's warm-up (1.14):** the first split after a helper is enabled includes one-time costs
(its textures, the driver's first use of the shader), so it is not judged. Before this, a cold first
split could make the conservative policy switch the helper off in one of three Godot runs.

### Measured on the reference PC [Test, 2026-10-28]

All outputs were compared bit for bit with the CPU reference; every one matched.

| Program | Work | GT 730 alone | Both GPUs | Verdict |
|---|---|---|---|---|
| `GPUFusionImage --mode both` | 13 photographs (the Windows wallpapers, 1920×1200 and 3840×1200), tent blur r = 6, 4 runs | 1,433 ms per batch (median) | split: 1,302 ms, Intel 11 % of rows; batch: 1,295 ms, Intel 7.7 % of images | **1.14× faster** (split), **1.15×** (batch) |
| Godot 4.7.2, `sdk/godot/demo/check.gd` (headless) | one 1920×1200 photo, r = 6, 12 runs, 5 repeats | 101.7–104.9 ms (median per repeat) | 90.8–93.7 ms, Intel 12 % of rows | **1.09–1.12× faster** in all 5 repeats |
| Python via `GPUFusionSDK.dll` (`sdk/examples/python`) | 1920×1080 test image, r = 6, 14 runs | 90.0 ms (the Balancer's refresh run) | 78–84 ms, Intel 13 % of rows | — (example, not a benchmark) |

- **CPU use:** `GPUFusionImage` used 3–4 % of all cores. Its peak working set was 460 MB, because it
  keeps all 13 photos plus their references in memory.
- **Measured routes:**
  - row-parallel pixel shaders on both GPUs (above);
  - independent jobs on both GPUs: FFmpeg and the built-in decoder through the dispatcher, and
    `GPUFusionImage` batch mode.
- **Unavailable here:** compute shaders and OpenCL on the HD 2000, D3D12 on the HD 2000, and Vulkan on
  either GPU. Each is checked, not assumed.

## Integrations

**Godot (GDExtension), `sdk/godot`:**

- The class `GPUFusionCompute`:
  - `adapters()`;
  - `blur(image, radius)`, which runs on every GPU;
  - `blur_reference(image, radius)`, on the CPU;
  - `last_report()`: rows per GPU, times, the plan;
  - `configure(policy, max_gpus)`;
  - `learned_state()` / `restore_state()`.
- **Build:** `scripts\build-godot.ps1`, using a godot-cpp 4.5 checkout. The extension loads in Godot
  4.5 and later, and was tested with 4.7.2.
- **The demo project** `sdk/godot/demo` runs headless:
  `Godot --headless --path sdk/godot/demo --script res://check.gd -- <image> <radius> <runs>`.
- **What a Godot developer does:**
  - copies `demo/bin/gpufusion_godot.dll` and `demo/gpufusion.gdextension` into the project;
  - calls `GPUFusionCompute` for image work: heightmaps, procedural textures, filters.
- **What it does not do:** it does not change how Godot renders.
- **Godot 4.7.2 quirks seen here:**
  - **The first headless `--import`** of a project with *any* GDExtension crashes Godot on exit (an empty
    extension with no GPU Fusion code does the same); a second import and normal runs work.
    `build-godot.ps1` retries the import once.
  - **The extension is built against godot-cpp `template_debug`.** A `template_release` build crashed
    the editor's import.
  - **Godot's data stays on G::** a `_sc_` file next to Godot keeps its editor data there, and the
    script points `APPDATA` at `G:\DevTools\godotppdata` for Godot runs.

**Any language, `GPUFusionSDK.dll` and `gpufusion_c.h`:**

- functions: `gf_create`, `gf_adapter_info`, `gf_kernel_create` (your own compiled shader),
  `gf_kernel_blur`, `gf_run` (with a report), `gf_learned`/`gf_restore`, `gf_blur_reference`;
- `gf_abi_version()` must equal `GF_ABI_VERSION`;
- example: `sdk/examples/python/blur_both_gpus.py`;
- **Unity:** put the DLL in `Assets/Plugins` and declare the functions with `[DllImport]`;
- **Java 22+:** the Foreign Function & Memory API calls the same functions.

**Java and Minecraft:** a Java program or mod could call `GPUFusionSDK.dll` through FFM. No such mod
exists, and Minecraft itself cannot be changed by GPU Fusion (see
[GAME_MULTI_GPU_FEASIBILITY.md](GAME_MULTI_GPU_FEASIBILITY.md)).

## What the SDK cannot do

- It cannot make one rendering workload (one swap chain, one frame) run on two GPUs. That needs the
  program's own explicit multi-adapter design and Direct3D 12 on every GPU. On this PC that is
  impossible: the Intel GPU has no Direct3D 12, and cross-vendor resource sharing does not exist.
- It cannot share resources between GPUs of different vendors without a copy through system memory.
- It cannot affect programs that do not include it. Those follow Windows' per-app preference (where
  that works) or their own GPU setting.
- It does not promise a speed-up. On mismatched GPUs, splitting is often slower, and then the
  Balancer uses the primary alone.
