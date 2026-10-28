# GPU Fusion SDK (optional), version 2

GPU Fusion is a generic GPU manager: it works with any executable, and no program needs to know about
it. The SDK is for programs whose developers choose to integrate it, so that they can:

- **follow the GPU choice made in GPU Fusion**, even where Windows cannot enforce that choice;
- **divide their own work between GPUs** when the user picks "Both GPUs (SDK programs)", with
  GPU Fusion's adaptive, NVIDIA-first scheduler.

**Which programs use it:** GPU Fusion's own `GPUFusionProbe.exe` and `GPUFusionSplitDemo.exe`. No
other program, games and browsers included, uses it unless its developers add it. In particular,
Roblox and Minecraft do not use it, and GPU Fusion cannot add it to them (see
[GPU_SELECTION.md](GPU_SELECTION.md#roblox-and-minecraft)).

The SDK is header-only (C++17, Windows, MIT License), with nothing to build and no DLL to ship:

| Header | Contents |
|---|---|
| [`gpufusion.h`](../sdk/include/gpufusion/gpufusion.h) | the mode and adapters GPU Fusion offered, the format version, a weighted split |
| [`d3d11.h`](../sdk/include/gpufusion/d3d11.h) | the capabilities of each adapter, and one Direct3D 11 device per adapter |
| [`scheduler.h`](../sdk/include/gpufusion/scheduler.h) | the adaptive scheduler (independent work units over several devices), measured per-device times, a fallback to the primary device, and the `Balancer` |

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

## What the SDK cannot do

- It cannot make one rendering workload (one swap chain, one frame) run on two GPUs. That needs the
  program's own explicit multi-adapter design and Direct3D 12 on every GPU. On this PC that is
  impossible: the Intel GPU has no Direct3D 12, and cross-vendor resource sharing does not exist.
- It cannot share resources between GPUs of different vendors without a copy through system memory.
- It cannot affect programs that do not include it. Those follow Windows' per-app preference (where
  that works) or their own GPU setting.
- It does not promise a speed-up. On mismatched GPUs, splitting is often slower, and then the
  Balancer uses the primary alone.
