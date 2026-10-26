# Storage policy

The C: drive on the development machine has limited free space, so everything GPU Fusion
produces during development goes to G:.

| What | Location | Verified |
|---|---|---|
| Source, scripts, docs | `G:\GPUFusion` | Phase 1 |
| Build output | `G:\GPUFusion\build\<preset>` | Phase 1 |
| Compiler / test temp files | `G:\GPUFusion\temp` (TEMP/TMP set per session by `scripts\env.ps1`); tests use `build\<preset>\test-temp` | Phase 1 |
| Runtime data (dev builds) | `G:\GPUFusion\data\{config,logs,benchmarks,reports,temp}` | Phase 1–2 |
| Phase 0 audit evidence | `G:\GPUFusion\data\audit` | Phase 0 |
| Release artifacts | `G:\GPUFusion\release` | Phase 9 |
| Toolchain (pre-existing, shared) | `G:\DevTools` (MSVC, Windows SDK, CMake, Ninja, gh, NSIS), `G:\Qt\6.8.3` | Phase 0 |

## Rules followed

- No new SDKs or tools were installed for Phases 0–1; the existing G: toolchain is reused.
- Environment changes are session-scoped (`scripts\env.ps1`); no global variables or registry edits.
- Logs are capped: at most *max file size × files kept* (default 1 MB × 5).
- The application writes only to its data folder. If that folder is not writable it falls back to
  `%LOCALAPPDATA%\GPUFusion` (C:) and says so in the status bar and Diagnostics page.

## Driver caches and process TEMP

Creating Direct3D devices (adapter probes, from Phase 2) makes the NVIDIA driver write a shader
cache (`NVIDIA Corporation\NV_Cache`, ~20 KB) under `%TEMP%`. To keep it off C:, GPU Fusion sets
**its own process's** `TEMP`/`TMP` to `<data folder>\temp` at startup. Nothing outside the process
changes; the original value is kept in `GPUFUSION_ORIGINAL_TEMP` for child processes.

Verified 2026-10-16: launched from a shell with TEMP on C:, 0 files changed under
`C:\Users\…\Temp\NVIDIA Corporation`, and the cache appeared in `G:\GPUFusion\data\temp`.
One 20 KB cache file written by a test launch before this fix (16:43) remains in the C: TEMP; it is
driver-managed and safe to delete.

Not relocated (Windows-managed): Qt's Widgets/raster rendering uses no per-user caches.

## Verification log

| Date | Check | Result |
|---|---|---|
| 2026-10-16 | Phase 0: C: free before/after audit | 84.73 GB / 84.73 GB |
| 2026-10-16 | Phase 1: build, tests, 10 app launches | Build output 62 MB, all under `G:\GPUFusion\build`. No `%LOCALAPPDATA%\GPUFusion` created. C: free 84.73 → 84.68 GB (other running programs also write to C:). No file in C: TEMP is attributable to GPU Fusion: the NVIDIA cache entries there predate the first launch, and 0-byte GUID `.tmp` files of the same pattern appeared before GPU Fusion existed. |
| 2026-10-16 | Phase 2: launch with C: TEMP after the process-TEMP fix | 0 files changed on C: TEMP; NV cache in `G:\GPUFusion\data\temp`; no `%LOCALAPPDATA%\GPUFusion` |
| 2026-10-16 | Phase 3: all monitoring tests, 9 app launches (incl. 3 × 75 s overhead runs) | 0 NVIDIA cache files written on C: since the fix; no `%LOCALAPPDATA%\GPUFusion`; data folder 0.1 MB (log 18 KB); build 145 MB on G:; C: free 84.67 GB |
| 2026-10-16 | Phase 4: probes, tests, ~12 app launches incl. two 75 s overhead runs | No new files on C: (the only NVIDIA cache files there date from the pre-fix Phase 2 launch at 16:43); app cache in `data\temp`, test cache in `build\release\test-temp`; data folder 0.07 MB; C: free 84.70 GB |
| 2026-10-16 | Phase 5: probe runs (app, CLI, tests) | 0 new files in C: TEMP: the probe inherits GPU Fusion's TEMP (`data\temp`); preference journal and verification report in `data\config` / `data\reports`; C: free 84.70 GB |
| 2026-10-16 | Phase 7: test suite, 2 headless benchmark sessions, UI runs | No GPU Fusion files on C: (`%LOCALAPPDATA%\GPUFusion` absent; no NVIDIA/D3D cache writes on C:; 4 tiny files in C: TEMP from other programs). Benchmark data 0.08 MB in `data\benchmarks`; build 363 MB on G:; C: free 84.61 GB |
| 2026-10-16 | Phase 8: test suite, 5 Workload Lab sessions (incl. a heavy split), UI runs, test-clip generation | No GPU Fusion files on C: (`%LOCALAPPDATA%\GPUFusion` absent; no NVIDIA/D3D/Intel cache writes on C:; 7 tiny files (0.03 MB) in C: TEMP of the same GUID `.tmp` / `cv_debug.log` pattern seen before GPU Fusion existed). Lab data 5.1 MB in `data\benchmarks\lab` (4.9 MB is the generated test video in `lab\media`); test clip for the tests in `build\release\test-temp`; build 418 MB on G:; C: free 84.58 GB |
| 2026-10-16 | Phase 9: debug + release builds and suites, packaging and its verification (ZIP launch, launcher, test install/uninstall), license texts downloaded (36 KB to `downloads\licenses`) | Packaging verification ran entirely in `temp\package-verify` (removed afterwards); the test install wrote no registry entry and no shortcut. Only file on C:: the requested desktop shortcut `GPU Fusion.lnk` (~1 KB). Release folder ~45 MB on G: |
| 2026-10-20 | 1.1: clean rebuild, live verification runs (probe, Edge with a throwaway profile in `temp\edge-profile`, deleted afterwards) | Builds, logs and the Edge test profile on G: only; the Edge test used `--user-data-dir` on G: so the user's own Edge profile on C: was not used; no GPU Fusion files on C: |
| 2026-10-26 | 1.2–1.11: all live tests (probe, Edge and Brave with throwaway profiles on G:, FFmpeg), debug + release builds, 11 packagings | No GPU Fusion files on C: other than the requested desktop shortcut. Checked by name in C: TEMP, `%LOCALAPPDATA%`, `%APPDATA%` and the Desktop. C: free 84.58 → 80.96 GB since Phase 8; files changed on C: since 2026-10-16 are Brave 2.9 GB, OneDrive 0.6 GB, Edge 0.6 GB (557 MB in the user's own *Default* profile; the tests' profiles were on G:), Discord 0.45 GB (updates), Claude Code's session data 0.1 GB, C: TEMP 43 MB (no file with GPU Fusion's or its tests' names; GPU Fusion and its tests use TEMP folders on G:). Automatic profile backup (1.11) writes only to a folder the user chooses, off by default. |
| 2026-10-16 | Graphics DLLs loaded by GPUFusion.exe (Phase 1, no GPU features yet) | Qt's Windows platform plugin loads `d3d11`, `d3d12`, `d3d9`, `dxgi`, `DWrite`; no shader cache written during the test launches |
