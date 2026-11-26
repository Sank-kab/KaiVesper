# Generates nro/icon.jpg (256x256 hbmenu icon) and nro/source/font_aa.h (anti-aliased Segoe UI font)
Add-Type -AssemblyName System.Drawing

$Root = Split-Path -Parent $PSScriptRoot
$IconPath = Join-Path $Root "icon.jpg"
$FontHeaderPath = Join-Path $Root "source\font_aa.h"

# --- 1. Generate 256x256 KaiVesper hbmenu icon.jpg ---
$bmp = New-Object System.Drawing.Bitmap 256, 256
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# Dark midnight vertical gradient background
$rect = New-Object System.Drawing.Rectangle 0, 0, 256, 256
$bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $rect,
    [System.Drawing.Color]::FromArgb(255, 11, 16, 30),
    [System.Drawing.Color]::FromArgb(255, 22, 32, 56),
    [System.Drawing.Drawing2D.LinearGradientMode]::Vertical
)
$g.FillRectangle($bgBrush, $rect)

# Subtle top cyan/coral accent strip
$cyanBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 216, 255))
$coralBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 78, 114))
$g.FillRectangle($cyanBrush, 0, 0, 128, 7)
$g.FillRectangle($coralBrush, 128, 0, 128, 7)

# Helper for rounded rect path
function New-RoundPath($x, $y, $w, $h, $r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Soft glow behind Joy-Con emblem
$glowBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(32, 0, 216, 255))
$g.FillEllipse($glowBrush, 36, 18, 120, 120)
$glowBrush2 = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(32, 255, 78, 114))
$g.FillEllipse($glowBrush2, 100, 18, 120, 120)

# Dual Joy-Con emblem centered
$leftPath = New-RoundPath 70 36 50 112 22
$rightPath = New-RoundPath 136 36 50 112 22
$g.FillPath($cyanBrush, $leftPath)
$g.FillPath($coralBrush, $rightPath)

$darkBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 13, 19, 34))
$g.FillEllipse($darkBrush, 84, 58, 22, 22)
$g.FillEllipse($darkBrush, 150, 98, 22, 22)

# Bold "KaiVesper" title
$titleFont = New-Object System.Drawing.Font("Segoe UI", 28, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$subFont   = New-Object System.Drawing.Font("Segoe UI", 13, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$whiteBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 245, 248, 255))
$mutedBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 148, 168, 198))
$sf = New-Object System.Drawing.StringFormat
$sf.Alignment = [System.Drawing.StringAlignment]::Center

$g.DrawString("KaiVesper", $titleFont, $whiteBrush, (New-Object System.Drawing.RectangleF 0, 168, 256, 42), $sf)
$g.DrawString("by sank-kab", $subFont, $mutedBrush, (New-Object System.Drawing.RectangleF 0, 212, 256, 24), $sf)

# Save as baseline JPEG (quality 92)
$jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' } | Select-Object -First 1
$encParams = New-Object System.Drawing.Imaging.EncoderParameters 1
$encParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, 92L)
$bmp.Save($IconPath, $jpegCodec, $encParams)
$g.Dispose()
$bmp.Dispose()
Write-Host "[OK] Generated $IconPath (256x256 JPEG)"

# --- 2. Generate Anti-Aliased Proportional Segoe UI Font Header (font_aa.h) ---
function Build-FontData($name, $family, $pxSize, $style) {
    $font = New-Object System.Drawing.Font($family, $pxSize, $style, [System.Drawing.GraphicsUnit]::Pixel)
    $cellW = [int][Math]::Ceiling($pxSize * 1.1)
    $cellH = [int][Math]::Ceiling($pxSize * 1.32)
    $tmp = New-Object System.Drawing.Bitmap $cellW, $cellH
    $tg = [System.Drawing.Graphics]::FromImage($tmp)
    $tg.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

    $glyphs = @()
    $blob = New-Object System.Collections.Generic.List[byte]

    for ($code = 32; $code -le 126; $code++) {
        $ch = [char]$code
        $str = [string]$ch
        $tg.Clear([System.Drawing.Color]::Black)
        $sz = $tg.MeasureString($str, $font, 1000, $fmt)
        $adv = [int][Math]::Round($sz.Width)
        if ($code -eq 32) { $adv = [int][Math]::Max(4, [Math]::Round($pxSize * 0.30)) }
        if ($adv -lt 2) { $adv = 2 }

        $tg.DrawString($str, $font, [System.Drawing.Brushes]::White, 1, 0, $fmt)

        # Find tight bounding box of non-zero pixels
        $minX = $cellW; $minY = $cellH; $maxX = -1; $maxY = -1
        for ($y = 0; $y -lt $cellH; $y++) {
            for ($x = 0; $x -lt $cellW; $x++) {
                $px = $tmp.GetPixel($x, $y)
                if ($px.R -gt 4) {
                    if ($x -lt $minX) { $minX = $x }
                    if ($y -lt $minY) { $minY = $y }
                    if ($x -gt $maxX) { $maxX = $x }
                    if ($y -gt $maxY) { $maxY = $y }
                }
            }
        }

        if ($maxX -lt $minX) {
            $glyphs += [PSCustomObject]@{ off = $blob.Count; w = 0; h = 0; xoff = 0; yoff = 0; adv = $adv }
        } else {
            $gw = $maxX - $minX + 1
            $gh = $maxY - $minY + 1
            $off = $blob.Count
            for ($y = $minY; $y -le $maxY; $y++) {
                for ($x = $minX; $x -le $maxX; $x++) {
                    $blob.Add($tmp.GetPixel($x, $y).R)
                }
            }
            $glyphs += [PSCustomObject]@{ off = $off; w = $gw; h = $gh; xoff = ($minX - 1); yoff = $minY; adv = $adv }
        }
    }

    $tg.Dispose()
    $tmp.Dispose()
    $font.Dispose()

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("static const uint8_t ${name}_ALPHA[$($blob.Count)] = {")
    for ($i = 0; $i -lt $blob.Count; $i += 20) {
        $end = [Math]::Min($i + 19, $blob.Count - 1)
        $vals = for ($j = $i; $j -le $end; $j++) { $blob[$j] }
        [void]$sb.AppendLine("  " + ($vals -join ",") + ",")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("static const VesperGlyph ${name}_GLYPHS[95] = {")
    foreach ($gl in $glyphs) {
        [void]$sb.AppendLine("  {$($gl.off), $($gl.w), $($gl.h), $($gl.xoff), $($gl.yoff), $($gl.adv)},")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("static const VesperFont $name = {$cellH, ${name}_GLYPHS, ${name}_ALPHA};")
    return $sb.ToString()
}

$out = New-Object System.Text.StringBuilder
[void]$out.AppendLine("/* Auto-generated anti-aliased Segoe UI fonts for KaiVesper */")
[void]$out.AppendLine("/* SPDX-License-Identifier: GPL-2.0-only */")
[void]$out.AppendLine("#pragma once")
[void]$out.AppendLine("#include <stdint.h>")
[void]$out.AppendLine("")
[void]$out.AppendLine("typedef struct { uint32_t off; uint8_t w, h; int8_t xoff, yoff; uint8_t adv; } VesperGlyph;")
[void]$out.AppendLine("typedef struct { uint8_t line_h; const VesperGlyph* glyphs; const uint8_t* alpha; } VesperFont;")
[void]$out.AppendLine("")
[void]$out.Append((Build-FontData "FONT_SM" "Segoe UI" 18 ([System.Drawing.FontStyle]::Regular)))
[void]$out.AppendLine("")
[void]$out.Append((Build-FontData "FONT_MD" "Segoe UI" 22 ([System.Drawing.FontStyle]::Bold)))
[void]$out.AppendLine("")
[void]$out.Append((Build-FontData "FONT_LG" "Segoe UI" 30 ([System.Drawing.FontStyle]::Bold)))

[System.IO.File]::WriteAllText($FontHeaderPath, $out.ToString(), (New-Object System.Text.UTF8Encoding $false))
Write-Host "[OK] Generated $FontHeaderPath (anti-aliased Segoe UI 18px + 22px Bold + 30px Bold)"
