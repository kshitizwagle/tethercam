# Renders the Tethercam app icon (1024x1024, opaque) to the given PNG paths.
#   powershell -File scripts\make-icon.ps1 ios\Tethercam\Assets.xcassets\AppIcon.appiconset\icon.png altstore\icon.png
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Out)
Add-Type -AssemblyName System.Drawing
$size = 1024
$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
# Background: vertical green gradient.
$rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(52, 199, 116)), ([System.Drawing.Color]::FromArgb(22, 128, 74)), 90
$g.FillRectangle($bg, $rect)
$white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
$green = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(30, 150, 88))
# Camera body (rounded rectangle) and viewfinder triangle.
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$x = 190; $y = 330; $w = 470; $h = 330; $r = 70
$path.AddArc($x, $y, $r * 2, $r * 2, 180, 90)
$path.AddArc($x + $w - $r * 2, $y, $r * 2, $r * 2, 270, 90)
$path.AddArc($x + $w - $r * 2, $y + $h - $r * 2, $r * 2, $r * 2, 0, 90)
$path.AddArc($x, $y + $h - $r * 2, $r * 2, $r * 2, 90, 90)
$path.CloseFigure()
$g.FillPath($white, $path)
$g.FillPolygon($white, [System.Drawing.Point[]]@((New-Object System.Drawing.Point 690, 495), (New-Object System.Drawing.Point 840, 395), (New-Object System.Drawing.Point 840, 595)))
# Lens.
$g.FillEllipse($green, 330, 400, 190, 190)
$g.FillEllipse($white, 385, 455, 80, 80)
# Cable below the camera.
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 44
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawBezier($pen, 425, 660, 425, 800, 640, 760, 640, 880)
$g.Dispose()
foreach ($o in $Out) {
    $dir = Split-Path $o -Parent
    if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $bmp.Save($o, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "wrote $o"
}
$bmp.Dispose()
