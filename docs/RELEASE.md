# Releasing GPU Fusion

## Build the artifacts

```powershell
G:\GPUFusion\scripts\package.ps1          # build + all tests + package + verify
G:\GPUFusion\scripts\package.ps1 -SkipBuild
G:\GPUFusion\scripts\make-shortcut.ps1    # desktop shortcut "GPU Fusion.lnk" (never replaces a foreign one)
```

Output in `release\` (not committed; `.gitignore`):

| Artifact | Contents |
|---|---|
| `GPUFusion\` | Portable app: `GPUFusion.exe`, `GPUFusionProbe.exe`, Qt 6 Core/Gui/Widgets + `platforms`, `styles`, `imageformats` plugins (windeployqt), app-local VC++ runtime, `README.txt`, `CHANGELOG.md`, `THIRD-PARTY-NOTICES.txt`, `licenses\` (LGPLv3/GPLv3 texts for Qt). No development data pointer. |
| `GPUFusion.exe` | Launcher (no Qt, static CRT, app icon + version info) that starts `GPUFusion\GPUFusion.exe` with its arguments — the double-clickable `release\GPUFusion.exe`. |
| `GPUFusion-Windows-x64.zip` | The portable folder. |
| `GPUFusion-Setup.exe` | NSIS per-user installer (`installer\GPUFusion.nsi`): `%LOCALAPPDATA%\Programs\GPU Fusion`, Start-menu shortcut, optional desktop shortcut, HKCU uninstall entry; no administrator rights. The uninstaller removes only shortcuts it created (recorded in `install.ini`) and keeps the `data` folder unless the user agrees to delete it. |
| `SHA256SUMS.txt` | SHA-256 of the ZIP and the installer. |

## What `package.ps1` verifies (all under `temp\`, nothing on C:)

1. The extracted ZIP starts (`--smoke-test`) and creates its `data` folder next to the executable.
2. The launcher next to the extracted folder starts the app with its arguments.
3. The installer, run silently with `/TESTINSTALL` (files only: no shortcuts, no registry), installs a
   working app; its uninstaller then removes all program files.

## Publish

```powershell
git push origin main
gh release create v1.0.0 release\GPUFusion-Windows-x64.zip release\GPUFusion-Setup.exe release\SHA256SUMS.txt `
   --title "GPU Fusion 1.0.0" --notes-file <notes>
```

Commits use the GitHub noreply address configured in the repository (`git config user.email`), so no
personal e-mail address is published.

## Licensing

- GPU Fusion's own code: **no license chosen yet — all rights reserved.** Choose one (and add
  `LICENSE`) before making the repository public.
- Qt 6.8.3 is used under the LGPLv3 as unmodified shared libraries; the package ships the license
  texts and a notice with the source location (`third_party\`).
- The VC++ runtime DLLs are redistributable files from the MSVC 2022 toolset.
