Add-Type -AssemblyName System.Drawing

# Palette lifted from src/ui_theme.cpp, so the artwork and the program agree.
$BASE    = [Drawing.Color]::FromArgb(24, 24, 27)
$SURFACE = [Drawing.Color]::FromArgb(34, 34, 39)
$TEXT    = [Drawing.Color]::FromArgb(233, 233, 238)
$DIM     = [Drawing.Color]::FromArgb(150, 150, 160)
$ACCENT  = [Drawing.Color]::FromArgb(214, 45, 50)
$HOT     = [Drawing.Color]::FromArgb(240, 74, 79)
$DOT     = [char]0x00B7

$SP   = 'C:\Users\Windows\AppData\Local\Temp\claude\C--Users-Windows-Desktop-zdisplay\768e961d-914a-43e7-a946-a662e5afa121\scratchpad'
$REPO = 'C:\Users\Windows\Desktop\zdisplay\zdisplay-repo'
$logo = [Drawing.Image]::FromFile("$SP\logo.png")

# ZDISPLAY as a 7-row block font, one column per character cell.
#
# Solid blocks only: the box-drawing "shadow" style depends on exact character
# cell alignment, and text rendering leaves hairline seams between rows that
# make the word look broken. Drawing the cells as rectangles tiles exactly.
$art = @(
#  Z      D      I    S      P      L      A      Y
 '11111 11110 111 01111 11110 10000 01110 10001',
 '00001 10001 010 10000 10001 10000 10001 10001',
 '00010 10001 010 10000 10001 10000 10001 01010',
 '00100 10001 010 01110 11110 10000 11111 00100',
 '01000 10001 010 00001 10000 10000 10001 00100',
 '10000 10001 010 10001 10000 10000 10001 00100',
 '11111 11110 111 01110 10000 11111 10001 00100'
)

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object Drawing.Bitmap($w, $h)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode      = 'AntiAlias'
    $g.TextRenderingHint  = 'AntiAliasGridFit'
    $g.InterpolationMode  = 'HighQualityBicubic'
    $g.PixelOffsetMode    = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'
    $g.FillRectangle((New-Object Drawing.SolidBrush($BASE)), 0, 0, $w, $h)
    return @($bmp, $g)
}

function Add-Glow($g, [single]$cx, [single]$cy, [single]$r, [int]$alpha, $color) {
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse(($cx - $r), ($cy - $r), ($r * 2), ($r * 2))
    $pgb = New-Object Drawing.Drawing2D.PathGradientBrush($path)
    $pgb.CenterPoint    = New-Object Drawing.PointF($cx, $cy)
    $pgb.CenterColor    = [Drawing.Color]::FromArgb($alpha, $color.R, $color.G, $color.B)
    $pgb.SurroundColors = @([Drawing.Color]::FromArgb(0, $color.R, $color.G, $color.B))
    $g.FillEllipse($pgb, ($cx - $r), ($cy - $r), ($r * 2), ($r * 2))
    $pgb.Dispose(); $path.Dispose()
}

# Draws the block-capital name centred on $cx, scaled so it spans $targetW.
# Each set cell becomes one rectangle, so neighbouring blocks share an edge and
# the word reads as solid rather than as rows of glyphs.
function Draw-Ascii($g, [int]$cx, [int]$top, [int]$targetW) {
    $cols = $art[0].Length
    $cell = $targetW / $cols
    $x0   = $cx - ($targetW / 2)
    $blockH = $cell * $art.Count

    # Hot at the top, settling into the accent at the baseline.
    $brush = New-Object Drawing.Drawing2D.LinearGradientBrush(
        (New-Object Drawing.PointF([single]$x0, [single]$top)),
        (New-Object Drawing.PointF([single]$x0, [single]($top + $blockH))),
        $HOT, $ACCENT)

    for ($r = 0; $r -lt $art.Count; $r++) {
        $row = $art[$r]
        for ($c = 0; $c -lt $cols; $c++) {
            if ($row[$c] -ne '1') { continue }
            # A half-pixel of overlap hides the seam antialiasing would leave
            # between two rectangles that only touch.
            $g.FillRectangle($brush,
                [single]($x0 + $c * $cell), [single]($top + $r * $cell),
                [single]($cell + 0.5),      [single]($cell + 0.5))
        }
    }
    $brush.Dispose()
    return ($top + $blockH)
}

function Draw-Center($g, $text, $font, $brush, [int]$cx, [int]$y) {
    $fmt = New-Object Drawing.StringFormat
    $fmt.Alignment = 'Center'
    $g.DrawString($text, $font, $brush, [single]$cx, [single]$y, $fmt)
}

function Add-Ramp($g, [int]$x, [int]$y, [int]$w, [int]$h, [bool]$lifted) {
    for ($i = 0; $i -lt $w; $i++) {
        $tone = ($i / $w) * 32.0 / 255.0
        if ($lifted) {
            $wgt  = [Math]::Pow(1.0 - $tone, 3) * (1.0 + 3.0 * $tone)
            $tone = [Math]::Pow($tone + 0.122 * $wgt, 0.78)
        }
        $v = [int][Math]::Min(255, $tone * 255)
        $p = New-Object Drawing.Pen([Drawing.Color]::FromArgb(255, $v, $v, $v))
        $g.DrawLine($p, ($x + $i), $y, ($x + $i), ($y + $h))
        $p.Dispose()
    }
}

$fS    = New-Object Drawing.Font('Segoe UI', 21, [Drawing.FontStyle]::Regular, 'Pixel')
$fXs   = New-Object Drawing.Font('Segoe UI', 15, [Drawing.FontStyle]::Regular, 'Pixel')
$fNum  = New-Object Drawing.Font('Segoe UI', 27, [Drawing.FontStyle]::Bold, 'Pixel')
$bT = New-Object Drawing.SolidBrush($TEXT)
$bD = New-Object Drawing.SolidBrush($DIM)
$bA = New-Object Drawing.SolidBrush($HOT)

# ---------------------------------------------------------------- README banner
#
# The wordmark is set in white here. The block-capital version lives in the
# README as text, and repeating it in the artwork made the page top read as the
# same word three times over.
$W = 1280; $HT = 420
$c = New-Canvas $W $HT; $bmp = $c[0]; $g = $c[1]

Add-Glow $g 196 210 210 54 $ACCENT
Add-Glow $g 196 210  96 58 $HOT
$g.DrawImage($logo, 116, 130, 160, 160)

$fTitle = New-Object Drawing.Font('Segoe UI Light', 74, [Drawing.FontStyle]::Regular, 'Pixel')
$tx = 344
$g.DrawString('Zdisplay', $fTitle, $bT, ($tx - 8), 92)
$g.FillRectangle((New-Object Drawing.SolidBrush($ACCENT)), $tx, 196, 84, 3)
$g.DrawString('Black equalizer, digital vibrance, brightness and color', $fS, $bD, $tx, 216)
$g.DrawString('temperature for Windows, in one tray app.',             $fS, $bD, $tx, 248)
$g.DrawString("any GPU   $DOT   with or without DDC/CI   $DOT   HDR displays   $DOT   fullscreen games",
              $fXs, $bA, $tx, 292)

$g.FillRectangle((New-Object Drawing.SolidBrush($SURFACE)), $tx, 330, 820, 1)
$stats = @(
    @{ v = '1.6 MB';  l = 'executable' },
    @{ v = '3.6 MB';  l = 'memory' },
    @{ v = '0%';      l = 'idle CPU' },
    @{ v = '404';     l = 'tests' },
    @{ v = 'GPL-3.0'; l = 'license' }
)
$sx = $tx
foreach ($s in $stats) {
    $g.DrawString($s.v, $fNum, $bT, $sx, 352)
    $g.DrawString($s.l, $fXs, $bD, ($sx + 2), 390)
    $sx += 166
}

$bmp.Save("$REPO\docs\img\banner.png", [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"banner.png  ${W}x${HT}"

# --------------------------------------------------------------- social preview
$W = 1280; $HT = 640
$c = New-Canvas $W $HT; $bmp = $c[0]; $g = $c[1]

Add-Glow $g 640 132 210 54 $ACCENT
Add-Glow $g 640 132  96 58 $HOT
$g.DrawImage($logo, 572, 64, 136, 136)

$fBig = New-Object Drawing.Font('Segoe UI Light', 92, [Drawing.FontStyle]::Regular, 'Pixel')
Draw-Center $g 'Zdisplay' $fBig $bT 640 222
# Clear of the descender on the "y": the rule sits below the whole word.
$g.FillRectangle((New-Object Drawing.SolidBrush($ACCENT)), 598, 364, 84, 3)

Draw-Center $g 'Black equalizer, digital vibrance, brightness and color temperature for Windows' $fS $bD 640 386
Draw-Center $g "any GPU   $DOT   with or without DDC/CI   $DOT   HDR displays   $DOT   fullscreen games" $fXs $bA 640 424

Draw-Center $g 'near-black tones, untouched' $fXs $bD 640 474
Add-Ramp $g 200 498 880 28 $false
Draw-Center $g "with Shadows 78 + Clarity 65   $DOT   27 of 32 tones stay distinct" $fXs $bA 640 540
Add-Ramp $g 200 564 880 28 $true

$bmp.Save("$REPO\.github\social-preview.png", [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
$logo.Dispose()
"social-preview.png  ${W}x${HT}"
