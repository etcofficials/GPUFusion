# Phase plan

| Phase | Scope | Status |
|---|---|---|
| 0 | Environment audit, feasibility probes, architecture | **Done** (2026-10-16) — `data/audit/PHASE0_AUDIT.md` |
| 1 | Project skeleton: CMake, logging, settings, main window, build + launch from G: | **Done** (2026-10-16) |
| 2 | Hardware detection: DXGI + PnP/driver info, WARP separation, init diagnostics | **Done** (2026-10-16) |
| 3 | Live monitoring: D3DKMT + NVML providers, worker thread, history graphs, overhead measurement | **Done** (2026-10-16) |
| 4 | Application monitoring: per-process GPU attribution | **Done** (2026-10-16) |
| 5 | GPU preferences: per-app Windows preference with backup/rollback; verify actual routing | **Done** (2026-10-16) |
| 6 | Diagnostics report and export | **Done** (2026-10-16) |
| 7 | Benchmark framework | **Done** (2026-10-16) |
| 8 | Multi-GPU experiments (D3D11 tiled pixel-shader work, dual video decode) | **Done** (2026-10-16) |
| 9 | Hardening, packaging (portable ZIP, installer, shortcuts), GitHub release | Next |

## Phase 1 — delivered

- **Implemented:** CMake/Ninja presets on the portable G: toolchain; session environment script;
  `gpufusion_core` (data location, rotating structured log, versioned validated settings);
  `gpufusion_ui` (dark/light theme, main window with eight sections, functional Settings page,
  Diagnostics page with application/storage info, explicit "planned" pages for unbuilt
  sections); icon and Windows version resource; post-build deployment of Qt and MSVC runtime.
- **Built:** release preset, warnings as errors.
- **Tested:** 21 unit tests (3 suites) pass; launch checks listed in TESTING.md.
- **Not in Phase 1:** any GPU functionality.

## Phase 2 — delivered

- **Implemented:** adapter model (classification, WARP separation, DXGI ↔ Device Manager pairing,
  stale-duplicate suppression, findings, HRESULT/problem-code/decoder explanations); DXGI +
  D3D11/D3D12 + D3DKMT probe; SetupAPI enumeration including disabled/absent devices; async
  `HardwareService` with coalescing, debounced device-change re-detection and probe reuse;
  GPUs page with one card per adapter.
- **Tested:** 21 adapter-model unit tests; 6 live hardware tests (reference-machine expectations
  enabled); device-change debounce verified by posting `DBT_DEVNODES_CHANGED` to the app window.
- **Storage fix:** creating Direct3D devices made the NVIDIA driver write a ~20 KB shader cache to
  `%TEMP%` on C:. GPU Fusion now sets its own process TEMP/TMP to `<data>\temp`; verified 0 files
  changed on C: during a launch from a shell whose TEMP is on C:.
- **Verified on this hardware:** see COMPATIBILITY.md → Application.
- **Not tested:** behaviour during a real driver reset (TDR) or physical adapter removal — these
  cannot be triggered safely. The code path is the same device-change re-detection; a removed
  adapter disappears from DXGI and its Device Manager entry becomes "Not detected".
- **Not in Phase 2:** live metrics (Phase 3); the Dashboard is still a placeholder.

## Phase 3 — delivered

- **Implemented:** `MetricValue` with explicit states; D3DKMT provider (engine utilization,
  dedicated/shared memory, engine names where the driver gives them); run-time-loaded NVML
  provider; monitor worker thread with overhead accounting; `MonitorService` with bounded history,
  staleness, pause/resume, live settings; Dashboard (tiles, sensors line, utilization and memory
  history graphs with hover, per-engine bars, sources); Diagnostics "Monitoring sources" card;
  settings schema 2 (`useNvml`).
- **Performance fix found by profiling:** per-sample `QLabel` updates caused layout passes costing
  ~35 ms/s; replaced by `LiveText` → 1.02 % → 0.15 % CPU at 1 s.
- **Tested:** 13 metric-model unit tests, 8 live monitoring tests (load on each GPU seen on that GPU
  only; agreement with PDH; NVML support matrix; service pause/resume/interval/shutdown; cost),
  1 new settings test (schema 1 → 2).
- **Not in Phase 3:** per-process figures (Phase 4); clocks/power on this hardware (unsupported by
  the driver — documented, not faked).

## Phase 4 — delivered

- **Feasibility measured first** (`data/audit/phase4-procprobe.txt`): D3DKMT per-process works on the
  WDDM 1.2 Intel driver with `PROCESS_QUERY_INFORMATION`; a standard user can open ~64 of ~170
  processes; PDH covers all processes but only WDDM 2.0+ adapters.
- **Implemented:** process list with path/creation-time cache; PDH and D3DKMT per-process providers
  chosen per adapter by driver model; WARP rows (software rendering); read-only Windows GPU
  preference reader; Applications page (search, GPU filter, "include processes without GPU use",
  Refresh, per-row activity, details panel with per-engine figures, Open file location, coverage
  note); sampling only while the page is open.
- **Bugs found and fixed:** table model read stale row→process indices during removal
  notifications (crashed under the model tester; regression test added); tests sharing the app's
  `bin` folder could not find the offscreen platform plugin (QT_PLUGIN_PATH for tests); auto-select
  picked a row from a priming sample (`ratesReady`); signalling every row each refresh (0.97 % →
  0.42 % CPU).
- **Tested:** 8 process-model, 8 table-model and 6 live per-process tests, plus a service test that
  tracking stops when released.
- **Deferred to Phase 5:** "launch an application with a chosen GPU preference" (it belongs with the
  preference verification).

## Phase 5 — delivered

- **Answer, with evidence:** on this PC the Windows per-app preference does not change the GPU — the
  probe got the GT 730 under all four settings (see GPU_SELECTION.md).
- **Implemented:** GPUFusionProbe.exe (no Qt, static CRT); preference model (edit only the
  GpuPreference key, preserve others); registry store limited to per-app REG_SZ values; change
  journal written before every change; rollback that refuses to overwrite external changes;
  verification with a crash-recovery marker; PreferenceService (startup recovery, background
  verification, persisted report); GPU Preferences page (verdict + table, application list, editor
  with confirmation, Launch, Windows settings link, change history with Undo);
  `--verify-gpu-preferences`.
- **Bugs found:** rollback passed a reference into the journal that `append` invalidated (crash;
  fixed by passing by value); a crashed live test left the probe's value set — restored by hand
  immediately, then made impossible to repeat with a snapshot + CTest cleanup fixture.
- **Tested:** 17 unit tests (fake registry), 4 live registry tests, 1 GUI test driving the page
  (apply / cancel / undo ×2) — all only on GPUFusionProbe.exe; registry verified identical before
  and after every run.

## Phase 6 — delivered

- **Implemented:** pure report builder (`diagnostics/`): issue rules with suggested actions,
  multi-GPU compatibility assessment from detected capabilities, metric-availability matrix, shared
  tables, Markdown + JSON rendering, log-problem parsing, privacy redactor; read-only OpenCL check;
  Diagnostics page (summary, issues, adapters/APIs, monitoring matrix, multi-GPU, GPU preference,
  application/storage, recent log problems; Export / Copy / Open folder); `--export-diagnostics`.
- **Development-machine incident:** the G: disk is failing (bad sectors + bad SATA link; see
  TROUBLESHOOTING.md). Builds produced corrupt intermediate files and Ninja failed to rebuild 19
  out-of-date objects — one caused heap corruption when the Diagnostics page was destroyed. Fixed by
  making `scripts/build.ps1` retry damaged outputs and run an independent stale-object check after
  every build. The user chose a GitHub-only backup (pending `gh auth login`).
- **Tested:** 11 report unit tests, 1 end-to-end export test (no personal identifiers), page
  screenshot and smoke tests of every page.

## Phase 7 — delivered

- **Implemented:** `workloads/`: a deterministic integer hash workload with a CPU reference
  (single/multi-threaded) and a Direct3D 11 pixel-shader version (SM 4.0, so it runs on FL 10_1
  hardware without compute shaders; shaders compiled at build time by `fxc`). It renders any band of
  rows (`originY`), which is the unit Phase 8 will split between GPUs, and measures GPU time with
  timestamp queries; plus a CPU ↔ GPU transfer test that verifies the data. `benchmarks/`: model
  (statistics, comparable keys, baseline search, significance rule, cross-target summary, JSON/CSV,
  retention), runner (11 benchmarks on this PC: detection ×2, monitoring overhead ×2, CPU hash ×2,
  GPU hash ×2, transfer ×2, probe launch), worker-thread service saving `sessions\<id>.json` +
  `results.csv`. Benchmarks page (checklist, size preset, run/cancel with progress, results table,
  history) and `--run-benchmarks`. Monitoring is paused during runs.
- **Bugs found by the new tests and fixed:** session ids had one-second resolution (back-to-back
  sessions overwrote each other); the service emitted `finished` while still "running", so the Run
  button stayed disabled; viewing an old session could pick a *later* session as its baseline.
- **Build script fix:** the stale-object check attributed dependencies of non-object Ninja entries
  (AUTOGEN timestamps) to the previous object, causing needless rebuilds; fixed and re-verified by
  back-dating one object (detected; no false positives otherwise).
- **Tested:** 11 model unit tests (incl. hash values computed independently), 4 live workload tests
  (bit-exact GPU = CPU on both GPUs, incl. a mid-image band), 1 UI test driving the page.

## Phase 8 — delivered

- **Implemented:** `workloads/SplitPlan` (pure: row ranges, balanced shares, ideal split time),
  `workloads/SplitHash` (one image computed by several GPUs and CPU threads at once, static or shared
  queue, assembled in CPU memory), `workloads/VideoDecode` (generated H.264 test clip; Media
  Foundation decoding in software or on a chosen GPU via `IMFDXGIDeviceManager`; frame hashing),
  `benchmarks/LabRunner` (two experiments as ordinary benchmark sessions, with D3DKMT busy-engine and
  NVML P-state evidence per mode), `benchmarks/LabModel` (pure: verdict vs the best single device,
  ideal-split efficiency, findings), `BenchmarkService::startJob`, the Workload Lab page (verdict,
  bar chart with reference lines, table, findings, evidence tooltips), `--run-lab` / `--lab-size`.
- **Results** (MULTI_GPU_RESEARCH.md): compute split by measured speed 1.14–1.19× faster than the
  GT 730 alone (95–100 % of the theoretical limit); one video stream per GPU 2.2× slower than both on
  the HD 2000, whose decoder is 4.5× faster than the GT 730's.
- **Measurement fix:** an idle GT 730 drops to P8; one warm-up run did not bring it back, which made
  a split look 1.4× slower. All lab modes and GPU benchmarks now warm up for at least 1 s, and P-states
  are recorded.
- **Development incident:** a file-wide regex edit deleted part of `BenchmarkRunner.cpp`; it was
  restored exactly from the session transcript and diff-checked. Multi-line edits are now made with
  the editor tool, not regexes. (The project has no version control yet — pending GitHub login.)
- **Disk:** a new corruption symptom (LNK1285, corrupt PDB) appeared; `build.ps1` now recovers it.
- **Tested:** 6 lab model unit tests, 6 live lab tests (split image bit-exact in every mode incl. an
  odd height; clip creation; hardware decode identical to software on both GPUs; two decoders at once;
  both experiments end to end; cancellation), 1 UI test driving the Workload Lab page.

## Open items carried forward

- License choice (needed before Phase 9 publishing).
- `gh auth login` by the user before publishing.
- Child processes launched by GPU Fusion (Phase 4/5) must get the user's original TEMP back
  (`GPUFUSION_ORIGINAL_TEMP`), not GPU Fusion's data folder.
