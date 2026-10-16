<#
.SYNOPSIS
  Create the desktop shortcut "GPU Fusion.lnk" for the packaged release.

      G:\GPUFusion\scripts\make-shortcut.ps1

  If a "GPU Fusion.lnk" already exists on the desktop it is replaced only when it points to this
  repository's GPU Fusion; a shortcut to anything else is left untouched (and reported).
#>
[CmdletBinding()]
param([string]$Target = (Join-Path (Split-Path -Parent $PSScriptRoot) 'release\GPUFusion\GPUFusion.exe'))
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Target)) { throw "Not found: $Target (run scripts\package.ps1 first)" }
$desktop = [Environment]::GetFolderPath('Desktop')
$link = Join-Path $desktop 'GPU Fusion.lnk'
$shell = New-Object -ComObject WScript.Shell
if (Test-Path $link) {
    $existing = $shell.CreateShortcut($link).TargetPath
    $ours = $existing -and ([IO.Path]::GetFullPath($existing) -ieq [IO.Path]::GetFullPath($Target) -or
                            $existing -like (Join-Path (Split-Path -Parent $PSScriptRoot) '*'))
    if (-not $ours) {
        Write-Warning "A different 'GPU Fusion.lnk' already exists on the desktop (target: $existing). It was left unchanged."
        exit 2
    }
}
$s = $shell.CreateShortcut($link)
$s.TargetPath = $Target
$s.WorkingDirectory = Split-Path -Parent $Target
$s.IconLocation = "$Target,0"
$s.Description = 'GPU Fusion - GPU monitor, preferences, benchmarks and multi-GPU lab'
$s.Save()
Write-Host "Shortcut: $link -> $Target"
