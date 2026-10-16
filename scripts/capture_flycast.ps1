# Capture every top-level window of the Flycast process (game view + serial output) to PNGs.
# Usage: powershell -File capture_flycast.ps1 <out-prefix>   -> <prefix>_w0.png, _w1.png ...
# The serial console is captured with CopyFromScreen (PrintWindow can hang on consoles);
# if the console is in text-selection mode ("選択"/"Select" title), Escape is sent first.
param([string]$Out = "D:\DC\bleem2\build\flycast_shot", [int]$FlycastPid = 0)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Cap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static List<IntPtr> Find(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Title(IntPtr h) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); return sb.ToString(); }
}
"@

if ($FlycastPid -gt 0) { $p = Get-Process -Id $FlycastPid -ErrorAction SilentlyContinue } else { $p = Get-Process flycast -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $p) { Write-Host "no flycast process"; exit 1 }
$wins = [Win32Cap]::Find([uint32]$p.Id)
$i = 0
foreach ($h in $wins) {
    $title = [Win32Cap]::Title($h)
    if ($title -like "選択*" -or $title -like "Select *") {
        [Win32Cap]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 150
        [Win32Cap]::keybd_event(0x1B, 1, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
        [Win32Cap]::keybd_event(0x1B, 1, 2, [UIntPtr]::Zero)
        Write-Host "released console selection mode"
        Start-Sleep -Milliseconds 200
    }
    $r = New-Object Win32Cap+RECT
    [Win32Cap]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 50 -or $hh -le 50) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $ok = $false
    if ($title -notlike "*Serial Output*") {
        $hdc = $g.GetHdc()
        $ok = [Win32Cap]::PrintWindow($h, $hdc, 2)
        $g.ReleaseHdc($hdc)
    }
    if (-not $ok) { $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
    $g.Dispose()
    $file = "{0}_w{1}.png" -f $Out, $i
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("saved {0} [{1}] {2}x{3}" -f $file, $title, $w, $hh)
    $i++
}
