# Acceptance checklist (releases 1.0.0 – 1.2.0)

Verified on the reference PC (Windows 10 Pro 22H2, i5-2400S, 12 GB, GeForce GT 730 + Intel HD
Graphics 2000). "Evidence" points to the test, measurement or document that shows it.

Release 1.0.1 (2026-10-17) changes licensing and packaging only (MIT `LICENSE.txt` in the packages,
LF checksum file); it was rebuilt, re-tested with the full suite and re-verified by `package.ps1`.

## Functionality

| Requirement | Status | Evidence |
|---|---|---|
| Honest detection of every GPU; software adapters (WARP) never reported as GPUs | ✔ | `tst_hardware_live`, `tst_adaptermodel`; GPUs page |
| Live monitoring; unsupported metrics shown as Unsupported/Unavailable/Unknown, never 0 | ✔ | `tst_metricmodel`, `tst_monitoring_live`; COMPATIBILITY.md metric matrix |
| Intel HD 2000 (WDDM 1.2, invisible to Task Manager) monitored | ✔ | D3DKMT provider; verified 94–100 % under load |
| Per-application GPU usage | ✔ | `tst_process_live` (PDH and D3DKMT agree) |
| GPU preference management with backup, rollback, verification | ✔ | `tst_preferences` (journal before write, refusal to overwrite external changes), `tst_preferences_live`, `tst_preferences_ui`; GPU_SELECTION.md |
| Utilization of the two GPUs is never added together | ✔ | Dashboard shows each GPU separately; no combined figure exists in the code |
| Diagnostics with exportable report, personal identifiers removed | ✔ | `tst_diagnostics`, `tst_export_live` |
| Benchmarks: warm-up, repeated runs, variation, correctness, history, comparisons only beyond noise | ✔ | `tst_benchmarkmodel`, `tst_workload_live`, `tst_benchmarks_ui` |
| Multi-GPU workload lab; no speed-up claimed without measurement | ✔ | `tst_lab_live`, `tst_labmodel`; MULTI_GPU_RESEARCH.md (gain 1.14–1.19× measured; decode split measured slower and reported so) |
| WARP never reported as Intel GPU acceleration | ✔ | Workloads and decoders are bound to adapters by LUID; frames verified to come from that GPU |

## Safety and privacy

| Requirement | Status | Evidence |
|---|---|---|
| No automatic BIOS, driver, device or registry changes | ✔ | The only registry writes are per-app GPU preferences the user applies (HKCU, journalled); the per-user installer's own uninstall entry |
| No overclocking/voltage, no services, no DLL injection, no patching other programs | ✔ | Not present in the code |
| No administrator rights requested | ✔ | App and installer run as the user |
| No network access by the application | ✔ | No network code; Qt network module not deployed |
| No credentials or personal information published | ✔ | Commits use the GitHub noreply address; test fixtures use fictional identifiers; reports are redacted |

## Storage

| Requirement | Status | Evidence |
|---|---|---|
| Project, tools, build, downloads, temp, data on G: | ✔ | STORAGE_POLICY.md verification log (C: free 84.73 → 84.58 GB over all phases, no file attributable to GPU Fusion) |
| Process TEMP redirected (driver shader caches off C:) | ✔ | `main.cpp`; launched programs get the original TEMP back |

## Deliverables (section 21)

| Deliverable | Status | Evidence |
|---|---|---|
| `release\GPUFusion.exe` with icon and version info, double-click launchable | ✔ | Launcher → `release\GPUFusion\GPUFusion.exe`; version 1.0.0; verified by `package.ps1` |
| `Launch GPU Fusion.bat` | ✔ | Starts the release (or dev) build; smoke test passed through it |
| Desktop shortcut "GPU Fusion.lnk" (existing one checked, never overwritten if foreign) | ✔ | `scripts\make-shortcut.ps1`; created (none existed) |
| Portable folder via windeployqt + `GPUFusion-Windows-x64.zip` | ✔ | Extracted ZIP launches and keeps data next to the exe |
| Installer `GPUFusion-Setup.exe` | ✔ | Test install + uninstall verified (no shortcuts/registry in test mode) |
| GitHub repository with README, .gitignore, docs, CI workflow, changelog | ✔ | https://github.com/etcofficials/GPUFusion (private) |
| LICENSE only after confirming the choice | ✔ | MIT, chosen by the user on 2026-10-17; repository made public afterwards |
| GitHub release with checksums | ✔ | Release v1.0.0 with ZIP, installer, SHA256SUMS.txt |

## 1.1 — per-application GPU selection

| Requirement | Status | Evidence |
|---|---|---|
| Diagnose why GPU switching did not work | ✔ | GPU_SELECTION.md: value written correctly; Windows ranks the GT 730 first for every preference (boot device, only display, no hybrid flags, Intel WDDM 1.2) |
| Intel / NVIDIA / Automatic / Both choices | ✔ | GPU Preferences page; `tst_gpuselection`, `tst_preferences_ui` |
| Write a preference only where justified; read it back | ✔ | Intel not written on this PC (cannot be enforced); NVIDIA written and read back (`tst_preferences_ui`) |
| Preserve other keys and values; reversible | ✔ | `tst_preferences` (unchanged), journal + undo |
| Verify actual GPU usage, per process tree, 3D/compute only | ✔ | `tst_appverify_live` on the hardware; `--verify-app` with Microsoft Edge |
| NVIDIA selection verified on physical hardware | ✔ | probe rendering confirmed on GT 730 |
| Intel selection verified on physical hardware | ✘ (Windows limitation) | measured: with power saving set, rendering stays on the GT 730; a program choosing the Intel GPU itself is seen there |
| Launcher handling | ✔ | `cmd.exe` → probe traced to the child; button to configure the renderer |
| Dual-GPU capability, unsupported apps marked | ✔ | dual-GPU check (PE imports + hardware); not possible on this PC for one workload |
| No driver, firmware, global-setting or other-program changes | ✔ | only HKCU per-app values for chosen executables |

## 1.2 — generic GPU manager

| Requirement | Status | Evidence |
|---|---|---|
| Works for any executable; no per-application implementation | ✔ | profiles keyed by executable path; discovery from Start menu / installed programs / running processes; no application names in `src/profiles`, `src/dispatch` |
| Persistent per-app profiles (mode, arguments, applied value, last verification) | ✔ | `tst_profiles` (JSON, store isolation, ids), GPU Manager page |
| Intel / NVIDIA / Automatic / Both per profile | ✔ | `tst_gpumanager_ui`, `tst_profiles_live` |
| Strongest justified generic mechanism; never write a value that would not work | ✔ | Discrete: Windows value written + read back; Intel: no value on this PC (Windows cannot place programs there), value removed; `tst_profiles::applyWritesOnlyJustifiedValues` |
| Capability per mode, classes A–E, with reasons | ✔ | `assessMode`; page shows status/class/mechanism/reasons; `tst_profiles::referencePcCapabilities` |
| Verification of actual GPU use per mode | ✔ | `tst_profiles_live`: Integrated → work only on Intel (SDK program), Discrete → GT 730, Both → both GPUs, Automatic → observed |
| Intel selection for arbitrary programs | ✘ (Windows limitation, unchanged) | only programs with their own GPU setting, SDK programs or adapter-option jobs; documented, never promised |
| Reusable multi-GPU architecture (independent workloads) | ✔ | `src/dispatch`: any command template or built-in decoding, shared queue, straggler rule; `tst_dispatch`, `tst_dispatch_live`, `tst_jobpanel_ui` |
| No speed-up claimed from utilisation alone | ✔ | Both verdict says so explicitly; dispatcher compares with a concurrency control, checks outputs, uses the significance rule; measured gains ≤ ~15 % and not stable, reported as such (MULTI_GPU_RESEARCH.md) |
| Optional developer SDK | ✔ | `sdk/include/gpufusion/gpufusion.h`, docs/SDK.md, `tst_sdk`; shipped in the ZIP/installer under `sdk\` |
| One rendering workload split across GPUs (class E) | ✘ (not possible here) | Intel has no D3D12; no cross-vendor sharing; stated in every Both assessment |
| No injection, patching, driver/BIOS/global changes | ✔ | only HKCU per-app values for chosen executables, after confirmation |
| Existing features preserved | ✔ | GPU Preferences page, NVIDIA selection, detection, monitoring, benchmarks, Lab unchanged; their tests pass |

## 1.3 — the program's own GPU option

| Requirement | Status | Evidence |
|---|---|---|
| Real, unmodified programs on the GPU of the profile's mode where Windows cannot do it | ✔ (programs with a GPU option) | Edge and Brave on the Intel GPU via `--use-adapter-luid={luid_pair}` (renderer string + verification); FFmpeg via d3d11va index; `tst_profiles_live::realBrowserFollowsItsKnownOption` |
| Generic: no per-application code; suggestions are data with evidence, user-extensible | ✔ | placeholders work for any program; `KnownOptions` table + `config\gpu-options.json`; `tst_profiles`, `tst_gpumanager_ui` |
| Never claimed before verified | ✔ | status "Expected" until *Verify on this PC* confirms it for the program |
| No Windows value written for the program's own option | ✔ | Edge registry value checked absent before and after |
| Verification does not miss sandboxed processes on WDDM 1.x GPUs | ✔ | limited-rights retry; uninspected processes named; `tst_gpuselection` |
| Arbitrary programs without a GPU option on the Intel GPU | ✘ (Windows limitation, unchanged) | documented |

## 1.4 — running programs

| Requirement | Status | Evidence |
|---|---|---|
| A running program is not silently "verified" with the wrong GPU | ✔ | Launch/Verify detect it and explain; verification still notes "another instance was running" |
| Closing is graceful, consented, never forced | ✔ | WM_CLOSE only, after the user's choice (GUI) or `--close-running` (CLI); `tst_running_live` (refusing / windowless programs left running) |
| User's own running programs untouched during development | ✔ | only the probe was closed; Brave and Discord, which were running, were not tested |
| Chromium-family suggestion without per-app code | ✔ | file signature; `tst_profiles::chromiumProgramsAreDetectedByTheirFiles` |

## 1.5 — GPU shortcuts

| Requirement | Status | Evidence |
|---|---|---|
| The profile's GPU choice applies when the program is started the everyday way | ✔ (via a GPU Fusion shortcut) | `tst_running_live::shortcutStartsTheProgramThroughItsProfile` (Shell-opened .lnk → probe with the Intel LUID) |
| Writes only where the user chooses; nothing system-wide | ✔ | save dialog / `--shortcut-dir`; default Desktop only on explicit request |
| Running program handled with consent | ✔ | interactive dialog; dismissing it starts and closes nothing (checked) |
| Correct exit codes for scripted use | ✔ | headless modes return `QApplication::exit`'s code |

## Tests at release

1.5.0: 31 CTest entries, 225 cases, all passing with reference-machine expectations (2026-10-26).

1.4.0: 31 CTest entries, 223 cases, all passing with reference-machine expectations (2026-10-26).

1.3.0: 30 CTest entries, 218 cases, all passing with reference-machine expectations (2026-10-26).

1.2.0: 30 CTest entries (29 suites + registry restore fixture), 212 cases, all passing in the release
build with reference-machine expectations (2026-10-26).

1.0.0: 21 CTest entries (20 suites + registry restore fixture), 156 cases, all passing in the release build
with reference-machine expectations; unit tests also in the debug build; all 8 pages pass a
start/close smoke test.
