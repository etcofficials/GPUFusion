# GPU Fusion

A native Windows desktop application for seeing and managing the graphics adapters in a PC:
which GPUs are present and usable, how busy they are and with what, **which GPU each program should
use** (a GPU profile for any executable, with verification of what it really uses), and —
measured, never assumed — whether two dissimilar GPUs can usefully share real work.

GPU Fusion reports only what the hardware and drivers actually expose. Metrics that cannot be
read are shown as *Unsupported* or *Unavailable*, never as zero, and no performance gain is
claimed without a benchmark that measured it.

> **Status: version 1.4.0.** New in 1.4: when a program is already running, *Launch* and *Verify*
> offer to close it gracefully first (a running program keeps its GPU). Chromium-based programs,
> including Electron apps, are recognised by their files.
>
> New in 1.3: profiles can pass the chosen GPU to a program through its
> **own GPU option**, using placeholders such as `{luid_pair}` and `{dxgi_index}` in the launch
> arguments. There are known-option suggestions with their evidence. **Verified: Microsoft Edge, Brave
> and FFmpeg run on the Intel GPU this way**, where Windows' own preference cannot move programs.
>
> Version 1.2 made it a generic GPU manager. The **GPU Manager** page keeps a GPU profile for
> any program (picked from installed/running programs or browsed to): Automatic, Integrated GPU,
> Discrete GPU or Both GPUs. For each mode it shows what that mode can do for this program on this PC,
> applies the strongest mechanism that is justified, and verifies on the real GPUs which GPU(s) the
> program used. An optional header-only [SDK](docs/SDK.md) lets programs follow the choice, including
> where Windows cannot. A generic **job dispatcher** runs batches of independent jobs (any command
> line with GPU placeholders) across the GPUs and measures whether that beats the best single GPU.
> No application-specific code, injection or patching.
>
> Since 1.1.0: live monitoring of every physical GPU (including
> WDDM 1.x GPUs Task Manager cannot show), per-application GPU usage, adapter details, **per-application
> GPU selection by adapter** (Intel / NVIDIA / automatic) that writes Windows' per-app preference only
> where it is shown to work and **verifies where a program really renders** (see
> [docs/GPU_SELECTION.md](docs/GPU_SELECTION.md)), a diagnostics page with an
> exportable, privacy-safe report, a benchmark framework (warm-up + repeated runs, **output checked
> bit-exactly**, JSON + CSV, comparisons only between equivalent runs), and a **Workload Lab** that
> measures whether both GPUs together beat the best single one. On the reference PC: splitting a
> compute workload by measured speed is **1.14–1.19× faster** than the GT 730 alone; decoding one video
> stream per GPU is **2.2× slower** than decoding both on the Intel GPU — see
> [docs/MULTI_GPU_RESEARCH.md](docs/MULTI_GPU_RESEARCH.md) and [docs/PHASE_PLAN.md](docs/PHASE_PLAN.md).

## Supported systems

- Windows 10 (22H2 tested) 64-bit. Windows 11 is expected to work but is untested.
- Development and testing hardware: Intel Core i5-2400S, 12 GB RAM, NVIDIA GeForce GT 730
  (GF108 Fermi, driver 391.35) and Intel HD Graphics 2000 (driver 9.17.10.4459).
  Verified capabilities are recorded in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Download and install

From the [Releases](https://github.com/etcofficials/GPUFusion/releases) page:

| File | Use |
|---|---|
| `GPUFusion-Windows-x64.zip` | **Portable.** Extract anywhere and run `GPUFusion\GPUFusion.exe`. Settings, logs and results are kept in a `data` folder next to it. |
| `GPUFusion-Setup.exe` | **Installer**, per user, no administrator rights: installs to `%LOCALAPPDATA%\Programs\GPU Fusion`, adds a Start-menu entry (desktop shortcut optional) and an uninstaller. |
| `SHA256SUMS.txt` | Checksums: `Get-FileHash -Algorithm SHA256 <file>` must match. |

GPU Fusion installs no services or drivers, needs no internet connection and changes no system
settings by itself. The only setting it can change is a program's Windows GPU preference, and only
when you apply one on the GPU Manager or GPU Preferences page (confirmed, journalled, undoable).

## Building from source

Requirements: MSVC 2022 toolset (14.3x), Windows 10 SDK, CMake ≥ 3.28, Ninja, Qt 6.5+ (msvc2022_64)
with the Core, Gui, Widgets and Test modules plus `windeployqt`.

On the reference machine everything lives on G: and is activated per session:

```powershell
. G:\GPUFusion\scripts\env.ps1          # session-only: compiler, CMake, Ninja, Qt, TEMP on G:
G:\GPUFusion\scripts\build.ps1           # configure + build + run tests (release preset)
```

or step by step:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The result, `build\release\bin\GPUFusion.exe`, runs directly from Explorer: the Qt runtime and
the MSVC runtime are deployed next to it during the build.

On another machine, adjust `scripts\env.ps1` (or set `GPUFUSION_ENV=1`, `GPUFUSION_QT_PREFIX`
and put `cl`, `cmake` and `ninja` on PATH from a Visual Studio developer prompt — this is what the
GitHub Actions workflow in `.github/workflows/build.yml` does).

Release artifacts (portable folder, launcher, ZIP, installer, checksums — each verified by launching
it) are produced by `scripts\package.ps1`; see [docs/RELEASE.md](docs/RELEASE.md).
`Launch GPU Fusion.bat` in the repository root starts the packaged build (or the development build).

## Running

```
GPUFusion.exe [--data-dir <path>] [--page <id>] [--smoke-test[=<ms>]] [--screenshot <file>]
```

| Option | Purpose |
|---|---|
| `--data-dir` | Store settings, logs and benchmarks in `<path>` |
| `--page` | Open on `dashboard`, `gpus`, `applications`, `manager`, `preferences`, `workloads`, `benchmarks`, `diagnostics` or `settings` |
| `--smoke-test` | Start, show the window, exit with code 0 (automated launch check) |
| `--screenshot` | Save a PNG of the window and exit (waits for adapter detection) |
| `--screenshot-full` | With `--screenshot`: capture the page's whole scrollable content |
| `--screenshot-after` | With `--screenshot`: wait this many ms after adapter detection |
| `--verify-gpu-preferences` | Run the GPU preference verification (probe only, restored), save the report, exit |
| `--export-diagnostics <file>` | Write `<file>.md` and `<file>.json` (personal identifiers removed) and exit |
| `--run-benchmarks <preset>` | Run every available benchmark at size `quick`, `standard` or `heavy`, save the session, exit (0 = all completed with correct output) |
| `--run-lab <name>` | Run Workload Lab experiment `split`, `decode` or `all`, save the session(s), print the verdicts to the log, exit (0 = every mode completed with correct output) |
| `--lab-size <preset>` | With `--run-lab`: hash workload size for the split experiment (default `standard`) |
| `--verify-app <exe>` | Start the program (arguments after `--`), watch which GPU its processes render on, log the staged result, exit (0 = confirmed / observed) |
| `--verify-seconds <n>` | With `--verify-app` / `--verify-profile`: how long to watch (default 20) |
| `--apply-profile <exe> --mode <m>` | Create/update the GPU profile of any program (`automatic`, `integrated`, `discrete`, `both`; `--profile-args` for launch arguments, which may contain GPU placeholders such as `{luid_pair}` / `{dxgi_index}`; `--profile-known-option` adds the program's known GPU option), apply it, exit |
| `--launch-profile <exe\|id>` | Start the program from its profile (arguments + GPU choice for SDK programs) |
| `--close-running` | With `--launch-profile` / `--verify-profile`: if the program is running, ask it to close first (never forced; exit code 10 if it stays open) |
| `--verify-profile <exe\|id>` | Start it from its profile, verify which GPU(s) it used, store the result (0 = mode confirmed) |
| `--dispatch <template\|decode>` | Run one job per input over the GPUs; `--dispatch-inputs <folder\|a;b>`, `--dispatch-output`, `--dispatch-pattern {stem}.ext`, `--dispatch-gpus all\|integrated\|discrete\|0,1`, `--dispatch-compare <n>` (measured comparison) |

Example (FFmpeg hardware decoding, one file per GPU job):

```
GPUFusion.exe --dispatch "ffmpeg -y -init_hw_device d3d11va=g:{dxgi_index} -hwaccel d3d11va -hwaccel_device g -i {input} -f framemd5 {output}" ^
              --dispatch-inputs D:\clips --dispatch-output D:\out --dispatch-pattern {stem}.md5 --dispatch-compare 3
```

## Where data is stored

| Build | Data folder |
|---|---|
| Development build | `G:\GPUFusion\data` (via the `GPUFusion.datadir` file the build writes next to the exe) |
| Portable release | `data\` next to `GPUFusion.exe` |
| Location not writable | per-user `%LOCALAPPDATA%\GPUFusion` (reported in the status bar and Diagnostics) |

Inside it: `config\settings.json` (versioned), `config\profiles\<id>.json` (one GPU profile per
program), `logs\gpufusion.log` (rotated, size-capped),
`benchmarks\`, `reports\`, and `temp\` (GPU Fusion's own TEMP, so graphics-driver shader caches
stay off the system drive). Nothing is written to the registry, and GPU Fusion installs no
services or startup entries.

## Tests

`ctest --preset release` runs the automated tests: unit tests (`-L unit`) and tests on the real
GPUs, drivers and registry (`-L hardware`). See [docs/TESTING.md](docs/TESTING.md).

## Documentation

- [Architecture](docs/ARCHITECTURE.md) · [Phase plan](docs/PHASE_PLAN.md) · [Testing](docs/TESTING.md)
- [Compatibility](docs/COMPATIBILITY.md) · [Storage policy](docs/STORAGE_POLICY.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)
- [GPU selection and the GPU Manager](docs/GPU_SELECTION.md) · [SDK](docs/SDK.md) · [Multi-GPU research](docs/MULTI_GPU_RESEARCH.md)

## Known limitations (current)

- Per-process usage for GPUs on drivers older than WDDM 2.0 (here: Intel HD 2000) covers only the
  current user's processes unless GPU Fusion runs as administrator; it never asks for elevation.
- Intel HD Graphics 2000 uses a WDDM 1.2 driver, so Windows performance counters (and Task
  Manager) cannot show it. GPU Fusion reads its utilization and memory through D3DKMT kernel
  statistics (verified against a real load). It exposes no temperature, fan, power or clock sensors.
- GT 730 (Fermi, driver 391.35): temperature, fan and performance state via NVML; power draw and
  clocks are not supported by the driver and are shown as Unsupported.
- Monitoring cost on an i5-2400S: about 0.15 % of the CPU at the default 1-second interval.
- On the reference PC Windows cannot move programs to the Intel GPU: it ranks the GT 730 (boot device,
  drives the only display; the Intel driver is WDDM 1.2) first for every preference. Verified with real
  rendering — a program set to "Power saving" still renders on the GT 730. GPU Fusion therefore marks
  the Intel mode "only with the program's support". Programs with their own GPU option can use it when
  launched from their profile (verified: Edge, Brave, FFmpeg), as can SDK programs and dispatcher jobs.
  A browser that is already running ignores the option.
- No setting can make an existing program use both GPUs; "Both GPUs" works only for programs that
  divide their own work (SDK). Both GPUs showing activity does not mean the program got faster.
- The job dispatcher measured at most about 10–15 % gain for decode batches here (not stable between
  sessions); a second, much slower GPU can also make a batch slower. It reports what it measures.
- The Workload Lab's gains apply to GPU Fusion's own test workloads. GPU Fusion does not (and cannot,
  without patching other programs) make games or other applications use both GPUs.
- Benchmark results are for this machine's state at the time (other programs, temperature and
  driver all matter). GPU Fusion compares a result only with an earlier run of the same benchmark,
  workload size, GPU model and driver, and calls a change faster/slower only when it exceeds the
  measured run-to-run variation. Measured numbers for the reference PC are in
  [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).
- "Usable" on a GPU card means a Direct3D 11 device was created on it, not that any application
  is using it.

## License

GPU Fusion is released under the [MIT License](LICENSE). Third-party components keep their own
licenses: Qt 6 (LGPL v3, used as unmodified shared libraries) and the Microsoft Visual C++ runtime
(redistributable) — see [third_party/THIRD-PARTY-NOTICES.txt](third_party/THIRD-PARTY-NOTICES.txt).
