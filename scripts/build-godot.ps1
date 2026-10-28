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
    # Optional: the API of a specific Godot version (Godot --dump-extension-api); the extension then
    # requires that version or later.
    [string]$ApiFile = '',
    [switch]$SkipCheck
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1') -Quiet
$generated = Join-Path $repo 'build\release\generated'
$buildDir = Join-Path $repo $(if ($ApiFile) { 'build\godot-custom-api' } else { 'build\godot' })
$apiArg = if ($ApiFile) { "-DGODOTCPP_CUSTOM_API_FILE=$ApiFile" } else { '-DGODOTCPP_CUSTOM_API_FILE=' }
& cmake -S (Join-Path $repo 'sdk\godot') -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGODOT_CPP_DIR=$GodotCpp" "-DGPUFUSION_GENERATED_DIR=$generated" $apiArg
if ($LASTEXITCODE -ne 0) { throw "Configuring the Godot extension failed" }
& cmake --build $buildDir --target gpufusion_godot
if ($LASTEXITCODE -ne 0) { throw "Building the Godot extension failed" }
$dll = Join-Path $repo 'sdk\godot\demo\bin\gpufusion_godot.dll'
Write-Host "Built $dll"
if ($SkipCheck) { return }
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot" }
# Godot keeps a project's user:// folder under %APPDATA%: point it at G: for these runs (this process only).
$env:APPDATA = Join-Path (Split-Path -Parent $Godot) 'appdata'
New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
# --import so Godot registers the extension (creates demo\.godot). Godot 4.7.2 crashes on exit after the FIRST
# headless import of any project with a GDExtension (an empty extension does it too); the second import works.
& $Godot --headless --path (Join-Path $repo 'sdk\godot\demo') --import 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { & $Godot --headless --path (Join-Path $repo 'sdk\godot\demo') --import 2>&1 | Out-Null }
if ($LASTEXITCODE -ne 0) { throw "Godot could not import the demo project with the extension (exit $LASTEXITCODE)" }
& $Godot --headless --path (Join-Path $repo 'sdk\godot\demo') --script res://check.gd
exit $LASTEXITCODE
