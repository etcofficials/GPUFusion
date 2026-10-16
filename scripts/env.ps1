<#
.SYNOPSIS
  Initialise the GPU Fusion development environment in the CURRENT PowerShell session.

      . G:\GPUFusion\scripts\env.ps1

  Configures, for this session only (nothing is written to the registry or the system PATH):
    - portable MSVC + Windows SDK   (G:\DevTools\setup-msvc.ps1)
    - CMake, Ninja                  (G:\DevTools\CMake, G:\DevTools\Ninja)
    - Qt 6 msvc2022_64              (newest 6.x under G:\Qt)
    - TEMP/TMP                      (G:\GPUFusion\temp, keeps compiler and test temp files off C:)
    - GPUFUSION_* variables consumed by CMakePresets.json
#>
[CmdletBinding()]
param(
    [string]$DevTools = 'G:\DevTools',
    [string]$QtRoot   = 'G:\Qt',
    [switch]$Quiet
)
# Dot-sourced: do not leave a stricter $ErrorActionPreference behind in the caller's session.
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if ($env:GPUFUSION_ENV -eq '1') {
    if (-not $Quiet) { Write-Host 'GPU Fusion environment already active.' }
    return
}

$setupMsvc = Join-Path $DevTools 'setup-msvc.ps1'
if (-not (Test-Path $setupMsvc)) { throw "Portable MSVC environment script not found: $setupMsvc" }
. $setupMsvc -DevTools $DevTools -Quiet:$Quiet

$cmakeBin = Join-Path $DevTools 'CMake\bin'
$ninjaDir = Join-Path $DevTools 'Ninja'
foreach ($p in @($cmakeBin, $ninjaDir)) {
    if (-not (Test-Path $p)) { throw "Missing build tool directory: $p" }
}

$qtPrefix = Get-ChildItem $QtRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^6\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'msvc2022_64\bin\Qt6Core.dll')) } |
            Sort-Object { [version]$_.Name } | Select-Object -Last 1 |
            ForEach-Object { Join-Path $_.FullName 'msvc2022_64' }
if (-not $qtPrefix) { throw "No Qt 6 msvc2022_64 build found under $QtRoot" }

$env:PATH = "$cmakeBin;$ninjaDir;$qtPrefix\bin;" + $env:PATH

$temp = Join-Path $repo 'temp'
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$env:TEMP = $temp
$env:TMP  = $temp

$env:GPUFUSION_ROOT      = $repo
$env:GPUFUSION_QT_PREFIX = $qtPrefix
$env:CMAKE_GENERATOR     = 'Ninja'
$env:GPUFUSION_ENV       = '1'
$ErrorActionPreference   = $previousErrorAction

if (-not $Quiet) {
    Write-Host ("CMake         " + ((& "$cmakeBin\cmake.exe" --version | Select-Object -First 1) -replace 'cmake version ', ''))
    Write-Host ("Ninja         " + (& "$ninjaDir\ninja.exe" --version))
    Write-Host ("Qt            " + (& "$qtPrefix\bin\qtpaths.exe" --qt-version) + "  ($qtPrefix)")
    Write-Host ("TEMP          " + $temp)
    Write-Host 'Ready. Build:  cmake --preset release ; cmake --build --preset release ; ctest --preset release'
}
