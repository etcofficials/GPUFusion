# GPU selection (per-application preference)

**Status: implemented and verified (Phase 5).** Read and shown per process on the Applications page
(Phase 4); set, verified and undone on the GPU Preferences page.

## How Windows 10 stores it

Settings → System → Display → Graphics settings writes one string value per executable under
`HKEY_CURRENT_USER\Software\Microsoft\DirectX\UserGpuPreferences`:

| Choice | Stored data |
|---|---|
| Let Windows decide | `GpuPreference=0;` |
| Power saving | `GpuPreference=1;` |
| High performance | `GpuPreference=2;` |

The same string may hold other keys (e.g. `SwapEffectUpgradeEnable=1;`). GPU Fusion edits only the
`GpuPreference` key and keeps the rest in order; "Not set" removes the key (and the value if nothing
else is left). `DirectXUserGlobalSettings` (Windows' global switches) is never read or written.
Per user, so no administrator rights are needed.

## What it does on this PC — measured

GPU Fusion ships a harmless probe, `GPUFusionProbe.exe`, which creates a Direct3D 11 device on the
default adapter (as typical programs do) and reports the adapter it got, DXGI's enumeration order and
what `EnumAdapterByGpuPreference` returns. The verification sets each preference **for the probe
only**, launches it, and restores the original value.

Result on the reference PC (2026-10-16, reproduced by `tst_preferences_live`, the GPU Preferences
page and `GPUFusion.exe --verify-gpu-preferences`):

| Preference set for the probe | Default D3D11 device | First adapter enumerated | `MINIMUM_POWER` query |
|---|---|---|---|
| Not set | GeForce GT 730 | GeForce GT 730 | GeForce GT 730 |
| Let Windows decide | GeForce GT 730 | GeForce GT 730 | GeForce GT 730 |
| Power saving | GeForce GT 730 | GeForce GT 730 | GeForce GT 730 |
| High performance | GeForce GT 730 | GeForce GT 730 | GeForce GT 730 |

**Conclusion: on this PC the Windows preference makes no difference.** Windows moves applications
between GPUs only when it recognises a power-saving (integrated) adapter. It does not treat the
Intel HD Graphics 2000 as one — consistent with its WDDM 1.2 driver, which predates the hybrid-GPU
model. Setting "Power saving" will not move programs to the Intel GPU. GPU Fusion still lets you set
preferences (they matter on other PCs and are harmless here) but warns before you apply one.

## Ways a program *can* end up on the Intel GPU here (not automated by GPU Fusion)

- The program has its own adapter setting and enumerates adapters itself (many editors and games;
  LumaForge's renderer can be given an adapter). The Applications page shows where it really runs.
- **Untested hypothesis:** DXGI lists the adapter driving the primary display first. Connecting the
  monitor to the motherboard's video output and making the iGPU the primary display in the
  BIOS/UEFI would likely make default programs use the Intel GPU — at the cost of the GT 730 for
  them. This needs physical and firmware changes, so GPU Fusion does not attempt or claim it.

## Safety

- Every change is confirmed in a dialog that shows the exact key, value, old data and new data.
- Before Windows is touched, the change (with the previous data) is appended to
  `data\config\gpu-preference-changes.json`; if the journal cannot be saved, nothing is changed.
- Undo restores the previous data. It refuses — unless you confirm "Undo anyway" — if the setting
  was changed elsewhere (e.g. in Windows Settings) since, so a newer choice is never silently lost.
- Verification records the probe's original value as "pending" before changing it; if GPU Fusion
  stops mid-run, the next start restores it. Last result: `data\reports\gpu-preference-verification.json`.
- Applications must be restarted for a preference to apply; the page says so and detects whether
  the program is running. "Launch" starts the program with the user's normal environment (TEMP).
