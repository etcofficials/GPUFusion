# GPU selection per application

**Status:** adapter-oriented choices (Automatic / integrated GPU / discrete GPU / Both), mapped onto
Windows' per-app preference only where verification justifies it, plus **verification of where a
program really renders** (launch → observe its process tree → 3D/compute engine activity per GPU).
Measured on the reference PC 2026-10-20. Version 1.2 adds the **GPU Manager**: saved per-program
profiles for any executable (below).

## GPU Manager: a profile for any program (1.2)

The GPU Manager page (and `--apply-profile` / `--launch-profile` / `--verify-profile`) keeps one
profile per executable in `data\config\profiles\<id>.json`. There is no built-in program list and no
per-program code. Programs are added from the Start menu, the installed-programs list, running
processes or by browsing. Self-updating programs that install each version in its own folder
(`app-1.0.9265\`) are followed to their newest version.

Each profile has a mode, optional launch arguments, the value applied and the last verification. For
every mode the page shows what it can do **for this program on this PC**:

| Mode | Class | Mechanism (strongest generic one that is justified) | Reference PC |
|---|---|---|---|
| Automatic | A | remove the program's Windows preference | Verified |
| Integrated GPU | A | Windows preference *only if verified to work*; otherwise no value, and the GPU is passed to the program when GPU Fusion launches it: through its own option (GPU placeholder in the arguments, 1.3) or the SDK | **Only with the program's support**: Windows cannot place programs on the Intel GPU here. Verified on Intel: Microsoft Edge and Brave (their own option), FFmpeg, and an SDK program. |
| Discrete GPU | A | Windows preference `GpuPreference=2;` | Verified |
| Both GPUs | D | no Windows value; both GPUs passed to the program (SDK) | Only with the program's support. The SDK probe was verified using both. |

Class E (one rendering workload split across GPUs) is listed with its reason in the Both reasons and
is impossible here. Class B (different GPUs for different programs) is simply one profile per
program. Class C (independent jobs on several GPUs) is the dispatcher on the Workload Lab page. A
successful verification of the program itself overrides the generic status ("Verified for this
program"). Applying never writes a value that would not do what the mode says: an unjustified
mode *removes* any stored value, so the value cannot push the program the other way.

Verification of a profile launches the program with its arguments and the SDK variables, observes
its process tree as described below, and evaluates it against the mode. For Both, work on both GPUs
counts, including work of unknown type on the Intel GPU, whose driver does not name engines. That is
reported as "Both GPUs used" with the explicit note that it does not show a speed-up. See
[SDK.md](SDK.md) for the variables and the verified results.

## The program's own GPU option (1.3)

Many programs have their own command-line option for choosing a GPU. A profile's launch arguments can
contain **GPU placeholders**. GPU Fusion fills them in at every launch for the GPU of the profile's
mode: Integrated means the integrated GPU, Discrete and Both mean the discrete GPU.

| Placeholder | Example (Intel HD 2000 here) |
|---|---|
| `{luid}` | `00000000_0000870F` |
| `{luid_pair}` | `0,34575` (decimal HighPart,LowPart: the form Chromium expects) |
| `{luid_high}` / `{luid_low}` | `0` / `34575` |
| `{dxgi_index}` | `1` |
| `{vendor_id}` / `{vendor_hex}` | `32902` / `0x8086` |
| `{gpu_name}` | `Intel(R) HD Graphics` |

In Automatic mode, arguments containing a placeholder are left out, so the program decides by
itself. The profile stores the placeholder, not the value, so it stays valid when Windows assigns
new LUIDs at the next boot. A mode that Windows cannot enforce becomes **Expected** when the
arguments carry such an option. It becomes **Verified for this program** once *Verify on this PC*
confirms it. This only applies when the program is started from GPU Fusion.

**Known options** are optional suggestions that fill in the option for a program GPU Fusion
recognises by executable name. Each one states its evidence. You can add your own in
`data\config\gpu-options.json`:

```json
[ { "executables": ["tool.exe"], "arguments": "--gpu {dxgi_index}", "note": "from the tool's manual" } ]
```

| Program | Option | Evidence on the reference PC (2026-10-26) |
|---|---|---|
| Microsoft Edge, Brave | `--use-adapter-luid={luid_pair}` | **Verified:** with the Intel LUID, WebGL rendered by ANGLE on Intel HD Graphics (renderer string `ANGLE (Intel, Intel(R) HD Graphics … D3D11)`), and GPU Fusion's verification confirmed work only on Intel. Without the switch, with the GT 730's LUID, or with an invalid LUID: GT 730. |
| Chrome, Vivaldi, Opera, Chromium | same switch | not verified here (same Chromium code) |
| FFmpeg | `-init_hw_device d3d11va=gf:{dxgi_index} -hwaccel d3d11va -hwaccel_device gf` | **Verified:** index 1 → decoding work on Intel; index 0 → GT 730 video engine; identical frame hashes |

Caveat for browsers: a browser that is already running ignores the switch, because the new window
joins the running instance. Close every window first, or give the profile its own `--user-data-dir`.

Measured end to end through a profile (`--apply-profile msedge.exe --mode integrated
--profile-known-option`, then `--verify-profile`): **"Confirmed: the program's GPU work ran on
Intel(R) HD Graphics"**. Automatic mode: GT 730. No Windows value was written for Edge.

### Verification fix found on the way

Edge's sandboxed GPU process cannot be opened with `PROCESS_QUERY_INFORMATION`, so the D3DKMT
per-process statistics for the Intel (WDDM 1.2) GPU missed its work, and the verdict said "no GPU
activity". GPU Fusion now retries with `PROCESS_QUERY_LIMITED_INFORMATION`, which works, and then
sees the work. Processes that still cannot be opened are named in the result, and "no activity" is
qualified instead of claimed.

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
- The program has a command-line GPU option and is launched from its profile with a GPU placeholder
  (verified with Microsoft Edge, Brave and FFmpeg; see above).
- The program integrates the GPU Fusion SDK and is launched from its profile (verified with the probe).
- For batch work: the program accepts an adapter option on its command line. The dispatcher's command
  template fills in `{dxgi_index}` / `{luid}` per job (verified with FFmpeg `-init_hw_device d3d11va=g:{dxgi_index}`).
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
