# Builds windows/agent/tethercam.ico from the app icon artwork (altstore/icon.png, the same image as the
# iPhone app icon), with Windows-style rounded corners. Sizes: 16, 20, 24, 32, 40, 48, 64, 256 (PNG-compressed).
#   powershell -File scripts\make-ico.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\.."
$src = [System.Drawing.Image]::FromFile((Join-Path $root "altstore\icon.png"))
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    # Rounded square (22% radius), like Windows 11 app icons.
    $r = [math]::Max(2, $s * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($s - 2 * $r, 0, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($s - 2 * $r, $s - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc(0, $s - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $g.SetClip($path)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose()
}
$src.Dispose()
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$dest = Join-Path $root "windows\agent\tethercam.ico"
[System.IO.File]::WriteAllBytes($dest, $out.ToArray())
Write-Host "wrote $dest ($($out.Length) bytes)"
