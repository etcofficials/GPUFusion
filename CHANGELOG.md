# Changelog

## 1.5.1 — 2026-10-26

Hardening of the 1.2–1.5 work.

- Fix: a Windows per-app value holding only other keys, such as Windows' own windowed-game
  optimisation `SwapEffectUpgradeEnable=1;`, was shown as "Unrecognised value". It is now "Not set",
  because no GPU preference is stored. A `GpuPreference` entry that cannot be read is still reported
  as unrecognised.
- GPU profile files that cannot be read (for example, damaged by a failing disk) are now logged and
  left in place, instead of being skipped silently.
- Verified in a clean **debug build**: the full test suite and every page's start/close smoke test
  pass, with no assertions or debug-heap errors.
- Build script: recovers from `C1051` (obsolete `.pdb`) and truncated generated `.moc` files.

## 1.5.0 — 2026-10-26

- **GPU shortcuts:** *Create shortcut…* in the GPU Manager (and `--create-shortcut <exe|id>
  [--shortcut-dir]`) writes a shortcut that starts the program through its profile, without opening
  GPU Fusion's window. It applies the profile's GPU option and arguments (placeholders filled in for
  the current boot) and the SDK variables, and asks what to do if the program is already running.
  It uses the program's own icon and goes where you choose (Desktop by default).
- `--interactive` for `--launch-profile`: problems and the running-program question appear in dialogs.
- Fix: the headless command-line modes reported early exits with the wrong exit code (e.g. "did not
  close", 10, came back as 2).

## 1.4.0 — 2026-10-26

- **Programs that are already running:** *Launch* and *Verify on this PC* now detect a running
  instance. They explain that it keeps its current GPU (and may take over the new launch), and offer
  to close it first. Closing works like clicking the program's close button and is never forced.
  Programs that refuse, or run without a window, are reported and left alone. On the command line:
  `--close-running` (exit code 10 if the program stays open).
- **Chromium-based programs detected by their files** (`icudtl.dat` + V8 snapshot, next to the exe or
  in a version folder). The verified Chromium GPU switch is suggested for any of them, including
  Electron apps, and marked "not verified with this program".
- GPUFusionProbe `--window <s>` / `--ignore-close`: a test stand-in for a program with a window.

## 1.3.0 — 2026-10-26

Real programs on the GPU you choose, through their own GPU option. Verified with Microsoft Edge,
Brave and FFmpeg on the Intel GPU, where Windows itself cannot place programs.

- **GPU placeholders in profile launch arguments:** `{luid}`, `{luid_pair}`, `{luid_high}`,
  `{luid_low}`, `{dxgi_index}`, `{vendor_id}`, `{vendor_hex}`, `{gpu_name}` are filled in at every
  launch for the GPU of the profile's mode. In Automatic mode those arguments are left out. Profiles
  keep the placeholder, so they survive new LUIDs after a restart. A mode Windows cannot enforce shows
  as *Expected* with such an option, and as *Verified for this program* after verification.
- **Known GPU options** (optional suggestions with their evidence, plus your own in
  `config\gpu-options.json`):
  - Chromium-based browsers: `--use-adapter-luid={luid_pair}`, verified with Edge and Brave.
  - FFmpeg: d3d11va device index, verified.
  
  The GPU Manager shows the suggestion with a *Use this option* button. On the command line, use
  `--profile-known-option` with `--apply-profile`.
- **Verification fix:** work by sandboxed processes (e.g. a browser's GPU process) on pre-WDDM 2.0
  GPUs was missed, because the process could not be opened with full query rights. It is now read
  with limited rights. Processes that still cannot be inspected are named, and "no activity" is no
  longer claimed in that case.
- Dispatcher command templates accept `{luid_pair}`, `{luid_high}` and `{luid_low}`.

## 1.2.0 — 2026-10-25

GPU Fusion becomes a generic GPU manager: a GPU profile for any program, the strongest justified
mechanism per mode, verification of real use, an optional SDK and a generic multi-GPU job dispatcher.
Nothing in it is specific to one application.

- **GPU Manager page** (`--page manager`): one saved profile per executable, added from the programs
  installed or running on this PC (Start menu, installed programs, running processes) or by browsing.
  Self-updating programs in versioned folders are followed to their newest version.
- **Modes per profile** — Automatic, Integrated GPU, Discrete GPU, Both GPUs — each with its status
  for this program on this PC (verified / expected / only with the program's support / not possible),
  capability class (A–E), mechanism and reasons. Apply asks before any Windows value changes. Values
  are written only where they do what the mode says; otherwise any stored value is removed. Launch
  arguments are kept per profile.
- **Launch and Verify on this PC** from the profile: the program is started with its arguments and
  the GPU choice (`GPUFUSION_MODE`, `GPUFUSION_ADAPTERS`, `GPUFUSION_PROFILE`), observed, and judged
  against its mode. For Both, work on both GPUs counts (including the Intel GPU's unnamed engines),
  with an explicit note that it does not show a speed-up. Results are stored in the profile.
- **SDK** (`sdk/include/gpufusion/gpufusion.h`, header-only, see docs/SDK.md): lets a program use the
  GPU(s) chosen in its profile, even the Intel GPU where Windows cannot place programs. Verified with
  GPUFusionProbe in every mode.
- **Job dispatcher** (Workload Lab page, `--dispatch`): independent jobs, one per input file, spread
  over the GPUs through a shared queue. Jobs can be any command template (`{dxgi_index}`, `{luid}`,
  `{input}`, `{output}`, …) or built-in H.264 decoding. *Compare* measures each GPU alone, the fastest
  GPU running the same number of jobs at once (control), and all GPUs. It checks that outputs match,
  and reports faster/slower only when the difference is significant. Slower GPUs stay off the last jobs.
  On the reference PC, gains for decode batches are at most about 10–15 % and vary between sessions
  (docs/MULTI_GPU_RESEARCH.md).
- CLI: `--apply-profile <exe> --mode <m> [--profile-args]`, `--launch-profile`, `--verify-profile`,
  `--dispatch <template|decode>` with `--dispatch-inputs/-output/-pattern/-gpus/-compare`.
- GPUFusionProbe follows `GPUFUSION_ADAPTERS` (SDK) and renders on every requested GPU in Both mode.
- The GPU Preferences page, NVIDIA selection, detection, monitoring, benchmarks and the Lab are unchanged.

## 1.1.0 — 2026-10-20

Per-application GPU selection that says what it can and cannot do — and checks it.

- **Choices by adapter:** Automatic, the integrated GPU, the discrete GPU, or Both, each with its
  status on this PC (verified / expected / cannot be enforced / unknown) and the evidence. A Windows
  value is written only where the probe verification shows it selects that adapter; the raw values
  stay available under *Advanced*. Saved values are read back.
- **Verify on this PC** (and `--verify-app <exe> [--verify-seconds N] [-- args]`): starts the program,
  follows its process tree and reports, in four stages, whether the requested GPU actually did the
  rendering — counting only 3D/compute engines of the program's own processes. Launchers are detected
  and the rendering executable is offered instead.
- **Dual-GPU check** for a program: which kinds of multi-GPU use are possible on this PC, from the
  hardware and the graphics APIs the program links.
- **Diagnosis on this PC:** Windows ranks the GT 730 first for every preference (boot device, drives the
  only display, no hybrid classification, Intel WDDM 1.2 driver), so "power saving" cannot move programs
  to the Intel GPU — confirmed with real rendering. The GPUs page now shows Windows' kernel adapter
  classification and which adapter drives the primary display.
- GPUFusionProbe `--render <s>` and `--adapter <LUID>`.
- Fixes: GPU choices match saved verification results by model, not LUID (LUIDs change at every boot).
- Build: stops when a source file changed on disk without being edited (failing-disk detector);
  recovers truncated generated headers.

## 1.0.1 — 2026-10-17

Packaging and licensing only — the application is unchanged from 1.0.0.

- GPU Fusion is now licensed under the MIT License; the ZIP and the installer include `LICENSE.txt`.
- `SHA256SUMS.txt` uses LF line endings so `sha256sum -c` works on every platform.

## 1.0.0 — 2026-10-16

First release. Built and verified on Windows 10 22H2 with an NVIDIA GeForce GT 730 (Fermi, driver
391.35) and an Intel HD Graphics 2000 (driver 9.17.10.4459, WDDM 1.2).

### Features
- **GPUs:** every adapter Windows exposes (DXGI + Device Manager), drivers, WDDM version, Direct3D
  11/12 feature levels tested by creating devices, video decoder profiles; software adapters (WARP)
  separated, never counted as GPUs; plain-language explanations of failures.
- **Dashboard:** live utilization, per-engine load and memory for every physical GPU via D3DKMT —
  including WDDM 1.x GPUs that Task Manager cannot show — plus temperature, fan and performance
  state through NVML where the driver offers them. Unsupported metrics are labelled, never shown as 0.
- **Applications:** which processes use which GPU (Windows GPU counters for WDDM 2.x, D3DKMT per
  process for WDDM 1.x), with each program's Windows GPU preference.
- **GPU Preferences:** set or undo the Windows per-app graphics preference with a journal written
  before every change, refusal to overwrite external changes, and a probe that **verifies which GPU
  programs really get** (on the reference PC: the preference has no effect).
- **Diagnostics:** issues with suggested actions, adapter/API and metric-availability tables, multi-GPU
  compatibility assessment, exportable Markdown/JSON report with personal identifiers removed.
- **Benchmarks:** 11 benchmarks (detection, monitoring overhead, CPU/GPU hash workload, transfers,
  application launch); warm-up + repeated runs, mean/SD/range, bit-exact output checks, JSON + CSV
  history, comparisons only with equivalent earlier runs and only beyond measured noise.
- **Workload Lab:** measured multi-GPU experiments — a compute workload split between both GPUs
  (1.14–1.19× faster than the GT 730 alone when split by measured speed) and parallel H.264 decoding
  (one stream per GPU is 2.2× slower than both on the Intel GPU, whose decoder is 4.5× faster).
- Command line: `--page`, `--data-dir`, `--smoke-test`, `--screenshot`, `--verify-gpu-preferences`,
  `--export-diagnostics`, `--run-benchmarks`, `--run-lab`.

### Packaging
- Portable ZIP (no installation; data kept next to the executable), per-user installer (no
  administrator rights), SHA-256 checksums.

### Known limitations
- Per-process usage on WDDM 1.x GPUs covers only the current user's processes unless GPU Fusion runs
  as administrator (it never asks for elevation).
- The Workload Lab's gains apply to GPU Fusion's own test workloads; other applications are not
  changed to use both GPUs.
