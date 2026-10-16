<#
.SYNOPSIS
  Configure, build and test GPU Fusion in one step.

      G:\GPUFusion\scripts\build.ps1                 # release build + tests
      G:\GPUFusion\scripts\build.ps1 -Preset debug
      G:\GPUFusion\scripts\build.ps1 -SkipTests

  Output: build\<preset>\bin\GPUFusion.exe (runnable directly; Qt and the MSVC runtime are
  deployed next to it). Exits non-zero if any step fails.

  Damaged build outputs: on the development PC the G: disk has failing sectors and a bad SATA link
  (see docs/TROUBLESHOOTING.md), which sometimes corrupts freshly written intermediate files. When
  the build fails with one of those specific errors, the named intermediate file is deleted and the
  build retried (up to -Retries times). Source files are never touched; tests then verify the result.
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')][string]$Preset = 'release',
    [switch]$SkipTests,
    [int]$Retries = 3
)
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1') -Quiet

# Errors that mean "an intermediate file is unreadable/corrupt", not "the code is wrong".
$corruptionPattern = 'LNK1104|LNK1136|LNK1201|LNK1285|C2471|C1090|cmake_ninja_dyndep failed to parse|invalid or corrupt file'

function Remove-DamagedOutputs([string[]]$lines, [string]$buildDir) {
    $removed = @()
    foreach ($line in $lines) {
        if ($line -notmatch $corruptionPattern) { continue }
        foreach ($m in [regex]::Matches($line, "([A-Za-z]:\\[^'`"\s]+|[\w\\/.\-]+)\.(obj|ddi|pdb|lib|modmap)")) {
            $p = $m.Value
            if (-not [IO.Path]::IsPathRooted($p)) { $p = Join-Path $buildDir $p }
            $full = [IO.Path]::GetFullPath($p)
            # Only ever delete generated files inside the build directory.
            if ($full.StartsWith([IO.Path]::GetFullPath($buildDir), [StringComparison]::OrdinalIgnoreCase) -and (Test-Path $full)) {
                Remove-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
                $removed += $full
            }
        }
    }
    return $removed
}

# Independent staleness check: an object file older than any header/source it was compiled from is
# out of date, whatever Ninja's own (possibly damaged) records say. Returns the stale objects.
# (Found necessary after MainWindow.cpp.obj was not rebuilt after DiagnosticsPage.h changed,
# producing a heap-corrupting size mismatch.)
function Find-StaleObjects([string]$buildDir) {
    $stale = @()
    $current = $null; $objTime = $null; $reason = $null
    $mtimes = @{}
    foreach ($line in (ninja -C $buildDir -t deps 2>$null)) {
        if ($line -match '^(\S.*): #deps') {
            if ($current -and $reason) { $stale += $current; Write-Host "  stale: $current ($reason)" }
            # Other entries (e.g. AUTOGEN timestamps, which depend on CMakeLists.txt) end the previous
            # object's list but are not checked themselves.
            $target = $matches[1]
            $current = if ($target.EndsWith('.obj')) { Join-Path $buildDir $target } else { $null }
            $objTime = if ($current -and (Test-Path -LiteralPath $current)) { (Get-Item -LiteralPath $current).LastWriteTimeUtc } else { $null }
            $reason = $null
        } elseif ($current -and $objTime -and -not $reason -and $line -match '^\s+(\S.*)$') {
            $dep = $matches[1]
            if (-not [IO.Path]::IsPathRooted($dep)) { $dep = Join-Path $buildDir $dep }
            if (-not $mtimes.ContainsKey($dep)) {
                $mtimes[$dep] = if (Test-Path -LiteralPath $dep) { (Get-Item -LiteralPath $dep).LastWriteTimeUtc } else { [datetime]::MaxValue }
            }
            if ($mtimes[$dep] -gt $objTime) {
                $reason = if ($mtimes[$dep] -eq [datetime]::MaxValue) { "dependency missing: $dep" }
                          else { "{0} is newer: {1:HH:mm:ss.fff} > {2:HH:mm:ss.fff} UTC" -f $dep, $mtimes[$dep], $objTime }
            }
        }
    }
    if ($current -and $reason) { $stale += $current; Write-Host "  stale: $current ($reason)" }
    return $stale
}

Push-Location $repo
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    $buildDir = Join-Path $repo "build\$Preset"
    for ($attempt = 1; ; $attempt++) {
        $output = cmake --build --preset $Preset 2>&1 | ForEach-Object { "$_" }
        $output | Where-Object { $_ -match 'error|warning C|FAILED' } | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -eq 0) {
            $stale = @(Find-StaleObjects $buildDir)
            if ($stale.Count -eq 0) { break }
            if ($attempt -gt $Retries) { throw "Objects remain out of date after $Retries rebuilds: $($stale -join ', ')" }
            Write-Warning ("{0} object file(s) older than their sources despite a successful build; rebuilding them: {1}" -f $stale.Count, ($stale -join ', '))
            $stale | ForEach-Object { Remove-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue }
            continue
        }
        $removed = Remove-DamagedOutputs $output $buildDir
        if ($removed.Count -eq 0 -or $attempt -gt $Retries) { throw "Build failed ($LASTEXITCODE)" }
        Write-Warning ("Damaged build output (attempt {0}); removed and rebuilding: {1}" -f $attempt, ($removed -join ', '))
    }
    if (-not $SkipTests) {
        ctest --preset $Preset --timeout 180
        if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    }
    Write-Host "OK: $(Join-Path $repo "build\$Preset\bin\GPUFusion.exe")"
} finally {
    Pop-Location
}
