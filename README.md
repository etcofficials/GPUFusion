# GPU Fusion

A native Windows desktop application for seeing and managing the graphics adapters in a PC:
which GPUs are present and usable, how busy they are and with what, which GPU an application
prefers, and — experimentally — whether two dissimilar GPUs can usefully share real work.

GPU Fusion reports only what the hardware and drivers actually expose. Metrics that cannot be
read are shown as *Unsupported* or *Unavailable*, never as zero, and no performance gain is
claimed without a benchmark that measured it.

> **Status: Phase 8 of 9 (multi-GPU experiments).** Live monitoring of every physical GPU (including
> WDDM 1.x GPUs Task Manager cannot show), per-application GPU usage, adapter details, the Windows
> per-app GPU preference (set safely and **verified with a probe**), a diagnostics page with an
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
and put `cl`, `cmake` and `ninja` on PATH from a Visual Studio developer prompt).

## Running

```
GPUFusion.exe [--data-dir <path>] [--page <id>] [--smoke-test[=<ms>]] [--screenshot <file>]
```

| Option | Purpose |
|---|---|
| `--data-dir` | Store settings, logs and benchmarks in `<path>` |
| `--page` | Open on `dashboard`, `gpus`, `applications`, `preferences`, `workloads`, `benchmarks`, `diagnostics` or `settings` |
| `--smoke-test` | Start, show the window, exit with code 0 (automated launch check) |
| `--screenshot` | Save a PNG of the window and exit (waits for adapter detection) |
| `--screenshot-full` | With `--screenshot`: capture the page's whole scrollable content |
| `--screenshot-after` | With `--screenshot`: wait this many ms after adapter detection |
| `--verify-gpu-preferences` | Run the GPU preference verification (probe only, restored), save the report, exit |
| `--export-diagnostics <file>` | Write `<file>.md` and `<file>.json` (personal identifiers removed) and exit |
| `--run-benchmarks <preset>` | Run every available benchmark at size `quick`, `standard` or `heavy`, save the session, exit (0 = all completed with correct output) |
| `--run-lab <name>` | Run Workload Lab experiment `split`, `decode` or `all`, save the session(s), print the verdicts to the log, exit (0 = every mode completed with correct output) |
| `--lab-size <preset>` | With `--run-lab`: hash workload size for the split experiment (default `standard`) |

## Where data is stored

| Build | Data folder |
|---|---|
| Development build | `G:\GPUFusion\data` (via the `GPUFusion.datadir` file the build writes next to the exe) |
| Portable release | `data\` next to `GPUFusion.exe` |
| Location not writable | per-user `%LOCALAPPDATA%\GPUFusion` (reported in the status bar and Diagnostics) |

Inside it: `config\settings.json` (versioned), `logs\gpufusion.log` (rotated, size-capped),
`benchmarks\`, `reports\`, and `temp\` (GPU Fusion's own TEMP, so graphics-driver shader caches
stay off the system drive). Nothing is written to the registry, and GPU Fusion installs no
services or startup entries.

## Tests

`ctest --preset release` runs the automated tests: unit tests (`-L unit`) and tests on the real
GPUs, drivers and registry (`-L hardware`). See [docs/TESTING.md](docs/TESTING.md).

## Documentation

- [Architecture](docs/ARCHITECTURE.md) · [Phase plan](docs/PHASE_PLAN.md) · [Testing](docs/TESTING.md)
- [Compatibility](docs/COMPATIBILITY.md) · [Storage policy](docs/STORAGE_POLICY.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)
- [GPU selection](docs/GPU_SELECTION.md) · [Multi-GPU research](docs/MULTI_GPU_RESEARCH.md)

## Known limitations (current)

- Per-process usage for GPUs on drivers older than WDDM 2.0 (here: Intel HD 2000) covers only the
  current user's processes unless GPU Fusion runs as administrator; it never asks for elevation.
- Intel HD Graphics 2000 uses a WDDM 1.2 driver, so Windows performance counters (and Task
  Manager) cannot show it. GPU Fusion reads its utilization and memory through D3DKMT kernel
  statistics (verified against a real load). It exposes no temperature, fan, power or clock sensors.
- GT 730 (Fermi, driver 391.35): temperature, fan and performance state via NVML; power draw and
  clocks are not supported by the driver and are shown as Unsupported.
- Monitoring cost on an i5-2400S: about 0.15 % of the CPU at the default 1-second interval.
- On the reference PC the Windows "Power saving"/"High performance" preference does **not** change
  which GPU programs use (verified with a probe); GPU Fusion says so before you apply one.
- The Workload Lab's gains apply to GPU Fusion's own test workloads. GPU Fusion does not (and cannot,
  without patching other programs) make games or other applications use both GPUs.
- Benchmark results are for this machine's state at the time (other programs, temperature and
  driver all matter). GPU Fusion compares a result only with an earlier run of the same benchmark,
  workload size, GPU model and driver, and calls a change faster/slower only when it exceeds the
  measured run-to-run variation. Measured numbers for the reference PC are in
  [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).
- "Usable" on a GPU card means a Direct3D 11 device was created on it, not that any application
  is using it.
- License not chosen yet.
