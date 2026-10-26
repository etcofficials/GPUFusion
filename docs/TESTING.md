# Testing

## Automated tests

```powershell
. G:\GPUFusion\scripts\env.ps1
ctest --preset release            # or: G:\GPUFusion\scripts\build.ps1
```

Tests are Qt Test executables in `build\<preset>\bin`. CTest runs them with `TEMP`/`TMP` set to
`build\<preset>\test-temp` (nothing written to C:) and Qt's `bin` on PATH.

| Suite | Cases | Covers |
|---|---|---|
| `tst_settings` | 10 | defaults valid; JSON round trip of every field; missing file; **settings survive restart**; per-field fallback for out-of-range / wrong type / unknown choice / non-integer / huge values; **corrupt file moved aside with content preserved**; non-object JSON; **newer schema is read-only and left untouched**; **invalid settings rejected on save**; saved file carries `schemaVersion` |
| `tst_logging` | 5 | level names; line format (timestamp, severity, subsystem, thread, multi-line); **rotation keeps ≤ N files each ≤ max size and drops oldest entries**; reopen appends; process handler level filtering and subsystem tags |
| `tst_apppaths` | 6 | portable default; pointer file (comments, relative paths); command line precedence; **unwritable location falls back with a reason**; standard folder layout |
| `tst_adaptermodel` | 21 | PCI instance-ID parsing; **WARP is software even without the flag**; discrete/integrated/unknown classification incl. UMA, Arc-style and tiny-memory cases; reference-machine merge; **identical cards paired by PCI location**; **ambiguous identical cards left unpaired**; PCI-ID fallback; **duplicate LUIDs reported once**; disabled device kept with problem-code explanation; **stale duplicates hidden**; absent Intel iGPU with firmware hint; Intel / healthy / failed / Basic Display / software findings; HRESULT, problem-code, decoder-GUID text; formatting; PCI sentinel rejection |
| `tst_metricmodel` | 13 | ring buffer bounds/order; **non-OK values never formatted as numbers and graphed as NaN**; a real 0 stays 0; utilization arithmetic (needs two samples, clamp, **counter reset → Unknown, not negative**); busiest-engine rule; segment split; engine names; staleness; history capacity; **history stores gaps as NaN and drops vanished adapters**; software adapters never monitored |
| `tst_monitoring_live` (label `hardware`) | 8 | D3DKMT needs two samples then reports every adapter; **a D3D11 load on each GPU is seen on that GPU and not on the other**; **agreement with Windows PDH counters** (GT 730) and absence of PDH for WDDM 1.x (Intel); NVML states every sensor explicitly; reference-machine sensor matrix; **MonitorService start → pause (no samples) → resume → interval change → shutdown < 3 s**; collection cost < 20 ms |
| `tst_processmodel` | 10 | PDH engine/process instance parsing (real names from this PC); aggregation per process and adapter; unknown adapters ignored; **memory alone is "Memory only", not usage**; first sample Unknown, missing counters Unavailable (not 0); duplicate engine types numbered; Windows preference string parsing (**other keys only = not set; unreadable GpuPreference = unrecognised**) |
| `tst_applications_ui` (label `hardware`, serial) | 1 | real Applications page with live monitoring: the probe with an Integrated profile, **started normally, is flagged ✗ (on the discrete GPU); started with its own option on the integrated GPU, ✓** |
| `tst_processtable` | 9 | **live GPU-profile column: matches / other GPU (with where, in the tooltip) / idle / Automatic / no profile; all rows of a profiled process refresh when its activity moves between adapters**; model consistency (QAbstractItemModelTester); **rows update in place, exited processes disappear, no model reset**; **rows stay consistent inside removal notifications** (regression); **only changed rows are signalled**; one row per adapter; filters (activity, search, PID, adapter); preference shown only when set; non-OK values show their state |
| `tst_process_live` (label `hardware`) | 6 | process list finds this process with path; exited child process disappears; **this process loading each GPU is seen on that GPU through that GPU's source (PDH or D3DKMT)**; **D3DKMT and PDH agree for the same process**; Intel coverage counts add up; PDH pass cost |
| `tst_preferences` | 17 | value editing keeps other keys; only existing absolute .exe; case-insensitive value names; **journal on disk before any registry write** (enforced by the fake store); undo restores / only once; **undo refuses to overwrite an external change** (force works); failed writes change nothing; **unwritable journal blocks the change**; journal bound; probe output parsing; **verification sets each preference, restores the exact original** (absent and with other keys); detects an effect; failed probe runs give no conclusion; **interrupted verification recovered at restart**; failed restore keeps the marker |
| `tst_preferences_live` (labels `hardware`, `registry`) | 4 | probe runs; apply + undo on the real registry; **verification on this PC** (reference expectation: no effect, Power saving → GT 730); snapshot restore |
| `tst_preferences_ui` (labels `hardware`, `registry`) | 2 | drives the real page: Advanced → Power saving, Apply (confirmed) → registry `GpuPreference=1;` **read back**; Cancel changes nothing; Undo ×2 → not set; **adapter choices follow the verification**: with "every preference → discrete GPU", Integrated is marked "cannot enforce" and Apply stays disabled, Both writes nothing and shows the dual-GPU check, Discrete is "Verified" and writes/reads back `GpuPreference=2;` |
| `tst_gpuselection` | 24 | **processes that could not be inspected are named, and "no activity" is qualified**; **unnamed work on the other adapter counts as both**; choice planning: reference PC (Intel not enforceable and **not written**, NVIDIA verified), hybrid laptop (both verified), no verification (nothing written), ranking-only evidence (expected vs not enforceable), Automatic removes the value, Both never writes, ambiguous/missing targets, **verification from before a reboot (new LUIDs) still applies**; usage evaluation: engine classification, confirmed, wrong adapter, unnamed engines (qualified confirmation), **video/copy/other processes are not rendering**, one-pass blips ignored, both adapters + **launcher resolution**, not launched / already running (**relaunch requirement**) / not saved / Automatic; dual-GPU assessment on the reference PC and where hardware allows it; API detection from imports |
| `tst_appverify_live` (labels `hardware`, `registry`) | 5 | real rendering by GPUFusionProbe observed per process: default device **confirmed on GT 730**; probe choosing the Intel GPU itself → work seen on Intel; **probe with `GpuPreference=1;` (power saving) still renders on GT 730 → "Not applied"** (probe's value restored); launcher (`cmd.exe` → probe) traced to the rendering child; PE imports of real executables, invalid paths rejected |
| `tst_profiles` | 21 | **variants: ids (main unchanged, case-insensitive names), JSON, store; a variant never uses, writes or removes the program's Windows value (FakeStore value unchanged in every mode); option in the arguments makes its modes "Expected"**; **export/import round trip; verification results and applied values not imported; existing profiles kept unless replacing; programs not installed reported; invalid and newer documents refused**; **a damaged profile file is skipped with a warning and left in place, without hiding the others**; **shortcut names safe as file names, arguments (with `--data-dir`), quoting, .lnk written and read back (target, arguments with spaces, working folder, the program's icon)**; **Chromium-based programs detected by their files in both install layouts (not by icudtl.dat alone); detected suggestions marked not verified; named entries keep priority**; **GPU placeholders filled in for the mode's GPU (signed LUID high part), left out in Automatic or without such a GPU**; **a GPU option in the arguments makes an unenforceable or unverified mode "Expected", never writes a Windows value**; **known options carry evidence and a placeholder, match case-insensitively, are inserted once, user entries override, invalid ones reported**; profile ids stable and case/separator-insensitive; JSON round trip (incl. verification); **ids derived, never trusted from the file**; newer schema refused; **saving one profile never rewrites another's file**; list/find/remove; **versioned install folders resolve to the newest copy that contains the exe**; reference-PC capabilities (Automatic verified, **Intel "only with the program's support" and nothing written**, Discrete verified + HighPerformance, Both class D with the class-E reason); program's own verification overrides the status; SDK launch environment; argument splitting; **per-mode verdicts** (Both needs both GPUs; Intel unnamed work counts); discovery helpers; **apply writes only justified values, keeps other keys and other programs' values** |
| `tst_profiles_live` (labels `hardware`, `registry`) | 7 | **two variants of FFmpeg (if installed) confirmed on the Intel GPU and the GT 730 through its known option, no Windows value written or removed**; probe as an SDK program, each mode applied and verified on the real GPUs: **Integrated → work only on Intel with no Windows value**, **Discrete → `GpuPreference=2;` + rendered on GT 730**, **Both → both GPUs used**, Automatic → observed, value removed; **a real browser (Edge or Brave) with its known option `--use-adapter-luid={luid_pair}` confirmed on the Intel GPU, and on its default GPU in Automatic** (throwaway profile folder, no Windows value written, its processes closed afterwards); discovery returns existing, non-installer programs (65 here) |
| `tst_running_live` (label `hardware`, serial) | 4 | **a GPU shortcut opened through the Windows shell starts the probe through its profile with the integrated GPU's LUID filled in** (profile and shortcut created with `GPUFusion.exe --apply-profile` / `--create-shortcut`, exit codes checked); probe only: a program with a window **closes on a close request (it reports closing itself)**; one that ignores the request is **reported and left running**; one without a window is reported as such |
| `tst_gpumanager_ui` (labels `hardware`, `registry`) | 3 | **export → profile removed → import through the page restores it (without its verification)**; **a running instance is asked to close before Verify, then a fresh one is verified**; drives the GPU Manager page: add program, Integrated (status, nothing written, verification on a worker thread → confirmed, listed as verified), Discrete (confirmation → value written and read back), declined confirmation changes nothing, Automatic removes the value; **known option from the user's `gpu-options.json` shown and inserted with the button, SDK sharing off, mode "Expected", placeholder stored, probe confirmed on Intel through its arguments alone** |
| `tst_accessibility` (label `hardware`) | 1 | the real main window, every page: **every visible interactive control (90) has an accessible name and is reachable with Tab**; **benchmarks can be chosen with the keyboard only (Space toggles)** |
| `tst_guide` | 1 | in-app user guide: documents found (repository `docs\` in development builds, `docs\` next to the exe in packages), Markdown rendered, **every linked document exists and opens, every in-page link matches a heading, a contents link scrolls to its section** |
| `tst_sdk` | 3 | SDK: mode and LUIDs from the environment (empty items, unknown mode); **adapters resolved against the real DXGI factory, case-insensitive, default when none match**; weighted split covers every item once |
| `tst_dispatch` | 10 | GPU selection specs; jobs, output names and **all placeholders**; **command split before expansion (paths with spaces stay one argument)**; **dynamic queue gives the faster worker more jobs**; **straggler rule** keeps a much slower GPU off the last jobs, never blocks two slots of one GPU; cancellation; **command executor with any program** (cmd.exe): output SHA-1, exit codes, **GPUFUSION_ADAPTERS per job**; comparison analysis: significant gain only against the concurrency control, **inconsistent outputs and failed jobs → not comparable**, single runs → no significance claim |
| `tst_dispatch_live` (label `hardware`) | 1 | built-in decode jobs on each real GPU (hardware decoding, same frame count) and on all GPUs |
| `tst_jobpanel_ui` (label `hardware`) | 1 | drives the Workload Lab's job panel: a run once, then a comparison including the concurrency control (here honestly **"Slower"** for 4 short clips) |
| `tst_diagnostics` | 11 | reference-PC issues (legacy WDDM warning with action, sensors notes, no WARP noise, severity order); no-GPU / detection errors; NVML disabled vs missing → right fix; preference verdicts; storage/settings warnings; **multi-GPU assessment** (D3D11 split feasible; compute, D3D12, memory sharing, OpenCL, SLI not feasible — naming the adapter that blocks it; common codecs); single GPU; **metric matrix never invents values**; Markdown/JSON structure; log parsing (continuations, max lines); **redaction removes profile path / computer name / COMPUTER\user but keeps ordinary words containing the user name ("device", "dev.") and other paths** (fictional identifiers) |
| `tst_export_live` (label `hardware`) | 1 | runs `GPUFusion.exe --export-diagnostics` headless; Markdown + JSON written with live metrics; **no COMPUTERNAME or USERPROFILE in either file**; reference PC content |
| `tst_benchmarkmodel` | 11 | statistics (sample SD, median, CV); summary marks a result wrong if any run was; **comparable keys separate different sizes, drivers and test versions**; **baseline = newest earlier valid comparable result, never a later one**; **significance rule** (faster/slower only beyond 2 SE and 1 %; noise → no change); **cross-target summary compares only identical verified work**; CSV quoting and column count; JSON round trip keeps Unsupported states; retention; **hash kernel pinned to values computed independently** (arbitrary-precision arithmetic); threading/bands do not change the result |
| `tst_workload_live` (label `hardware`) | 4 | **pixel-shader hash is bit-exact with the CPU on every GPU** (whole image and a band starting at row 100); transfer returns the data sent; a short real session completes, is correct and is saved (JSON + CSV); cancellation |
| `tst_benchmarks_ui` (label `hardware`) | 2 | drives the real Benchmarks page twice: **monitoring paused during the run and resumed after**, Run re-enabled, session listed and shown, second run compared with the first; drives the **Workload Lab page**: split experiment runs, the other experiment is blocked meanwhile, monitoring paused/resumed, every mode "Correct", verdict shown |
| `tst_labmodel` | 6 | row ranges cover every row once (alignment, odd heights, zero/negative shares); balanced shares and ideal time; difference wording; **split analysis**: fastest GPU is the reference (never the CPU), helped/did-not-help verdicts, ideal and efficiency, wrong-output and failed modes excluded with findings, **P-state findings only for GPUs that worked in that mode**; **decode analysis**: two-stream reference, one-stream rows not compared with it, per-GPU stream times |
| `tst_lab_live` (label `hardware`) | 6 | **split image bit-exact with the CPU reference in every mode** (static, lopsided, shared queue, with CPU; odd height); test clip created and reused; **each GPU decodes in hardware (frames are textures on that GPU) and every frame is identical to the software decoder's**; two decoders at once; **both experiments end to end** (all modes complete, correct, ≥ 1 s warm-up, engine evidence, progress reaches the end); cancellation |
| `tst_hardware_live` (label `hardware`) | 6 | real detection: no global errors; **software never counted as GPU**; **no duplicate LUIDs or instance IDs**; every hardware adapter probed; second detection reuses probes; reference-machine expectations (opt-in) |

Run only the hardware-independent tests with `ctest --preset release -L unit`; the hardware suite
with `-L hardware`. Set `GPUFUSION_EXPECT_REFERENCE_MACHINE=1` on the development PC to also check
its known adapters (GT 730 FL 11_0 + D3D12; HD 2000 FL 10_1, no compute, no D3D12, WDDM 1.2).

### Latest result

2026-10-26, release build 1.10.0: **34 CTest entries (33 suites + registry restore fixture),
234/234 cases passed**, reference-machine expectations enabled, 0 skipped.

2026-10-26, release build 1.9.0: **33 CTest entries (32 suites + registry restore fixture),
233/233 cases passed**, reference-machine expectations enabled, 0 skipped.

2026-10-26, release build 1.8.0: **32 CTest entries (31 suites + registry restore fixture),
232/232 cases passed**, reference-machine expectations enabled, 0 skipped; no probe, browser or
FFmpeg processes left running.

2026-10-26, release build 1.7.0: **31 CTest entries, 230/230 cases passed**, reference-machine
expectations enabled, 0 skipped; no probe, browser or FFmpeg processes left running.

2026-10-26, release build 1.6.0: **31 CTest entries, 227/227 cases passed**, reference-machine
expectations enabled, 0 skipped.

2026-10-26, 1.5.1: release build **31 CTest entries, 226/226 cases passed** (reference-machine
expectations, 0 skipped). Clean **debug** build: all 31 entries pass. The one debug-only failure was
the test expecting `Qt6Core.dll` where debug builds use `Qt6Cored.dll`, now fixed. Every page passes
the start/close smoke test in the debug build.

2026-10-26, release build 1.5.0: **31 CTest entries, 225/225 cases passed**, reference-machine
expectations enabled, 0 skipped; no probe or browser processes left running.

2026-10-26, release build 1.4.0: **31 CTest entries (30 suites + registry restore fixture),
223/223 cases passed**, reference-machine expectations enabled, 0 skipped; no probe or browser
processes left running. On the way, `tst_profiles_live`'s browser case failed once with "no
measurable GPU work" while the desktop was unattended: Chromium pauses rendering for occluded
windows. The test now passes Chromium's switches against that and has passed three runs in a row.

2026-10-26, release build 1.3.0: **30 CTest entries, 218/218 cases passed**, reference-machine
expectations enabled, 0 skipped; no browser processes left behind by the live browser test.

2026-10-26, release build 1.2.0: **29 suites (30 CTest entries incl. the registry cleanup fixture),
212/212 cases passed**, reference-machine expectations enabled, 0 skipped. The probe's registry value
was unset afterwards. Fixed on the way: `tst_lab_live` required a "busiest engines" note even for the
CPU-only modes. Whether a GPU engine passes 1 % during those depends on unrelated desktop activity, so
the test failed whenever the desktop was idle. CPU-only modes now accept "no engine busier than 1 %".
An earlier intermittent failure of this suite had a different symptom (no output at all after about
17 s). This finding does not explain it, and that failure remains unexplained.

2026-10-16, release build: **20/20 suites (21 CTest entries incl. the registry cleanup fixture),
156/156 cases passed**, reference-machine expectations enabled, 0 skipped. Two headless
`--run-benchmarks standard` sessions and three `--run-lab` runs (2 × all, 1 × split heavy): exit
code 0 (every mode completed, every output correct). Phase 9: the same 21 entries also pass in the
**debug** build (debug heap and assertions) after fixing three use-after-free bugs in test code that
only the debug build exposed. Every page also passed a
start/close smoke test (`--page=<id> --smoke-test`), which destroys all pages.

### Building on the development PC

Always build with `scripts\build.ps1`. Because the development disk is failing (TROUBLESHOOTING.md),
it (1) deletes and rebuilds intermediate files named in corruption errors (LNK1104/LNK1136/LNK1285/C2471,
damaged dependency-scan files) and (2) after every successful build compares each object file with
the real timestamps of all headers it was compiled from and rebuilds any that are older — Ninja's own
records on that disk missed a header change once, which produced heap corruption at run time. Each
rebuilt object is printed with the dependency that made it stale.

### Tests that touch the registry

`tst_preferences_live`, `tst_preferences_ui`, `tst_appverify_live`, `tst_profiles_live` and
`tst_gpumanager_ui` write the per-user GPU preference **only for this build's GPUFusionProbe.exe**. Before changing it they save its original value to
`%TEMP%\gpufusion-preferences-live-snapshot.json`; the CTest cleanup fixture
`tst_preferences_live_restore` puts it back even if a test crashes. Every run on the reference PC
compared the whole `UserGpuPreferences` key before and after: identical. Exclude them with
`ctest -LE registry`.

GUI tests (`tst_processtable`) run with `QT_QPA_PLATFORM=offscreen` and `QT_PLUGIN_PATH` pointing at
the Qt installation's plugins (the test executables share `bin` with GPUFusion.exe, whose deployed
plugins folder only contains the `windows` platform plugin).

## Monitoring overhead (Phase 3)

Method: launch with `--page=dashboard --smoke-test=77000` and a throwaway `--data-dir`; read the
process's CPU time with `Get-Process` at 15 s and 75 s (60 s window); the app also logs its own
collection timing once a minute. Release build, i5-2400S (4 logical CPUs), Dashboard visible.

| Configuration | CPU share of machine | Collection per sample |
|---|---|---|
| Monitoring paused | not measurable (< one 15.6 ms tick in 60 s) | — |
| 1 s interval — before the LiveText fix | 1.02 % (41 ms CPU/s) | 0.38 ms |
| 1 s interval — final | **0.15 %** (6 ms CPU/s) | 0.41 ms avg, 1.1 ms max |
| 250 ms interval — after fix | 0.83 % (33 ms CPU/s) | 0.45 ms avg |

How the regression was found: per-thread CPU showed all time on the UI thread; timing every
UI-thread event by type showed `LayoutRequest` at ~31–36 ms/s (≈ 9/s) versus ~6.6 ms/s for painting.
After `LiveText` replaced per-sample QLabel updates, steady-state LayoutRequests went to zero.

## Launch checks (application)

Run against `build\release\bin\GPUFusion.exe`:

| Check | Command | Result (2026-10-16) |
|---|---|---|
| Starts with no dev environment (PATH = `C:\Windows\System32;C:\Windows`, working dir `C:\Windows`) | `GPUFusion.exe --smoke-test=2000` | exit 0, "Smoke test passed" logged |
| Settings persist across launches | two consecutive launches | 1st: "not found, defaults in use" + file written; 2nd: "loaded" |
| Light theme from settings | `--data-dir=<tmp> --screenshot=…` with `"theme":"light"` | rendered light |
| Corrupt settings file | `{ not json` in `config\settings.json` | moved to `settings.json.corrupt-<time>`, warning in log, banner on Settings page, Save enabled ("not saved yet") |
| Each page renders | `--page=<id> --screenshot=…` | dashboard, settings, diagnostics inspected |
| GPUs page, whole content | `--page=gpus --screenshot=… --screenshot-full` | 3 cards (GT 730, HD 2000, WARP) inspected (Phase 2) |
| Detection off the UI thread | log thread ids | detection on worker thread, UI thread separate |
| Device-change handling | post `WM_DEVICECHANGE / DBT_DEVNODES_CHANGED` ×3 to the GPU Fusion window only | one re-detection after debounce, 11 ms, probes reused |
| Driver cache off C: | launch from a shell with TEMP on C: | 0 C: files changed; NV cache in `data\temp` |
| Dashboard renders live data (dark + light) | `--page=dashboard --screenshot=… --screenshot-after=20000` | tiles, sensors line, graphs with clean ticks, engine bars inspected (Phase 3) |
| Diagnostics monitoring card | `--page=diagnostics --screenshot-full` | both providers active, NVML unsupported list, cost line |
| Applications page with a third-party Intel load | `temp\probe\gpuload.exe 1 25` running, `--page=applications --screenshot-after=8000` | gpuload.exe top row on Intel HD Graphics, 100 %, Engine 0, 72 MB shared, source D3DKMT; auto-selected with details (Phase 4) |
| Applications page cost | 60 s window, 1 s interval | 0.97 % before / **0.42 %** after the changed-rows fix |

## Hardware-dependent tests

`tst_hardware_live` asserts only invariants that hold on any Windows PC and `QSKIP`s when no
physical GPU is available to DirectX; machine-specific checks are opt-in (see above).

Cannot be automated safely, so **not tested**: a real driver reset (TDR), driver upgrade while
running, or physical removal of an adapter. These use the same device-change path verified above.

## Manual checks still to do

- Interactive Save/Revert/Restore defaults and live theme switch (logic covered by unit tests;
  the button wiring has not been exercised by an automated UI test).
- Double-click launch from Explorer (equivalent launch verified via `Start-Process`; Explorer
  launch is part of the Phase 9 release checklist).
