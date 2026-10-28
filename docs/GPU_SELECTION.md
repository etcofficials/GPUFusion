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

Caveats for browsers:
- A browser that is already running ignores the switch, because the new window joins the running
  instance. Close every window first (1.4 offers to do this), or give the profile its own
  `--user-data-dir`.
- Chromium pauses rendering for windows that are covered or minimised, or while the desktop is
  locked. Keep the browser window visible while *Verify on this PC* watches, otherwise the result is
  "no measurable GPU work". The live test adds `--disable-features=CalculateNativeWinOcclusion
  --disable-backgrounding-occluded-windows --disable-renderer-backgrounding` for this reason. A run
  without them failed this way once while the desktop was unattended.

Measured end to end through a profile (`--apply-profile msedge.exe --mode integrated
--profile-known-option`, then `--verify-profile`): **"Confirmed: the program's GPU work ran on
Intel(R) HD Graphics"**. Automatic mode: GT 730. No Windows value was written for Edge.

### Programs that are already running (1.4)

Windows applies a per-app preference, and programs read their own GPU option, only when they start.
Many programs (browsers, chat apps) also hand a new launch over to the instance that is already
running. So *Launch* and *Verify on this PC* now check whether the program is running. If it is,
they explain why that matters and offer:

- **Close it and continue:** GPU Fusion sends a close request to the program's top-level windows,
  as if you clicked their close buttons, and waits up to 15 s. It never ends a program by force.
  If the program asks to save something, keeps running in the background, or has no window (tray
  only), GPU Fusion says so and stops.
- **Continue anyway**, or **Cancel**.

On the command line, `--close-running` does the same for `--launch-profile` / `--verify-profile`
(exit code 10 if the program stays open). Without it, a warning is logged. Tested only with GPU
Fusion's own probe (`tst_running_live`: closes on request; one that refuses or has no window is
reported and left running). GPU Fusion never closed any of your programs during development.

### GPU shortcuts (1.5)

A profile's GPU option, launch arguments, SDK variables and running-program check apply only when
GPU Fusion starts the program. **Create shortcut…** (GPU Manager), or `--create-shortcut <exe|id>
[--shortcut-dir <folder>]`, writes a shortcut such as `msedge (Integrated GPU).lnk` with the program's
own icon. By default it goes on the Desktop; in the GUI you choose where. It runs:

```
GPUFusion.exe --launch-profile <profile id> --interactive [--data-dir <dir>]
```

This starts the program through its profile **without opening GPU Fusion's window**, filling in the
GPU placeholders for the mode's GPU at that moment, so new LUIDs after a restart do not matter. If
the program is already running, a small dialog offers *Close it and start* / *Start anyway* /
*Cancel*. Problems such as a removed profile, or a program that does not close, are shown in a
dialog. The shortcut follows later changes to the profile. It stops working if the profile is removed
or GPU Fusion is moved; GPU Fusion's uninstaller does not delete shortcuts you made.

Verified: a shortcut opened through the Windows shell, as Explorer does on a double-click, started
the probe with `--adapter 00000000_0000870F`, the Intel GPU's LUID at that boot
(`tst_running_live`). With the program already running, the dialog waited; dismissing it started
nothing and closed nothing.

### Live check of running programs (1.8)

The Applications page already measures which GPU every process is using. For programs with a GPU
profile it now compares that with the profile, **without launching anything**:

| Column | Meaning |
|---|---|
| ✓ *mode* | working on a GPU its profile (or one of its variants) asks for |
| ✗ *mode* | working on another GPU (or the software renderer); the tooltip says which and why |
| ✓ Both GPUs active · *mode* | (1.12) a Both profile, and the program's **own** work is on two GPUs right now. This does not show that its work was split or that it got faster. |
| One GPU only (*GPU*) · *mode* | (1.12) a Both profile, but the program works on one GPU. Both GPUs were offered; the program does not use the second one, which is the normal case for programs without the SDK. Before 1.12 this case showed ✓. |
| *mode* · idle | no GPU work right now, so nothing to compare |
| Automatic | its profile does not ask for a GPU |

A ✗ usually means the program was started another way than through GPU Fusion, which is how the
program's own GPU option and the SDK apply, or that it was already running when a Windows preference
was applied. *GPU profile…* opens the program in the GPU Manager. Tested with live monitoring
(`tst_applications_ui`): the probe with an Integrated profile, started normally, was flagged ✗ on
the GT 730; started with its own option on the Intel GPU, it showed ✓.

### Variants: several profiles for one program (1.7)

*Add variant…* creates another profile for the same executable, with its own name (e.g. "Intel",
"video encoding"), mode, launch arguments and shortcut. This is capability class B within one
program: for example, one FFmpeg job on each GPU, or two browser windows on different GPUs (each
needs its own `--user-data-dir` to run at the same time).

**Windows stores only one GPU preference per executable**, and it belongs to the program's main
profile. A variant never writes or removes it:
- **Integrated / Discrete:** the variant reaches its GPU through the program's own GPU option (GPU
  placeholder in its arguments, shown as *Expected* until verified) or the SDK. Without either it
  needs the program's support.
- **Automatic:** the program chooses by itself; any Windows value from the main profile still applies.

Verified on the reference PC: variants "Intel" (Integrated) and "NVIDIA" (Discrete) of FFmpeg, both
with FFmpeg's known option. "Intel" was **confirmed on Intel HD Graphics**. "NVIDIA" was **confirmed
on the GT 730** (video engine up to 98 %). No Windows value existed for FFmpeg before or after.
Command line: `--apply-profile <exe> --variant <name> --mode …`, and `--variant` with
`--launch-profile`, `--verify-profile` and `--create-shortcut` when the profile is given as a path.

### Backup and transfer (1.6)

*Export…* writes every profile to one JSON file. *Import…* adds them back on this PC or another.
Profiles that already exist are kept unless you choose to replace them. What is imported: program
path, name, mode, launch arguments (placeholders stay placeholders, valid on any PC) and the SDK
setting. What is not imported: verification results and applied Windows values, because "verified
on this PC" is about that PC's hardware. Importing changes no Windows setting. Programs that are not
installed are imported, listed, and shown as missing until they are. Command line:
`--export-profiles <file>`, `--import-profiles <file> [--replace-existing]`.

### Chromium-based programs detected by their files (1.4)

Programs built on Chromium (browsers, Electron and CEF apps) ship `icudtl.dat` and a V8 snapshot
(`v8_context_snapshot.bin` or `snapshot_blob.bin`). These files sit next to the executable (Electron,
e.g. Discord) or in a version folder beside it (Chrome, Edge, Brave). For such programs GPU Fusion
suggests the Chromium switch `--use-adapter-luid={luid_pair}`. The suggestion is marked *detected,
not verified with this program*, because some apps do not pass command-line switches on to Chromium.
Programs known by name keep their own entry and evidence.

### Other programs checked on the reference PC

| Program | GPU option on its command line | Result |
|---|---|---|
| VLC 3 | none for choosing the Direct3D 11 adapter (help text checked; `--gpu-affinity` is an OpenGL affinity setting) | no suggestion; on this PC it uses the default GPU |
| Discord (Electron) | detected as Chromium-based | not tested: it was running and in use, so it was not closed or relaunched |

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

Since 1.12 the mode is called **Both GPUs (SDK programs)**, and every place that reports on it says
what was actually measured:

- **Launch message:** "Started X (process N). Both GPUs were offered to it; only programs that
  integrate the GPU Fusion SDK use them…". Launches are now logged with their arguments and variables.
- **Verification of a program that used one GPU:** "One GPU only: …", recorded as `one-gpu`, never
  "Confirmed".
- **Live check:** see the table above.

The modes, as the GPU Manager explains them:

| Mode | What happens |
|---|---|
| Automatic | Windows and the program choose the GPU |
| Integrated GPU / Discrete GPU | one GPU for the whole program: Windows' preference where it works here (GT 730 only), otherwise the program's own GPU option or the SDK |
| Both GPUs (SDK programs) | both GPUs are *offered* (`GPUFUSION_ADAPTERS`); only programs that integrate the SDK use them. Others run on one GPU, as in Automatic. |
| Independent job batches | Workload Lab → Job batches / `--dispatch`: separate jobs, each on one GPU |
| Split workload | Workload Lab → Adaptive multi-GPU scheduler, and SDK programs: independent units of one workload over both GPUs, NVIDIA first ([MULTI_GPU_SCHEDULER.md](MULTI_GPU_SCHEDULER.md)) |

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

## Roblox and Minecraft

Investigated after a user saw Roblox at about 57 % 3D on the GT 730 in Both mode, while Intel showed
0 %. Each line says how it is known.

| Question | Answer |
|---|---|
| Does Both mode make Roblox or Minecraft use two GPUs? | **No.** Both passes adapter IDs in environment variables. Neither game reads them, so they render on one GPU as in Automatic: the GT 730, which drives the display here. *Confirmed from the source code and by the user's observation.* |
| Is there a supported way to make either game split its rendering across these GPUs? | **No.** Splitting one game's rendering needs the game's own explicit multi-adapter support and Direct3D 12 on both GPUs; the HD 2000 has none. *Unsupported on this hardware.* |
| Can GPU Fusion add it? | **No.** That would need code injection or patching the game, which GPU Fusion never does. |
| Can Roblox be put on the Intel GPU? | No supported adapter option is known for the Roblox player. Windows' preference cannot select the Intel GPU here: even the minimum-power DXGI query returns the GT 730. *Inferred; GPU Fusion has never verified Roblox in Integrated mode on this PC.* |
| Can Minecraft (Java) be put on the Intel GPU? | Minecraft Java renders with OpenGL (LWJGL in `javaw.exe`). A windowed OpenGL program uses the driver of the GPU that drives its display, the GT 730 here. *Inferred.* |
| Do GPU Fusion's variables even reach them? | Possibly not. Roblox here is an Xbox/Store package (`G:\XboxGames\…`), and Minecraft's `javaw.exe` lives in a package folder (`…\WpSystem\…`) and is normally started by the launcher with many arguments. Launching such a program warns about this since 1.12. |

What *can* use both GPUs here: GPU Fusion's own workloads (Workload Lab), independent job batches,
and programs that integrate the SDK, such as `GPUFusionSplitDemo.exe`.

Every legitimate way games could benefit from both GPUs on this PC is assessed in
[GAME_MULTI_GPU_FEASIBILITY.md](GAME_MULTI_GPU_FEASIBILITY.md). The most promising is measured there:
keep the game alone on the GT 730 and move other programs' 3D work to the Intel GPU.
