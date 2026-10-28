<#
.SYNOPSIS
  Builds GPU Fusion for Godot (sdk/godot: a GDExtension over the GPU Fusion SDK) and runs its headless
  check with Godot.

      .\scripts\build-godot.ps1 [-GodotCpp G:\DevTools\godot\godot-cpp] [-Godot G:\DevTools\godot\Godot_v4.7.2-stable_win64_console.exe]
                                [-SkipCheck]

  Needs: a godot-cpp checkout (branch 4.5; extensions built for 4.5 load in later 4.x), a Godot 4.5+
  executable for the check, and GPU Fusion's release build (for the compiled SDK shaders). Everything
  stays on G:; nothing is installed.
#>
[CmdletBinding()]
param(
    [string]$GodotCpp = 'G:\DevTools\godot\godot-cpp',
    [string]$Godot = 'G:\DevTools\godot\Godot_v4.7.2-stable_win64_console.exe',
    [switch]$SkipCheck
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1') -Quiet
$generated = Join-Path $repo 'build\release\generated'
$buildDir = Join-Path $repo 'build\godot'
& cmake -S (Join-Path $repo 'sdk\godot') -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGODOT_CPP_DIR=$GodotCpp" "-DGPUFUSION_GENERATED_DIR=$generated"
if ($LASTEXITCODE -ne 0) { throw "Configuring the Godot extension failed" }
& cmake --build $buildDir --target gpufusion_godot
if ($LASTEXITCODE -ne 0) { throw "Building the Godot extension failed" }
$dll = Join-Path $repo 'sdk\godot\demo\bin\gpufusion_godot.dll'
Write-Host "Built $dll"
if ($SkipCheck) { return }
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot" }
# --import once so Godot registers the extension (creates demo\.godot), then the check.
& $Godot --headless --path (Join-Path $repo 'sdk\godot\demo') --import 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Godot could not import the demo project with the extension (exit $LASTEXITCODE)" }
& $Godot --headless --path (Join-Path $repo 'sdk\godot\demo') --script res://check.gd
exit $LASTEXITCODE
