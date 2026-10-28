# Changelog

## 1.12.0 — 2026-10-28

Real multi-GPU work distribution, and honest reporting of "Both GPUs". See
[docs/MULTI_GPU_SCHEDULER.md](docs/MULTI_GPU_SCHEDULER.md) for how it works and every measurement.

**Honest "Both GPUs"**

- The mode is now called **Both GPUs (SDK programs)**. It offers both GPUs to programs that
  integrate the SDK; every other program, games included, runs on one GPU as in Automatic.
- **Live check fixed.** In Both mode the Applications page showed ✓ when the program worked on only
  one GPU. It now shows **✓ Both GPUs active** only while the program's own work is on two GPUs, and
  **One GPU only (GPU)** otherwise.
- **Verification** of a Both profile that used one GPU now says "One GPU only: …", recorded as
  `one-gpu`. It never says "Confirmed".
- **Launch message.** "Started X (process N) in mode Both GPUs" became a message that says what was
  passed, and that a started process does not show which GPU it uses. Every launch is logged with
  its arguments and variables.
- **Packaged programs.** Launching a Microsoft Store / Xbox package (WindowsApps, XboxGames,
  WpSystem, or a package manifest next to the program) warns that its GPU option and the SDK
  variables may not reach it.
- The GPU Manager explains the six modes: Automatic, Integrated, Discrete, Both (SDK), Independent
  job batches, Split workload.

**Adaptive multi-GPU scheduler (new)**

- **Workload Lab → Adaptive multi-GPU scheduler**, also `--run-lab adaptive [--lab-policy …]`. The
  fastest GPU (the GT 730) does most of the hash image, and the Intel GPU helps with small batches
  from the other end: only up to its measured share, and never a batch it would finish after the
  GT 730. Three policies: conservative, balanced and performance. The scheduler compares itself
  with the GT 730 alone and switches the Intel GPU off when splitting is not faster.
- **Measured on the reference PC, 10 runs per mode:**
  - standard size: 1.14× faster than the GT 730 alone (63.8 → 55.5 ms median);
  - heavy size: 1.14× (243.4 → 210.7 ms);
  - quick size: 1.06× with the performance policy; the conservative and balanced policies switched
    the Intel GPU off there.
  - The Intel GPU did 12–13 % of the rows. Every image was bit-exact, and the scheduler's own time
    was ≤ 0.02 ms per run.
- **Fallback.** If a helper's batch fails, the GT 730 re-runs its rows and the output stays complete
  and correct (checked in the Lab and in tests).

**SDK 2**

- New `gpufusion/d3d11.h`: capabilities of each offered adapter (feature level, compute shaders,
  memory), and one device per adapter.
- New `gpufusion/scheduler.h`: the scheduler and Balancer above, with per-device units, times,
  transfers and scheduling cost, and exactly-once accounting. It is lock-free and needs only the
  standard library.
- **Versioned variables.** GPU Fusion now sets `GPUFUSION_ENV_VERSION=1`. A program built with SDK 2
  ignores a newer format instead of misreading it.
- **`GPUFusionSplitDemo.exe`** (shipped next to GPUFusion.exe) is an example program that integrates
  the SDK. It measures each GPU alone, then the adaptive split, checks every image and prints a JSON
  report with a verdict. Given a Both profile, GPU Fusion verifies it on both GPUs.

**Lighter**

- GPU waits sleep on a DXGI event instead of spinning. In the demo this cut CPU use from 25 % to
  2.5 % of all cores, at the same speed.
- The Workload Lab's GPU workers reuse their render target for every batch size.
- Lab results now record GPU Fusion's memory (working set, private, peak).

**Tests**

New `tst_splitdemo_live`. Added scheduler and Balancer cases to `tst_sdk` (simulated devices, so
they run on CI), adaptive analysis to `tst_labmodel`, the end-to-end adaptive experiment to
`tst_lab_live`, the adaptive panel to `tst_benchmarks_ui`, and Both-mode live checks and launch
messages to `tst_processtable` and `tst_profiles`.

## 1.11.1 — 2026-10-26

Fixes from an independent code review of everything since 1.1. All ten findings were confirmed and
fixed.

- **Job runner could destroy input files.** It deletes a job's output file before the job runs, and
  with an output name equal to the input name in the same folder (e.g. pattern `{name}`) that was the
  input. Such jobs, and jobs sharing one output file, are now refused before anything runs.
- **Job runner could leave jobs unrun.** The rule that keeps a slow GPU off the last jobs compared
  against GPUs that had already stopped taking jobs. Now the last active GPU always finishes the queue.
- **Automatic backup** no longer writes a backup while a profile file cannot be read. That backup
  would lack the damaged profile, and after enough changes the rotation would delete every backup
  that still had it.
- **Verification:** a program that rendered on the requested GPU stays *confirmed* when the other GPU
  only shows work of unknown type, which may be incidental. Before, such a Discrete profile was
  marked "partly". A *Both GPUs* profile still counts that work as the second GPU.
- **Intel GPU monitoring:** a process whose statistics Windows refuses is now reported as "could not be
  inspected" instead of being counted as idle.
- **GPU Manager:** stays disabled while it waits for a program to close (a hardware re-detect used to
  re-enable the buttons). It no longer re-reads the profile file on every keystroke.
- **Applications page:** re-reads profiles only when they or the GPUs changed, not every 10 s.
- **Placeholders** are expanded in one pass by a single shared implementation, so a file name
  containing `{job}` is no longer expanded twice.

## 1.11.0 — 2026-10-26

- **Automatic profile backup.** In Settings → *GPU profiles*, choose a folder, ideally on another
  drive.
  - After every profile change, GPU Fusion saves all profiles there (the same file format as
    *Export…*) and keeps the newest copies (10 by default).
  - A backup is skipped when nothing changed, and an import makes one backup, not one per profile.
  - The page warns when the folder is on the same drive as GPU Fusion's data, because such a backup
    would not survive that drive failing.
  - It is off by default: nothing is written anywhere until you choose a folder. Saving the setting
    makes a first backup straight away. Restore with *Import…*.
- Settings file schema 3. Older settings files load unchanged, with the backup off.

## 1.10.1 — 2026-10-26

Final hardening.

- Fix: on the Applications page, a program with an **Automatic** profile or variant was marked ✗ when
  another of its profiles asked for a different GPU. GPU Fusion cannot tell which profile started the
  program, and Automatic accepts any GPU, so it is now shown as consistent with that profile. The
  details still name the other profile's request.
- The Applications page's GPU-profile column is computed once per process per refresh instead of for
  every cell on every repaint. Measured with 500 rows, 40 of them profiled: 14.1 ms → 4.4 ms per
  refresh plus full repaint (1.4 ms without profiles).
- **Final report** (`docs/FINAL_REPORT.md`, also in the packages): what was verified on the reference
  PC, what is inferred, which hypotheses need the user's decision, and what was not done and why.
- Clean debug build: the full suite and every page's start/close test pass.

## 1.10.0 — 2026-10-26

Accessibility and keyboard use.

- Every interactive control on every page now has an accessible name (what a screen reader
  announces) and can be reached with Tab. This is checked by a new test that visits all pages of
  the real window: 90 controls.
- Fixes found by that test:
  - **Benchmarks:** the list of benchmarks to run could not be focused, so benchmarks could not be
    chosen without a mouse. Now the arrow keys and Space work. The workload-size selector and the
    results table are named.
  - **Diagnostics:** the four read-only tables can be reached with Tab and are named, so they can be
    read with the keyboard or a screen reader.
  - **GPU Preferences, Workload Lab, GPU Manager:** named tables, fields and selectors. The launch
    arguments field has a keyboard shortcut (Alt+L). The *Add program* picker and the guide viewer
    are named too.

## 1.9.0 — 2026-10-26

Documentation you can use from inside the app.

- **User guide** (`docs/USER_GUIDE.md`), task by task: choose a program's GPU, use its own GPU option,
  start it through a shortcut, check that it works, variants, backup, batches of jobs, and what GPU
  Fusion never does.
- **Open user guide** on the GPU Manager page shows it inside GPU Fusion. No Markdown viewer is
  needed, the contents and the links between documents work, and web links open in your browser.
  The ZIP and the installer now include the guide and the documents it links to (`docs\`, `README.md`).
- **Troubleshooting** gained a GPU Manager section: ✗ in the Applications page, "no measurable GPU
  work", processes that could not be inspected, browsers ignoring their option, programs that do not
  close, shortcuts without a profile, variants, imported profiles.
- The README now gives an overview by page instead of a list of every release's news.

## 1.8.0 — 2026-10-26

- **Live GPU-profile check on the Applications page.** For every running program that has a GPU
  profile, a new column compares the GPU it is using right now with the GPU its profile asks for:
  ✓ (matches), ✗ (working on another GPU), *idle*, or *Automatic*. The tooltip and details panel
  say where it runs and why a mismatch usually happens: it was not started through GPU Fusion, or
  it was already running when the profile was applied. Nothing is launched for this check. Variants
  count too.
- **GPU profile…** button on a running process opens the program in the GPU Manager, creating its
  profile if it has none.
- Applications table columns rebalanced to fit without horizontal scrolling. The GPU column's
  tooltip shows the full adapter name.

## 1.7.0 — 2026-10-26

- **Profile variants: several GPU profiles for one program.** *Add variant…* in the GPU Manager
  (`--variant <name>` on the command line) creates another profile for the same program, with its own
  mode, arguments and shortcut, e.g. "FFmpeg — Intel" and "FFmpeg — NVIDIA".
  - Windows stores only one GPU preference per program, and it belongs to the main profile. A
    variant never writes or removes it.
  - A variant reaches its GPU through the program's own GPU option (placeholders) or the SDK. The
    GPU Manager shows this for each mode.
  - Verified on the reference PC: two FFmpeg variants were confirmed decoding on the Intel GPU and
    on the GT 730 respectively, with no Windows value written for FFmpeg.

## 1.6.0 — 2026-10-26

- **Profile backup and transfer.** *Export…* / *Import…* in the GPU Manager (and `--export-profiles
  <file>`, `--import-profiles <file> [--replace-existing]`) save every GPU profile to one JSON file and
  add them back, on this PC or another.
  - Import keeps profiles that already exist unless you choose to replace them.
  - Import changes no Windows setting: a profile's mode takes effect when you apply it.
  - Verification results and applied Windows values are not imported, because they describe the PC
    they were made on.
  - Programs that are not installed are listed.

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
