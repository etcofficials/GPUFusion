# GPU selection per application

**Status:** adapter-oriented choices (Automatic / integrated GPU / discrete GPU / Both), mapped onto
Windows' per-app preference only where verification justifies it, plus **verification of where a
program really renders** (launch → observe its process tree → 3D/compute engine activity per GPU).
Measured on the reference PC 2026-10-20.

## Diagnosis: why "use the Intel GPU" did nothing

GPU Fusion's buttons were already writing the real Windows setting correctly — the value is in
`HKCU\Software\Microsoft\DirectX\UserGpuPreferences` and reads back as written. The setting simply
has no effect on this PC, for reasons GPU Fusion now reads from Windows itself:

| | NVIDIA GeForce GT 730 | Intel HD Graphics 2000 |
|---|---|---|
| DXGI enumeration order | #0 (first) | #1 |
| Kernel adapter type (`KMTQAITYPE_ADAPTERTYPE`) | render, display, **boot (POST) device** | render, display |
| Hybrid integrated / discrete flag | — | — |
| Displays attached | **1 (the primary)** | 0 |
| Driver model | WDDM 2.3 | WDDM 1.2 (2012) |

Windows moves a program between GPUs only for the per-app preference's *power saving* / *high
performance* ranking (`IDXGIFactory6::EnumAdapterByGpuPreference`). On this PC Windows ranks the GT
730 first for **every** preference, including `MINIMUM_POWER`: neither GPU carries a hybrid
classification, the GT 730 is the boot device and drives the only monitor, and the Intel driver
predates the GPU-preference model. Measured with the probe (Default D3D11 device under each
preference): GT 730 every time. Measured with real rendering (below): a program with
`GpuPreference=1;` (power saving) still renders on the GT 730.

**This is a property of the hardware/driver/display configuration, not something a registry value
can change.** GPU Fusion does not modify drivers, firmware or global settings.

## What GPU Fusion does now

### Choices, and what gets written

| Choice | Stored value | Written only when |
|---|---|---|
| Automatic | value removed (`GpuPreference` key deleted, other keys kept) | always |
| Integrated GPU (Intel) | `GpuPreference=1;` | the verification showed the probe landing on the integrated GPU under "Power saving" (or, before a full verification, Windows ranks it first for minimum power) |
| Discrete GPU (NVIDIA) | `GpuPreference=2;` | likewise for "High performance" |
| Both GPUs | nothing | never — see the dual-GPU check |

Each choice shows *Verified on this PC*, *Expected (not verified yet)*, *Windows cannot enforce this
on this PC* or *Unknown*, with the evidence. On the reference PC: **NVIDIA = Verified, Intel = cannot
be enforced** (Apply is disabled for it). The raw Windows values remain available under *Advanced*.
Adapters are matched to the saved verification by vendor and model, because Windows assigns new
LUIDs at every boot. Every write is journalled first, read back afterwards, and undoable.

### Verifying a program ("Verify on this PC…" / `--verify-app`)

1. **Preference saved** — read back from Windows (or "not applicable" for Automatic).
2. **Application launched** — GPU Fusion starts it with the user's normal environment; if an instance
   was already running it says so (Windows applies a preference only at process start).
3. **GPU activity observed** — once per second, per process of the *launched tree* (the program and
   every process it starts, matched by parent PID and creation time so reused PIDs are not counted),
   per adapter: Windows GPU counters for WDDM 2.x adapters, D3DKMT for WDDM 1.x adapters.
   Rendering = 3D and compute engines only, at ≥ 2 % in ≥ 2 passes. Video decode/encode, copy
   engines, other processes (e.g. the desktop compositor) and total GPU load are never counted.
4. **Requested GPU confirmed** — rendering only on the requested adapter → *Confirmed*; on another →
   *Not applied*; on both → *Partly*. The Intel WDDM 1.2 driver does not name its engines, so work
   there is reported as "of unknown type": GPU Fusion can say *where* it ran, not that it was 3D.

If the rendering comes from a different executable than the one selected (a launcher), GPU Fusion
names it and offers to configure that executable instead.

### Measured on the reference PC (2026-10-20)

| Program | Stored preference | Result |
|---|---|---|
| GPUFusionProbe `--render` (default device, like a typical program) | none | **Confirmed on GT 730** — 3D up to 48 % |
| GPUFusionProbe `--render --adapter <Intel>` (program that picks its GPU) | none | **Work seen only on Intel HD** (up to 100 %, engine type not reported) |
| GPUFusionProbe `--render` | `GpuPreference=1;` (power saving → Intel requested) | **Not applied — rendered on GT 730** |
| `cmd.exe /c GPUFusionProbe --render` (launcher) | none | rendering traced to GPUFusionProbe.exe; cmd.exe flagged as launcher |
| Microsoft Edge, WebGL page (separate profile) | none | rendered on **GT 730**, 3D up to 100 % |

So: **NVIDIA selection works** (it is also the default). **Intel selection cannot be enforced by
Windows here** — verified with actual rendering, not inferred. Programs that choose their adapter
themselves (in their own settings) can use the Intel GPU, and GPU Fusion verifies that too.

## Ways a program *can* end up on the Intel GPU here

- The program offers its own GPU/adapter setting (many games, editors, encoders do). Set it there,
  then use *Verify on this PC*.
- **Untested hypothesis, needs your decision:** connecting the monitor to the motherboard's video
  output and making the integrated GPU the boot/primary display in the BIOS/UEFI would likely make
  default programs use the Intel GPU — at the cost of the GT 730 for them, and with the HD 2000's
  much lower performance. GPU Fusion does not change firmware or cabling and does not claim this works.

## Dual-GPU use (the "Both GPUs" choice)

| Kind | On this PC |
|---|---|
| One rendering workload split across both GPUs | **Not possible.** Direct3D 12 explicit multi-adapter needs D3D12 on both GPUs (the HD 2000 has none); SLI/CrossFire and Vulkan device groups need same-vendor GPUs. |
| One program using each GPU for a different task | Only where the program offers a device choice per task. |
| Two programs on different GPUs | Only programs that pick their GPU themselves (the preference cannot place them). |
| A game on one GPU, a compute job on the other | Yes, when the second program selects the other GPU itself — the Workload Lab does exactly this. |

For a selected program GPU Fusion reads which graphics APIs it links (PE import table: D3D9/11/12,
OpenGL, Vulkan, CUDA, OpenCL) and states the verdict. **No setting can make an existing single-GPU
program use two GPUs**; the Workload Lab shows what is measurably possible with programs written for it.

## Safety

- Writes only `HKCU\…\DirectX\UserGpuPreferences` values for executables the user picks;
  `DirectXUserGlobalSettings`, drivers, firmware and global settings are never touched.
- Every change is confirmed (exact key, old and new data), journalled before writing, read back after.
- Undo restores the previous data and refuses to overwrite a newer external change.
- Verification of a program launches it like the user would; nothing is injected or patched.
- The probe verification changes only GPUFusionProbe.exe's value and restores it (also after a crash).
