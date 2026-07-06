# Capture the TombaRecomp game window's client area to a PNG.
# Usage: powershell -File tools\capture_game_window.ps1 -Out _shots\name.png [-Foreground]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [switch]$Foreground
)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public class TgwCap {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, StringBuilder t, int max);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
  public static IntPtr Found = IntPtr.Zero;
}
'@
[TgwCap]::Found = [IntPtr]::Zero
[TgwCap]::EnumWindows({ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [TgwCap]::GetWindowText($h, $sb, 256) | Out-Null
    if ($sb.ToString() -like "*ApeEscape*") { [TgwCap]::Found = $h; return $false }
    $true
}, [IntPtr]::Zero) | Out-Null
$h = [TgwCap]::Found
if ($h -eq [IntPtr]::Zero) { Write-Error "game window not found"; exit 1 }
if ($Foreground) { [TgwCap]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 500 }
$r = New-Object TgwCap+RECT; [TgwCap]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object TgwCap+POINT; $p.X = 0; $p.Y = 0; [TgwCap]::ClientToScreen($h, [ref]$p) | Out-Null
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("captured {0}x{1} -> {2}" -f $w, $hh, $Out)
