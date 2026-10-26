# Architecture

## Layers

```
GPUFusion.exe (src/main.cpp)
   │  command line, data location, logging, settings load, theme, main window
   ▼
gpufusion_ui  (src/ui, Qt Widgets)           ── pages, reusable widgets, theme
   │  depends on
   ▼
gpufusion_monitoring (src/monitoring + platform/windows providers) ── live metrics
   │
   ▼
gpufusion_hardware (src/hardware, src/platform/windows) ── adapter model + Windows probes
   │
   ▼
gpufusion_core (src/core, Qt Core only)      ── paths, logging, settings, ring buffer — unit-tested
   ▲
   └─ later phases add: processes/, preferences/, workloads/, benchmarks/, diagnostics/
```

UI code never talks to Windows graphics APIs directly. From Phase 2 on, hardware and monitoring
code lives in non-UI libraries behind interfaces (`IAdapterEnumerator`, `IMetricProvider`,
`IPreferenceProvider`, `IWorkload`) and runs off the UI thread; pages receive immutable
snapshots through queued signals.

Headers live next to their sources under `src/` (an application, not a library, so there is no
separate public `include/` tree).

## Implemented modules (Phase 1)

| Module | Files | Responsibility |
|---|---|---|
| Data location | `core/AppPaths.*` | Resolve the data folder: `--data-dir` → `GPUFusion.datadir` pointer file → portable `data\` → per-user fallback. Verifies it is writable and creates `logs`, `benchmarks`, `reports`, `config`. |
| Logging | `core/Logging.*` | `RotatingFileLog` (size-capped, N files), process-wide Qt message handler. Line format: `ISO-timestamp LEVEL [subsystem] (thread) message`. Subsystems are Qt logging categories `gpufusion.<name>`. Warnings and errors are flushed immediately. |
| Settings | `core/Settings.*` | `Settings` struct with defaults and ranges; JSON schema v1; per-field validation on load; corrupt files moved aside; newer-schema files read-only; atomic save via `QSaveFile`. |
| App context | `ui/AppContext.*` | Owns current settings, broadcasts `settingsChanged`. |
| Main window | `ui/MainWindow.*` | Sidebar navigation, page header, stacked pages, status bar, window geometry persistence. |
| Pages | `ui/pages/*` | `SettingsPage` (functional), `DiagnosticsPage` (application/storage info), `PlaceholderPage` (explicit "not implemented, planned for Phase N"). |
| Theme | `ui/Theme.*` | Fusion style, dark/light palettes, small stylesheet, semantic colours for custom painting. |

## Hardware detection (Phase 2)

```
HardwareService (UI thread, owns latest DetectionResult)
   │ refresh(force) / deviceConfigurationChanged() [debounced 1.5 s]
   ▼  one worker QThread per detection, never two at once (requests coalesce)
detectAdapters(previous)
   ├─ win::enumerateDxgi(previous)      DXGI EnumAdapters1, D3DKMT (WDDM version, PCI address),
   │                                    EnumOutputs, D3D11 + D3D12 probe devices (reused when
   │                                    LUID + driver version are unchanged)
   ├─ win::enumerateDisplayDevices()    SetupAPI display class incl. non-present devices,
   │                                    CM status/problem code, driver version/date/provider/INF
   ├─ mergeAdapters()                   pure: LUID de-dup, pair by PCI location → full PCI IDs →
   │                                    vendor/device (only if unambiguous), hide stale duplicates
   └─ findingsFor()                     pure: plain-language warnings and explanations
   ▼ queued back to the UI thread → detectionFinished(result) → GpusPage rebuilds AdapterCards
```

| File | Role |
|---|---|
| `hardware/AdapterModel.*` | Data types and all decision logic (classification, merging, findings, HRESULT / problem-code / decoder-GUID text). No graphics API calls; unit-tested. |
| `platform/windows/DxgiProbe.*` | DXGI/Direct3D/D3DKMT queries. `d3d12.dll` is loaded at run time so a missing runtime cannot stop the app starting. |
| `platform/windows/PnpDevices.*` | Read-only SetupAPI/CfgMgr enumeration. |
| `hardware/AdapterDetector.*` | Orchestrates one detection and logs a summary line per adapter. |
| `hardware/HardwareService.*` | Async ownership, coalescing, debounce, probe reuse. |
| `ui/widgets/AdapterCard.*`, `ui/pages/GpusPage.*` | Presentation only. |

Rules encoded in the model:
- WARP (`DXGI_ADAPTER_FLAG_SOFTWARE` or `1414:008C`) is classified *Software*, never counted as a GPU, and gets no device probes.
- Integrated vs discrete: D3D12 UMA → integrated; Intel with ≤ 512 MB dedicated → integrated; > 512 MB dedicated → discrete; otherwise *Unknown* (stated, not guessed).
- A DXGI adapter is paired with a Device Manager entry only when the match is unique; identical cards without a PCI location stay unpaired rather than being guessed.
- Non-present Device Manager records of hardware that is present under another instance are hidden as stale duplicates and counted.
- "Usable" means a Direct3D 11 device was created — not that any application uses the adapter.

`MainWindow::nativeEvent` forwards `WM_DEVICECHANGE / DBT_DEVNODES_CHANGED` (sent on device add/remove/enable/disable and driver restarts) to `HardwareService::deviceConfigurationChanged()`.

## Live monitoring (Phase 3)

```
MonitorService (UI thread)  — history (ring buffers), freshness, pause/resume, settings
   ▲ snapshotReady(Snapshot)          │ setTargets / start / stop / setInterval / setNvmlEnabled
   │ (queued)                          ▼ (queued)
MonitorWorker ("gpufusion-monitor" thread, low priority, QTimer)
   ├─ win::KmtStatsProvider   D3DKMTQueryStatistics: per-engine running time → utilization,
   │                          segment residency → dedicated / shared memory (all WDDM adapters)
   └─ win::NvmlProvider       nvml.dll loaded at run time: temperature, fan, P-state,
                              power/clocks (marked Unsupported once if NVML says so)
```

| Decision | Reason (measured) |
|---|---|
| D3DKMT is the primary source for utilization and memory | It is the only interface that covers the WDDM 1.2 Intel driver; on the GT 730 it agrees with Windows' PDH counters (99 % vs 99.4 % under load; 445 MB vs 445 MB). |
| Utilization = busiest engine | Task Manager's definition, so the numbers are comparable. |
| PDH not used for adapter readings | Covers only WDDM 2.0+ (no Intel instances at all). Kept for per-process attribution (Phase 4). |
| NVML optional, run-time loaded, opt-out in Settings | No SDK or hard dependency; GT 730/R391 exposes temperature, fan, P-state only. |
| Every reading is a `MetricValue` with a state | Ok / Unsupported / Unavailable / Unknown; non-Ok values format as their state and graph as gaps (NaN), never as 0. |
| `LiveText` for per-sample text | QLabel::setText invalidates layouts; per-sample QLabel updates cost ~35 ms UI CPU per second (profiled: ~9 LayoutRequests/s). LiveText only repaints. |

Engine names come from `KMTQAITYPE_NODEMETADATA` (WDDM 2.0+); the Intel driver does not provide
them, so its engines are shown as "Engine 0–5". The worker resets utilization baselines on pause so
the first reading after resuming is not an average over the pause. A provider that can no longer
query an adapter (removal, driver reset) marks its metrics Unavailable and triggers hardware
re-detection; the history of a vanished LUID is dropped.

History capacity = `historySeconds / interval + 1` points per series (max 14 401), four series per
adapter; changing the interval clears history so the time axis stays truthful.

## Per-process attribution (Phase 4)

```
ApplicationsPage (visible)  ── MonitorService::setProcessTracking(true)  [reference-counted]
                                   │
MonitorWorker::collectProcesses()  (same monitor thread; every max(2 s, interval); on demand via Refresh)
   ├─ win::ProcessList              Toolhelp snapshot; image path + creation time cached per (pid, creation)
   ├─ win::PdhGpuProcessProvider    WDDM 2.0+ adapters (GT 730, WARP): "GPU Engine" + "GPU Process Memory"
   │                                per pid/luid/engine; one PDH query kept open while tracking
   └─ win::KmtProcessProvider       WDDM 1.x adapters (Intel HD 2000): D3DKMT PROCESS_NODE /
                                    PROCESS_SEGMENT, needs PROCESS_QUERY_INFORMATION per process
   → ProcessSnapshot (processes, coverage counts, ratesReady) → ProcessTableModel (in-place row updates)
```

| Decision | Evidence |
|---|---|
| Source chosen per adapter by driver model | PDH has no instances for the WDDM 1.2 Intel adapter; D3DKMT per-process works there (a separate load process read 100 %). |
| PDH for WDDM 2.0+ | Covers every process without opening it, including dwm.exe and csrss.exe, which a standard user cannot open. Agrees with D3DKMT for the same process: 99 % vs 99 %, 64 MB vs 64 MB. |
| D3DKMT needs `PROCESS_QUERY_INFORMATION` | `QUERY_LIMITED` returns STATUS_INVALID_PARAMETER; with the full right, STATUS_INVALID_PARAMETER on the first engine means "no context on this adapter", not an error. |
| Coverage is reported, elevation is not requested | ~64 of ~170 processes are inspectable on the Intel path without admin; the page states this. |
| "Using a GPU" = engine time in the latest sample | Holding memory alone is shown as "Memory only", never as usage. |
| WARP rows | Activity on the Basic Render Driver means the process renders on the CPU; shown as "(software, CPU)". |
| `ratesReady` on snapshots | The first sample after start/target change has no rates; UI behaviour that depends on ordering waits for real rates. |
| Rows signalled only when their shown values change | Signalling all ~170 rows each refresh cost ~56 ms of UI time; Applications page cost 0.97 % → 0.42 % CPU. |

The Windows per-app GPU preference is **read** (HKCU `UserGpuPreferences`, never written in this
phase) by `win::readUserGpuPreferences()` and shown per process.

## GPU preferences (Phase 5)

```
PreferencesPage ──► PreferenceService (UI thread; owns everything below; startup: recoverPending)
                      ├─ PreferenceManager ── apply / rollback ──► ChangeJournal (JSON, written first)
                      │                                         └► IPreferenceStore
                      │                                              └ win::RegistryPreferenceStore (HKCU, per-app REG_SZ only)
                      └─ verifyPreferences (worker thread)  ── set → win::runProbe(GPUFusionProbe.exe) → parse → … → restore
```

`preferences/PreferenceModel.*` is pure (value editing, journal, manager, verification) and is tested
with an in-memory store. The page disables edits while a verification runs (it shares the store and
journal). `tools/probe` builds `GPUFusionProbe.exe` (Win32 + DXGI/D3D11 only, static CRT). The probe
inherits GPU Fusion's TEMP (driver cache stays on the data drive); programs started with "Launch"
get the user's original TEMP back.

## Diagnostics (Phase 6)

```
ui::collectReportInputs(AppContext)  — snapshot of existing state (no probing):
    detection, latest monitor snapshot + providers, process coverage, preference verification,
    data location / settings warnings, OpenCL check (read-only), recent WARN/ERROR log lines
        │
diag::buildReport(inputs)  — pure: issues (+ actions), multi-GPU assessment, tables
        ├─ DiagnosticsPage (cards + tables from Report::adapterTable/metricTable/…)
        └─ ui::exportReport → Redactor → <name>.md + <name>.json
```

The page and the export render the same `Report`, so they cannot disagree. The page rebuilds when
shown and when detection, monitoring providers, preferences or settings change — not per sample.

## Workloads and benchmarks (Phase 7)

```
work::hashPixel / hashRowsCpu        — reference workload (pure C++; values pinned by tests)
work::D3D11HashDevice                — same function as an SM 4.0 pixel shader (fxc → header at build)
    run(params, firstRow, rows)        renders rows [firstRow, firstRow+rows) into an R32_UINT target,
                                       in bands, timed with timestamp queries; optional read-back
    transfer(bytes)                    upload + read-back, data verified
        │
bench::runBenchmarks(specs, options)  — worker thread: warm-up + measured runs, correctness per run,
                                        CPU % (process times), GPU busy % + memory (D3DKMT)
        │
bench::BenchmarkService               — saves sessions\<id>.json + results.csv, retention, history
        ├─ BenchmarksPage              (pauses MonitorService while running)
        └─ --run-benchmarks <preset>
bench model (pure): Stats, comparableKey (test + version + params + GPU model/driver),
    findBaseline (earlier sessions only), compare (2·SE and ≥ 1 %), sessionSummary
```

Why a pixel shader: the Intel HD 2000 is FL 10_1 without compute-shader support, so a compute
shader would exclude it. Integer-only arithmetic makes GPU output bit-exactly checkable.

## Workload Lab (Phase 8)

```
work::SplitPlan (pure)       staticRanges · balancedShares · idealSplitMs
work::runSplit               one thread + D3D11HashDevice per GPU (+ CPU threads); static ranges or a
                             shared queue of row items; computeInto() writes each item straight into
                             the image in CPU memory; common start via std::latch
work::VideoDecoder           Media Foundation Source Reader → NV12; software, or DXVA through an
                             IMFDXGIDeviceManager on a D3D11 device created on the chosen adapter;
                             decodeAll(hashFrames) — frame hashes for the exactness check
work::ensureTestClip         generated once with the software H.264 encoder (Sink Writer)
        │
bench::runSplitExperiment / runDecodeExperiment   → Session (ordinary BenchmarkResults)
    Loop: warm-up ≥ configured runs AND ≥ 1 s · measured runs · D3DKMT busy engines · NVML P-state
        │
BenchmarkService("<benchmarks>\lab").startJob(...)  — same storage, CSV and retention as benchmarks
        ├─ WorkloadLabPage   (bench::analyzeSplit / analyzeDecode → verdict, findings, BarChart, table)
        └─ --run-lab split|decode|all [--lab-size]
```

The reference for every verdict is the best *single* device for the same work (fastest GPU alone
for compute; fastest device decoding both streams for video), compared with `compareStats`.

## Per-application GPU selection (1.1)

```
hw detection ── KernelAdapterType (KMTQAITYPE_ADAPTERTYPE), primary display per output
pref::planChoice(choice, gpus, verification)    pure: Automatic/Integrated/Discrete/Both → value to write
                                                 (only if justified) + Enforcement + evidence
pref::assessDualGpu(exe, imports, system)       pure: the four kinds of multi-GPU use, for this program
pref::win::readImports(exe)                      PE import table (graphics APIs a program links)
mon::observeUsage(targets, {exe, args, seconds}) launch (user's environment) → follow the process tree
                                                 (parent PID + creation time) → per-process engine
                                                 samples per adapter (PDH for WDDM 2.x, D3DKMT for 1.x)
mon::evaluateUsage(evidence)                     pure: 4 stages (saved, launched, activity, adapter
                                                 confirmed), outcome, rendering executables
ui::evaluateForApp                               stored value → requested adapter, then evaluateUsage
   ├─ PreferencesPage: adapter choices, Apply (journalled, read back), "Verify on this PC" (worker thread)
   └─ --verify-app <exe> [--verify-seconds N] [-- args]
GPUFusionProbe --render <s> [--adapter <LUID>]   stand-in application for live tests
```

## Generic GPU manager (1.2)

```
profiles/AppProfile        pure: AppProfile (any executable; id = basename + SHA-1 of the normalised path),
                           JSON (schemaVersion 1), GpuMode, assessMode → ModeCapability {status, class A–E,
                           mechanism, reasons, Windows value to write (only if justified)}, requestedAdapterLuids
profiles/ProfileStore      one JSON file per program (QSaveFile, atomic); resolveCurrentExecutable follows
                           versioned install folders (app-1.0.9265\) to the newest copy
profiles/AppDiscovery      read-only: Start-menu .lnk targets (IShellLinkW, no Resolve), uninstall registry
                           DisplayIcon, running processes; installers/updaters filtered out
profiles/ProfileActions    capabilityInputs (GPUs, probe verification, dual-GPU assessment from imports),
                           applyProfile (journalled PreferenceManager, read back; unjustified → value removed),
                           launchEnvironment (GPUFUSION_MODE/ADAPTERS/PROFILE), launchProfile,
                           verifyProfile → observeUsage (+args, +env) → evaluateProfileEvidence (per mode)
   ├─ ui/pages/GpuManagerPage   list, discovery picker, per-mode status, apply/launch/verify (worker thread)
   └─ --apply-profile <exe> --mode <m> [--profile-args], --launch-profile, --verify-profile
sdk/include/gpufusion/gpufusion.h   header-only SDK: mode(), requestedLuids(), adapters(factory), split()
dispatch/JobDispatcher     class C: workersFrom(detection), jobsFor(inputs, outDir, pattern), runJobs (shared
                           queue, one thread per worker slot, straggler rule), commandExecutor (template
                           placeholders + GPUFUSION_* per job, output SHA-1), decodeExecutor (MF on the
                           job's GPU), compareConfigurations (each GPU, fastest GPU with equal concurrency,
                           all GPUs) + analyze (failures, output identity per GPU, compareStats)
   ├─ ui/widgets/JobBatchPanel  on the Workload Lab page (worker thread, cancel)
   └─ --dispatch <template|decode> --dispatch-inputs/-output/-pattern/-gpus/-compare
```

Library graph: `gpufusion_profiles` → monitoring + preferences; `gpufusion_dispatch` → benchmarks
(→ workloads, monitoring); `gpufusion_ui` links both. Nothing in these modules names a specific
application.

## Threading

- UI thread: widgets, `AppContext`, `HardwareService` state.
- Detection worker: a short-lived `QThread` per detection (low priority). It touches no QObjects; the result is posted back with a queued call bound to the service, so it is dropped if the service is gone. The service destructor waits for a running detection (probes cannot be interrupted; ~0.2 s).
- Monitor worker: one long-lived `QThread` ("gpufusion-monitor", low priority). `MonitorWorker`
  lives there and owns the providers (NVML is initialised, used and shut down on that thread).
  Communication is by queued signals/`invokeMethod` only. Shutdown: `MonitorService`'s destructor
  calls `shutdown()` with a blocking queued call, then quits and joins the thread (< 3 s, tested).
- Benchmark worker: a `QThread` per session ("gpufusion-benchmark"). The session is handed to the
  service when the thread has finished (so `isRunning()` is false when `finished` is emitted).
  Cancel is checked between runs; the service destructor cancels and waits for the run in progress
  (at most one run — a few seconds for the heaviest CPU run).
- Workload Lab: the experiment runs on the lab service's worker thread; the split creates one
  short-lived `std::thread` per worker per run (each GPU's device is used by one thread at a time);
  the decode experiment uses one thread per stream, each with its own COM (MTA) initialisation.
  Media Foundation is started once per process.
- GPU Manager verification: a `QThread` per verification; the profile is copied in, and the result
  is merged into the latest saved profile on the UI thread. Job dispatcher: the panel's `QThread`
  runs the batch; `runJobs` starts one `std::thread` per worker slot and joins them; the queue,
  results and timings are guarded by one mutex; built-in decode jobs initialise COM per thread.
- Logging is mutex-protected and safe from any thread.

## Build and deployment

- CMake + Ninja presets (`debug`, `release`), MSVC with `/W4 /permissive- /WX`.
- `cmake/Deploy.cmake`: post-build `windeployqt` (platform, styles, ico/png image plugins only) and
  app-local MSVC runtime (`cmake/DeployCrt.cmake`), so `build/<preset>/bin` runs without any
  environment setup.
- `resources/app.rc.in`: icon (`IDI_ICON1`, also used by Qt as the window icon) and version info
  generated from `project(VERSION)`. `src/core/Version.h.in` exposes the same version to code.
