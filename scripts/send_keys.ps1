# Bring the Flycast *game* window ("Flycast - <title>") to the front and press keys
# with a hold time (the emulated Dreamcast polls the pad only a few times per second).
# Usage: powershell -File send_keys.ps1 -Keys "ENTER,X" [-HoldMs 500] [-Repeat 3]
param([string]$Keys = "ENTER", [int]$HoldMs = 500, [int]$Repeat = 1, [int]$GapMs = 400)

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Key {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    public static string Title(IntPtr h) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); return sb.ToString(); }
    public static List<IntPtr> Find(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
}
"@
$vkmap = @{ "ENTER"=0x0D; "SPACE"=0x20; "UP"=0x26; "DOWN"=0x28; "LEFT"=0x25; "RIGHT"=0x27;
            "X"=0x58; "C"=0x43; "S"=0x53; "D"=0x44; "Z"=0x5A; "A"=0x41; "Q"=0x51; "E"=0x45; "F1"=0x70 }

$p = Get-Process flycast -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "no flycast"; exit 1 }
$target = [IntPtr]::Zero
foreach ($h in [Win32Key]::Find([uint32]$p.Id)) {
    $t = [Win32Key]::Title($h)
    if ($t -like "Flycast - *") { $target = $h; break }
}
if ($target -eq [IntPtr]::Zero) { Write-Host "game window not found"; exit 1 }
[Win32Key]::ShowWindow($target, 9) | Out-Null
[Win32Key]::SetForegroundWindow($target) | Out-Null
Start-Sleep -Milliseconds 300

$list = $Keys.Split(",") | ForEach-Object { $_.Trim().ToUpper() }
for ($i = 0; $i -lt $Repeat; $i++) {
    foreach ($k in $list) {
        if (-not $vkmap.ContainsKey($k)) { Write-Host "unknown key $k"; continue }
        $vk = [byte]$vkmap[$k]
        $scan = [byte]([Win32Key]::MapVirtualKey($vk, 0))
        $ext = 0
        if ($k -eq "UP" -or $k -eq "DOWN" -or $k -eq "LEFT" -or $k -eq "RIGHT") { $ext = 1 }   # KEYEVENTF_EXTENDEDKEY
        [Win32Key]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $HoldMs
        [Win32Key]::keybd_event($vk, $scan, ($ext -bor 2), [UIntPtr]::Zero)   # KEYEVENTF_KEYUP
        Start-Sleep -Milliseconds $GapMs
    }
}
Write-Host "pressed [$Keys] x$Repeat (hold $HoldMs ms) on '$([Win32Key]::Title($target))'"
