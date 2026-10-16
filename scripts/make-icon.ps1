<#
.SYNOPSIS
  Regenerates resources\gpufusion.ico and resources\gpufusion-256.png with System.Drawing.
  The design: a dark rounded tile with two overlapping GPU "chips" (teal and blue) and pins.
  The generated files are committed; run this only when changing the icon.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'resources'

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); return $p
}

function Draw-Chip($g, [single]$s, [single]$cx, [single]$cy, [single]$size, $color) {
    $pin = New-Object System.Drawing.SolidBrush($color)
    $pw = [Math]::Max(1, $size * 0.09); $pl = $size * 0.14
    foreach ($i in 0..2) {
        $o = $cx - $size * 0.30 + $i * $size * 0.30 - $pw / 2
        $g.FillRectangle($pin, $o, $cy - $size / 2 - $pl, $pw, $pl)
        $g.FillRectangle($pin, $o, $cy + $size / 2, $pw, $pl)
        $g.FillRectangle($pin, $cx - $size / 2 - $pl, $o - $cx + $cy, $pl, $pw)
        $g.FillRectangle($pin, $cx + $size / 2, $o - $cx + $cy, $pl, $pw)
    }
    $body = New-RoundedPath ($cx - $size / 2) ($cy - $size / 2) $size $size ($size * 0.16)
    $g.FillPath((New-Object System.Drawing.SolidBrush($color)), $body)
    $core = $size * 0.42
    $inner = New-RoundedPath ($cx - $core / 2) ($cy - $core / 2) $core $core ($core * 0.18)
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235, 18, 22, 30))), $inner)
}

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.Clear([System.Drawing.Color]::Transparent)
    $m = $s * 0.03
    $bg = New-RoundedPath $m $m ($s - 2 * $m) ($s - 2 * $m) ($s * 0.2)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)), (New-Object System.Drawing.PointF($s, $s)),
        [System.Drawing.Color]::FromArgb(255, 36, 44, 60), [System.Drawing.Color]::FromArgb(255, 16, 20, 28))
    $g.FillPath($grad, $bg)
    Draw-Chip $g $s ($s * 0.40) ($s * 0.40) ($s * 0.36) ([System.Drawing.Color]::FromArgb(255, 46, 196, 182))
    Draw-Chip $g $s ($s * 0.61) ($s * 0.61) ($s * 0.36) ([System.Drawing.Color]::FromArgb(255, 76, 141, 255))
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $bmp.Save((Join-Path $out 'gpufusion-256.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$images = foreach ($s in $sizes) { ,(Render $s) }

# ICO container with PNG-compressed entries (supported since Windows Vista).
$fs = [System.IO.File]::Create((Join-Path $out 'gpufusion.ico'))
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Close()
Write-Host "Wrote $(Join-Path $out 'gpufusion.ico') ($($sizes -join ', ') px)"
