# Builds scratter.ico from docs/assets/scratter-logo.png: one PNG image per
# size Windows asks for (taskbar, title bar, Explorer at each scale), so it
# never has to shrink the 256 px one itself. Run it again after changing the
# logo:
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\make-icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$source = Join-Path $root 'docs\assets\scratter-logo.png'
$target = Join-Path $PSScriptRoot 'scratter.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

$logo = [System.Drawing.Image]::FromFile($source)
$images = foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($logo, 0, 0, $size, $size)
    $graphics.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    , $stream.ToArray()
}
$logo.Dispose()

# ICO layout: a 6-byte header, a 16-byte entry per image, then the images.
$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $out
$writer.Write([uint16]0)              # reserved
$writer.Write([uint16]1)              # 1 = icon
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $side = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }  # 0 means 256
    $writer.Write([byte]$side)            # width
    $writer.Write([byte]$side)            # height
    $writer.Write([byte]0)                # no palette
    $writer.Write([byte]0)                # reserved
    $writer.Write([uint16]1)              # colour planes
    $writer.Write([uint16]32)             # bits per pixel
    $writer.Write([uint32]$images[$i].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($target, $out.ToArray())
Write-Output "wrote $target ($($out.Length) bytes, $($sizes.Count) sizes)"
