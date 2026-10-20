# Changelog

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
