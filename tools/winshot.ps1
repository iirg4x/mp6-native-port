# winshot.ps1 -- captures the most recently started $Process window's own
# composed content via PrintWindow (PW_CLIENTONLY | PW_RENDERFULLCONTENT), so
# the capture is correct even when other windows overlap it on screen (a
# plain screen-rect copy would grab whatever else is on top instead).
param(
    [string]$Process = "mp6native",
    [string]$Out = "build\winshot.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinShot2 {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { Write-Host "[winshot2] no window for process '$Process'"; exit 1 }
$h = $proc.MainWindowHandle
$r = New-Object WinShot2+RECT
[WinShot2]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Host "[winshot2] degenerate rect"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# PW_CLIENTONLY(1) | PW_RENDERFULLCONTENT(2) = 3: client area, DWM-composed
$ok = [WinShot2]::PrintWindow($h, $hdc, 3)
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) { Write-Host "[winshot2] PrintWindow failed"; exit 1 }
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "[winshot2] saved $Out ($w x $ht)"
