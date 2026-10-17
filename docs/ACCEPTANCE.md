# Acceptance checklist (release 1.0.0, 2026-10-16)

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

## Tests at release

21 CTest entries (20 suites + registry restore fixture), 156 cases, all passing in the release build
with reference-machine expectations; unit tests also in the debug build; all 8 pages pass a
start/close smoke test.
