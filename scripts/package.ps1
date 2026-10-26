<#
.SYNOPSIS
  Build the GPU Fusion release artifacts and verify them.

      G:\GPUFusion\scripts\package.ps1             # build (with tests) + package + verify
      G:\GPUFusion\scripts\package.ps1 -SkipBuild  # package the existing release build

  Produces, under <repo>\release:
    GPUFusion\                   portable application folder (windeployqt + app-local VC++ runtime)
    GPUFusion.exe                launcher: double-click starts GPUFusion\GPUFusion.exe
    GPUFusion-Windows-x64.zip    the portable folder, zipped
    GPUFusion-Setup.exe          per-user installer (NSIS), no administrator rights needed
    SHA256SUMS.txt               checksums of the ZIP and the installer

  Verification (everything under <repo>\temp, nothing on C:): the extracted ZIP and a test install
  (installer /S /TESTINSTALL: files only, no shortcuts, no registry) each pass a launch smoke test;
  the test install is then removed with its uninstaller.
#>
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# Runs a native tool; success is judged by its exit code. (Windows PowerShell 5.1 turns any stderr
# line into an error record, which 'Stop' would make fatal even for harmless warnings.)
function Invoke-Native([string]$what, [scriptblock]$command) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $command 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) {
        $output | ForEach-Object { Write-Host "  $_" }
        throw "$what failed (exit $code)"
    }
}
. (Join-Path $PSScriptRoot 'env.ps1') -Quiet

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}
$bin = Join-Path $repo 'build\release\bin'
$version = (Select-String -Path (Join-Path $repo 'CMakeLists.txt') -Pattern '^\s+VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$release = Join-Path $repo 'release'
$portable = Join-Path $release 'GPUFusion'
$zip = Join-Path $release 'GPUFusion-Windows-x64.zip'
$setup = Join-Path $release 'GPUFusion-Setup.exe'
$sums = Join-Path $release 'SHA256SUMS.txt'
Write-Host "Packaging GPU Fusion $version"

# ---- portable folder (regenerated from scratch; only ever inside <repo>\release) ------------------
New-Item -ItemType Directory -Force -Path $release | Out-Null
# The packaged app (e.g. started from the desktop shortcut) keeps its settings and results in
# release\GPUFusion\data. Keep that data: move it aside while packaging (so it is never zipped or
# installed) and put it back at the end.
$keptData = $null
if (Test-Path (Join-Path $portable 'data')) {
    $keptData = Join-Path $release ("data-kept-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Move-Item -LiteralPath (Join-Path $portable 'data') -Destination $keptData
    Write-Host "Kept the app's data folder aside: $keptData"
}
function Restore-KeptData {
    if ($script:keptData -and (Test-Path -LiteralPath $script:keptData)) {
        New-Item -ItemType Directory -Force -Path $portable | Out-Null
        Move-Item -LiteralPath $script:keptData -Destination (Join-Path $portable 'data')
        Write-Host "Restored the app's data folder."
        $script:keptData = $null
    }
}
# Whatever fails below (the development disk is unreliable), the user's data goes back in place.
trap { Restore-KeptData; break }

# Starts a program and waits for it, but never forever: a launch that hangs (e.g. on unreadable
# sectors of a failing disk) fails the check instead of blocking packaging.
function Wait-Program([string]$exe, [string[]]$arguments, [int]$seconds, [string]$what) {
    $p = Start-Process $exe -ArgumentList $arguments -PassThru
    if (-not $p.WaitForExit($seconds * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "$what did not finish within $seconds s"
    }
    return $p.ExitCode
}
if (Test-Path $portable) { Remove-Item -LiteralPath $portable -Recurse -Force }
New-Item -ItemType Directory -Path $portable | Out-Null
Copy-Item (Join-Path $bin 'GPUFusion.exe'), (Join-Path $bin 'GPUFusionProbe.exe') $portable
# Same selection as the build tree (cmake/Deploy.cmake): Widgets app, Windows platform, ico/png.
Invoke-Native 'windeployqt' {
    windeployqt --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw `
        --no-compiler-runtime --no-network --no-svg --skip-plugin-types generic,networkinformation,tls,iconengines `
        --exclude-plugins qgif,qjpeg --verbose 0 (Join-Path $portable 'GPUFusion.exe')
}
# App-local VC++ runtime, exactly the files the build deployed (ucrtbase is part of Windows 10/11).
Get-ChildItem $bin -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp140|vcruntime140)' } | Copy-Item -Destination $portable
Copy-Item (Join-Path $repo 'CHANGELOG.md') $portable
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $portable 'LICENSE.txt')  # GPU Fusion: MIT
# Qt is used under the LGPLv3: ship the notices and the license texts with the DLLs.
Copy-Item (Join-Path $repo 'third_party\THIRD-PARTY-NOTICES.txt') $portable
Copy-Item (Join-Path $repo 'third_party\licenses') (Join-Path $portable 'licenses') -Recurse
# User documentation, shown by the app's "Open user guide" (links between these documents work).
New-Item -ItemType Directory -Force (Join-Path $portable 'docs') | Out-Null
foreach ($doc in 'USER_GUIDE.md', 'FINAL_REPORT.md', 'GPU_SELECTION.md', 'MULTI_GPU_RESEARCH.md', 'TROUBLESHOOTING.md', 'SDK.md', 'COMPATIBILITY.md') {
    Copy-Item (Join-Path $repo "docs\$doc") (Join-Path $portable "docs\$doc")
}
Copy-Item (Join-Path $repo 'README.md') $portable
# Optional SDK for programs that want to follow GPU Fusion's GPU choice (header-only, MIT).
New-Item -ItemType Directory -Force (Join-Path $portable 'sdk') | Out-Null
Copy-Item (Join-Path $repo 'sdk\include') (Join-Path $portable 'sdk\include') -Recurse
Copy-Item (Join-Path $repo 'docs\SDK.md') (Join-Path $portable 'sdk\SDK.md')
@"
GPU Fusion $version

Start GPUFusion.exe. No installation is needed: settings, logs and benchmark results are kept in the
"data" folder created next to GPUFusion.exe (or in %LOCALAPPDATA%\GPUFusion if this folder is not
writable). GPU Fusion installs no services or drivers and changes no system settings on its own.

Requirements: Windows 10 or 11, 64-bit, a DirectX 11 capable graphics adapter.
Documentation and source: https://github.com/etcofficials/GPUFusion
"@ | Set-Content -Encoding utf8 (Join-Path $portable 'README.txt')
if (Test-Path (Join-Path $portable 'GPUFusion.datadir')) { throw "Development data pointer must not be shipped" }

# ---- launcher next to the folder ---------------------------------------------------------------
Copy-Item (Join-Path $bin 'GPUFusionLauncher.exe') (Join-Path $release 'GPUFusion.exe') -Force

# ---- ZIP ------------------------------------------------------------------------------------------
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path $portable -DestinationPath $zip -CompressionLevel Optimal

# ---- installer -------------------------------------------------------------------------------------
$makensis = 'G:\DevTools\nsis\makensis.exe'
if (Test-Path $makensis) {
    Invoke-Native 'makensis' {
        & $makensis /V2 "/DVERSION=$version" "/DSRCDIR=$portable" "/DOUTFILE=$setup" "/DICON=$(Join-Path $repo 'resources\gpufusion.ico')" `
            (Join-Path $repo 'installer\GPUFusion.nsi')
    }
} else {
    Write-Warning "NSIS not found at $makensis; installer skipped"
}

# ---- checksums -------------------------------------------------------------------------------------
$lines = foreach ($f in @($zip, $setup)) {
    if (Test-Path $f) { "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 $f).Hash.ToLowerInvariant(), (Split-Path -Leaf $f) }
}
# LF line endings: `sha256sum -c SHA256SUMS.txt` (Git Bash, Linux, macOS) rejects CRLF file names.
[IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"), [Text.Encoding]::ASCII)

# ---- verification ---------------------------------------------------------------------------------
function Test-Launch([string]$exe, [string]$what) {
    $code = Wait-Program $exe @('--smoke-test=1500') 60 $what
    if ($code -ne 0) { throw "$what failed its launch smoke test (exit $code)" }
    Write-Host "  $what starts: OK"
}
$verify = Join-Path $repo 'temp\package-verify'
if (Test-Path $verify) { Remove-Item -LiteralPath $verify -Recurse -Force }
New-Item -ItemType Directory -Path $verify | Out-Null
Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $verify 'zip')
$fromZip = Join-Path $verify 'zip\GPUFusion\GPUFusion.exe'
Test-Launch $fromZip 'Portable app from the ZIP'
if (-not (Test-Path (Join-Path $verify 'zip\GPUFusion\data\logs'))) { throw "Portable app did not create its data folder next to the executable" }
Write-Host "  Portable data folder next to the executable: OK"
# The launcher, placed next to the extracted folder exactly as in <repo>\release.
Copy-Item (Join-Path $release 'GPUFusion.exe') (Join-Path $verify 'zip\GPUFusion.exe')
$code = Wait-Program (Join-Path $verify 'zip\GPUFusion.exe') @('--smoke-test=1500') 30 'The launcher'
if ($code -ne 0) { throw "Launcher failed (exit $code)" }
$log = Join-Path $verify 'zip\GPUFusion\data\logs\gpufusion.log'
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and @(Select-String -LiteralPath $log -Pattern 'Smoke test passed').Count -lt 2) { Start-Sleep -Milliseconds 500 }
if (@(Select-String -LiteralPath $log -Pattern 'Smoke test passed').Count -lt 2) { throw "The launcher did not start the application" }
Write-Host "  Launcher starts the application (with arguments): OK"
if (Test-Path $setup) {
    $target = Join-Path $verify 'installed'
    $code = Wait-Program $setup @('/S', '/TESTINSTALL', "/D=$target") 180 'The installer'
    if ($code -ne 0) { throw "Installer failed (exit $code)" }
    Test-Launch (Join-Path $target 'GPUFusion.exe') 'Test install'
    $uninst = Join-Path $target 'Uninstall GPU Fusion.exe'
    # NSIS uninstallers copy themselves to TEMP and return at once; _?= runs in place and waits.
    $code = Wait-Program $uninst @('/S', "_?=$target") 120 'The uninstaller'
    if ($code -ne 0) { throw "Uninstaller failed (exit $code)" }
    $left = @(Get-ChildItem $target -Recurse -File | Where-Object { $_.Name -ne 'Uninstall GPU Fusion.exe' -and $_.FullName -notlike "*\data\*" })
    if ($left.Count) { throw "Uninstaller left program files behind: $($left.FullName -join ', ')" }
    Write-Host "  Uninstall removes the program files (keeps data unless asked): OK"
}
Remove-Item -LiteralPath $verify -Recurse -Force
Restore-KeptData

Write-Host ""
Get-ChildItem $release -File | ForEach-Object { "{0,-28} {1,10:N0} KB" -f $_.Name, ($_.Length / 1KB) }
"{0,-28} {1,10:N0} KB ({2} files)" -f 'GPUFusion\', ((Get-ChildItem $portable -Recurse -File | Measure-Object Length -Sum).Sum / 1KB), (Get-ChildItem $portable -Recurse -File).Count
Get-Content $sums
