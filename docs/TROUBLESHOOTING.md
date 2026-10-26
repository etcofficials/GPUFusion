# Troubleshooting

## The application does not start

| Symptom | Cause | Fix |
|---|---|---|
| Nothing happens; exit code `0xC0000135` | A DLL is missing next to `GPUFusion.exe` (Qt or MSVC runtime) | Rebuild (deployment runs after every link) or re-extract the release package. Windows names the missing DLL in Event Viewer → System, Event ID 26. |
| "This application failed to start because no Qt platform plugin could be initialized" | `platforms\qwindows.dll` missing | Same as above — the `platforms` folder must sit next to the exe. |

## "Data folder" warnings

- **Status bar shows "Preferred data folder rejected"** — the configured folder could not be
  created or written (read-only drive, permissions, a file with the same name). GPU Fusion then
  uses `%LOCALAPPDATA%\GPUFusion`. Diagnostics → Storage shows the reason. Fix the folder or pass
  `--data-dir=<writable path>`.
- **Settings page shows "Settings file invalid, defaults in use"** — `config\settings.json` was not
  valid JSON. The original was kept as `settings.json.corrupt-<date-time>` next to it; saving
  writes a fresh file.
- **"written by a newer GPU Fusion … changes will not be saved"** — an older build is running
  against a newer settings file. It is left untouched on purpose; use the newer build.

## GPUs page

| What you see | Meaning | What to do |
|---|---|---|
| A GPU is missing from Task Manager but shown in GPU Fusion | Its driver is WDDM 1.x; Task Manager only graphs WDDM 2.0+ drivers | Nothing — expected for older drivers such as Intel HD 2000/3000 (9.17.x) |
| **Not detected** card (e.g. Intel graphics) | Windows remembers the device but it is not on the PCI bus | Check BIOS/UEFI for "iGPU", "Internal Graphics", "IGD Multi-Monitor" or "Primary Display". Many boards switch the iGPU off when a discrete card is installed. |
| **Problem** badge with a code | Device Manager problem code; the card explains it | Code 22: enable in Device Manager. Code 28/31/39: reinstall the vendor driver. Code 43: driver or hardware fault. |
| **Not available to DirectX** | Device Manager lists it but DXGI does not | Reinstall the vendor driver; a virtual/display-only device can be ignored |
| **No vendor driver installed** | Windows "Basic Display Adapter" is in use | Install the GPU vendor's driver |
| **Device creation failed** | Direct3D 11 could not start on that adapter; the card shows the plain-language HRESULT | Restart; update/reinstall the driver |
| Software renderer card | Microsoft Basic Render Driver (WARP) | Not a GPU; never counted |

The page refreshes automatically when Windows reports a device change; **Refresh** re-tests every adapter.

## Dashboard

| What you see | Meaning |
|---|---|
| **Unsupported** | The adapter/driver does not provide that metric at all (e.g. GT 730 power and clocks via NVML; every sensor on Intel HD 2000). Not a fault. Hover for the reason. |
| **Unavailable** | It should be readable but the last read failed, or the source is off (NVML disabled in Settings or not installed). |
| **Unknown** | Not determined yet — utilization needs two samples, so the first reading after start or resume is Unknown. |
| Gaps in a graph | Readings that were not available, or a pause. Gaps are never drawn as zero. |
| "Engine 0 … Engine 5" | The driver (WDDM 1.x) does not name its engines. |
| Yellow "No new readings" banner | No sample for 3× the interval (≥ 3 s). Usually a driver reset or a heavily loaded system; adapters are re-detected automatically if one disappeared. |
| Utilization differs slightly from Task Manager | Both use the busiest engine; small differences come from different sampling instants. Measured agreement on the GT 730: within 1 %. |

## Applications page

| What you see | Meaning |
|---|---|
| A process is missing for the Intel (or another pre-WDDM 2.0) GPU | Per-process data for such drivers needs to open the process with query rights; Windows only allows that for your own processes unless GPU Fusion runs as administrator. The coverage line shows how many could be inspected. GPU Fusion never asks for elevation. |
| "Memory only" | The process holds GPU memory but did no GPU work in the last sample. It is not counted as using the GPU. |
| GPU usage "Unknown" right after opening the page | Usage is a rate and needs two samples (about 2 s). |
| Path / preference "Unknown" | The process is protected or belongs to another account, so its executable path cannot be read. |
| Row on "Microsoft Basic Render Driver (software, CPU)" with activity | That process renders on the CPU (WARP), not on a GPU. |
| Per-process numbers don't add up to the Dashboard | Different sampling instants, rounding, and processes that cannot be inspected. The Dashboard total is the adapter-wide figure. |

## GPU Preferences page

| What you see | Meaning / what to do |
|---|---|
| "On this PC the preference makes no difference" | Measured with the probe: Windows sends every preference to the same GPU here. Choose the GPU inside the program if it has a setting. See GPU_SELECTION.md. |
| A preference has no effect on one program | It needs a restart; and programs that pick an adapter themselves ignore the preference. Check the Applications page while it runs. |
| "Undo … refused: changed outside GPU Fusion" | Windows Settings (or another tool) changed it after GPU Fusion did. Undo anyway only if you want GPU Fusion's earlier value back. |
| "Could not save the change journal, so nothing was changed" | The data folder is not writable. GPU Fusion never changes a preference it cannot record. |
| Verification shows "could not be restored" | Windows refused the restore; GPU Fusion retries at the next start. The value only affects GPUFusionProbe.exe. |
| "Integrated GPU — Windows cannot enforce this on this PC", Apply disabled | The probe verification showed Windows still gives programs the other GPU under "Power saving". Writing it would not do what it says, so GPU Fusion does not. Use the program's own GPU setting, or (your decision, untested) drive the display from the integrated GPU. |
| "Unknown — run the verification" | No probe verification yet on this PC; run it first (top of the page). |
| Verify: "did no measurable GPU work while observed" | Use the program while GPU Fusion watches (open its window or a 3D view), or increase `--verify-seconds`. Programs that hand over to an already running instance exit at once — close every instance first. |
| Verify: "GPU work … engine type not reported" | The Intel WDDM 1.2 driver does not name its engines: GPU Fusion can say *where* the work ran, not that it was 3D rendering. |
| Verify: "The GPU work came from X.exe … probably a launcher" | The selected program starts the real renderer. Use the offered button to configure that executable instead. |

## Logs

`<data folder>\logs\gpufusion.log` (Diagnostics → Open log file). Older logs are
`gpufusion.1.log` … Raise the detail with Settings → Logging → Minimum level → Debug.

## Diagnostics page and reports

- **Export report…** writes `<name>.md` (readable) and `<name>.json` (machine-readable) to the data
  folder's `reports` folder by default; **Copy report** puts the Markdown on the clipboard.
- Reports leave out the user name, computer name, running programs and configured application paths;
  the user-profile folder appears as `%USERPROFILE%`. Hardware, drivers and settings are included.
- From a script: `GPUFusion.exe --export-diagnostics <file>` (waits for live readings, max ~8 s).

## Benchmarks page

| Symptom | Cause / fix |
|---|---|
| Run is disabled | Adapter detection is still running, or nothing is ticked. |
| "Wrong" in the Output column | The GPU produced a different image than the CPU reference — a driver or hardware fault. The result is kept for evidence but never used as a baseline. Re-run; if it repeats, update or reinstall the driver. |
| Variation shown in orange (> 10 %) | Something else was using the CPU/GPU, or the GPU changed clock state. Close other programs and run again. |
| "No earlier comparable result" | Comparisons need an earlier run with the same benchmark, workload size, GPU model and driver. Changing any of these starts a new baseline on purpose. |
| Closing the window during a run takes a few seconds | The run in progress finishes first (runs are never cut short); that session is not saved. |

Results: `<data>\benchmarks\sessions\*.json` (full detail) and `results.csv`. Sessions older than the
retention period in Settings are deleted when a new session is saved; the CSV is kept.

## Workload Lab page

| Symptom | Cause / fix |
|---|---|
| "No H.264 decoder producing NV12 is available" / "Media Foundation is not available" | Windows "N" editions lack media components: install the Media Feature Pack. |
| A GPU's frames "did not arrive as textures on this GPU" | Media Foundation fell back to software decoding on that adapter (driver without D3D11 video decoding for H.264). Its timing then measures the CPU, and the page says so. |
| "Wrong" output for a GPU | The GPU's result differs from the CPU reference (compute) or the software decoder (video) — a driver or hardware fault. That mode is excluded from the verdict. |
| A split mode much slower than expected | Check the row tooltip: a GPU in performance state P8 runs at reduced clocks. GPU Fusion warms up for at least 1 s per mode; a GPU that is mostly idle in a mode (e.g. waiting for a slower partner) may legitimately stay in P8. |
| The first decode run takes a few seconds longer | The test video is generated once (about 2 s) and kept in `<data>\benchmarks\lab\media`. Deleting it is safe; it is recreated. |

## Development PC: failing G: disk (diagnosed 2026-10-16)

Symptoms seen while building: corrupt `.obj`/`.pdb`/dependency files, `LNK1104`, `LNK1136`, `LNK1285`,
`C2471`, an object that was not rebuilt after its header changed (heap corruption at run time), and
a freshly compiled test that crashed with an *illegal instruction* (0xC000001D) until its object was
rebuilt. If a binary misbehaves in a way no source change explains, delete its `build\<preset>\...\CMakeFiles\<target>.dir`
folder and rebuild.

While packaging 1.0.1 (2026-10-17), freshly written copies of the app on G: sometimes hung at start-up
or failed with 0xC000007B ("invalid image format") although their bytes were identical to working
copies; a copy of the same files elsewhere started normally. Windows keeps the loaded program image
of a file cached separately from its data, so one bad read while loading can make every later launch
of *that* file fail. `scripts\package.ps1` therefore times out hung launches and always puts the app's
data folder back; re-running it writes fresh files. The published packages were verified on another
drive after download.

2026-10-26, rebuilding the debug tree (unused for ten days): `C1051` (a `.pdb` "has an obsolete
format"), then `C2471` for another `.pdb`, then `C1004` in a **truncated generated `.moc` file**. The
whole debug tree (800 MB of generated files) was deleted and rebuilt cleanly. `build.ps1` now also
treats `C1051` as damage, and can remove damaged generated `.moc`/`.cpp` files inside the build
directory. It never touches anything outside the build directory.

Evidence (read-only checks; SMART read once with administrator consent):

| Source | Finding | Meaning |
|---|---|---|
| SMART 187 Reported uncorrectable | 434 (normalized 1 of 100) | the drive failed to read data hundreds of times |
| SMART 197 / 198 | 8 pending / 8 offline-uncorrectable sectors | unreadable sectors |
| SMART 199 UDMA CRC errors | 1013 | data corrupted on the SATA cable/port |
| System log, disk 154 / disk 7 | 2,244 failed I/Os in 63 regions (0–325 GB); 700 "bad block" | widespread media errors, rising since 2026-10-09 |
| System log, NTFS 55/130/131/50 | corruption found / repaired / once "cannot be corrected"; delayed writes failed | file-system damage, possible lost writes |

Two problems: **the disk surface is degrading** (replace the drive — software cannot fix this) and
**the SATA connection is faulty** (reseat or replace the SATA data cable, or use another port).
Recommended order: back up everything important on G: first; then fix the cable; then replace the
disk. Run `chkdsk G: /r` only after a backup — it reads every sector for hours and can make a failing
disk worse.

## Build problems

| Symptom | Fix |
|---|---|
| `cmake --preset` says the preset is disabled | Dot-source `scripts\env.ps1` first (it sets `GPUFUSION_ENV=1`). |
| A dot-sourced script makes later commands stop on harmless stderr output | Use the current `env.ps1`, which restores `$ErrorActionPreference`. |
