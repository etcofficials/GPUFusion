# GPU Fusion — final report

State at version 1.10.x (2026-10-26), on the reference PC: Windows 10 Pro 22H2, Intel Core i5-2400S,
NVIDIA GeForce GT 730 (Fermi GF108, driver 391.35, WDDM 2.3), Intel HD Graphics 2000 (driver
9.17.10.4459, WDDM 1.2). This report separates what was **verified on that hardware** from what is
**inferred or hypothetical**, and lists what was **not done**. Details and measurements are in the
linked documents.

## 1. Verified on the reference PC

### GPU detection and monitoring
- Both GPUs are detected with drivers, WDDM versions and tested Direct3D feature levels. The software
  renderer (WARP) is never counted as a GPU. ([COMPATIBILITY.md](COMPATIBILITY.md))
- Live load and memory for both GPUs. The Intel GPU (WDDM 1.2) is invisible to Task Manager and
  Windows performance counters; GPU Fusion reads it through D3DKMT. Values a driver does not expose
  are shown as Unsupported/Unavailable, never as 0.
- Per-process GPU use on both GPUs. Since 1.3, this includes sandboxed processes (e.g. a browser's
  GPU process) on the WDDM 1.2 GPU. Processes Windows does not let a standard user open are named,
  not silently missed.

### Choosing a program's GPU
- **Windows' per-app preference cannot move programs to the Intel GPU on this PC.** Windows ranks the
  GT 730 first for every preference: it is the boot device, it drives the only display, neither GPU
  has a hybrid classification, and the Intel driver is WDDM 1.2. Verified with real rendering: a
  program set to "Power saving" still renders on the GT 730. ([GPU_SELECTION.md](GPU_SELECTION.md))
- **Windows' preference does select the GT 730** (`GpuPreference=2;`, written, read back, confirmed
  by observed rendering).
- **Programs with their own GPU option can be put on the Intel GPU through a GPU Fusion profile**,
  with no Windows setting written:
  - **Microsoft Edge and Brave** with `--use-adapter-luid=<high>,<low>`: WebGL rendered by ANGLE on
    Intel HD Graphics (renderer string), confirmed by GPU Fusion's verification. Without the
    switch, or with the GT 730's LUID, they rendered on the GT 730.
  - **FFmpeg** with its d3d11va device index: decoding confirmed on each GPU, with identical frame
    hashes.
  - **GPU Fusion's SDK test program**, in every mode, including "Both GPUs": both GPUs did work.
- Profiles work for any executable, and so do variants (several profiles per program), shortcuts
  that start a program through its profile, export/import, and the live ✓/✗ check of running
  programs. Each was tested on this PC (`tst_profiles_live`, `tst_gpumanager_ui`, `tst_running_live`,
  `tst_applications_ui`).
- **GPU Fusion never closed, relaunched or changed a program of the user's** during development. Only
  its own test program was closed; Edge and Brave tests used separate, throwaway profile folders.

### Using both GPUs
- **One rendering workload cannot be split across these GPUs.** The Intel GPU has no Direct3D 12,
  and there is no resource sharing between these two vendors' drivers. No setting changes that.
- **GPU Fusion's own compute workload**, split by measured speed, was **1.14–1.19× faster** than the
  GT 730 alone (several sessions). Splitting it evenly was 2.5× slower.
  ([MULTI_GPU_RESEARCH.md](MULTI_GPU_RESEARCH.md))
- **Video decoding:** the Intel GPU decodes H.264 about 4× faster than the GT 730. One stream per GPU
  was 2.2× *slower* than both streams on the Intel GPU.
- **Batches of independent decode jobs** (job runner): the second GPU added **at most about 10–15 %**
  over the best single-GPU configuration, and the result was not stable between sessions; a short
  batch was measurably slower. Most of an apparent 1.55× gain came from running two jobs at once,
  not from the second GPU. The concurrency control was added for this reason.

### Quality
- 34 CTest entries, 234+ cases, all passing in the release build with reference-machine
  expectations. The full suite and every page's start/close test also pass in a clean debug build
  (debug heap, assertions). CI runs the hardware-independent tests on every push. (TESTING.md in the
  repository lists every suite.)
- Every visible control on every page has an accessible name and can be reached with Tab.

## 2. Inferred, not verified

- **Other Chromium-based browsers** (Chrome, Vivaldi, Opera) and **Electron/CEF apps** use the same
  Chromium switch, so it is *expected* to work. It was verified only with Edge and Brave. GPU Fusion
  marks these suggestions "not verified with this program".
- **Windows 11** is expected to behave like Windows 10 22H2. It was not tested.
- **Hybrid laptops** (with an integrated GPU that drives the display): Windows' preference is expected
  to work there in both directions. The logic is tested with simulated verification results, not on
  such hardware.
- **Screen readers:** accessible names are checked through Qt's accessibility interface, which screen
  readers use. No screen reader was run.

## 3. Hypotheses that need a decision or hardware change by the user

- **Driving the display from the integrated GPU** (monitor on the motherboard output, integrated GPU
  as primary/boot display in the BIOS) would probably make Windows give programs the Intel GPU by
  default. GPU Fusion never changes firmware, cabling or global settings, and does not claim this
  works. The cost would be that the GT 730 no longer drives the display.

## 4. Not done, and why

- **Discord (Electron) was not verified.** Testing means starting the user's Discord and ending it
  afterwards. That was blocked as interference with the user's own programs, and it was not worked
  around.
- **Hardware video encoding:** neither GPU offers it here (no NVENC on Fermi; no Quick Sync runtime).
- **A real driver reset (TDR), a driver upgrade while running, or removing a GPU** cannot be tested
  safely. They use the same device-change path that was tested.

## 5. Open risk

- **The development disk (G:) is failing:** SMART reports hundreds of uncorrectable reads, pending
  and offline-uncorrectable sectors, and over a thousand SATA CRC errors; NTFS has logged corruption. It silently replaced source files once and truncated build outputs several times.
  Safeguards are in place (`scripts/build.ps1`, git as the source of truth, everything on GitHub).
  The drive should still be replaced, and GPU Fusion profiles exported to another disk.
  ([TROUBLESHOOTING.md](TROUBLESHOOTING.md))
