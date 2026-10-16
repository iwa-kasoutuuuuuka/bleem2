# Launch Flycast with the bleem2 CDI, capture screenshots every 2s, stop, dump VMU log.
# Usage: powershell -File run_and_capture.ps1 [seconds] [-Cdi path] [-FlycastDir dir]
# Paths default to this checkout (the folder above scripts/), so it works from any git worktree.
# Flycast dir: -FlycastDir, else $env:BLEEM2_FLYCAST, else the path in build\flycast_dir.txt, else D:\DC\flycast.
# Only Flycast processes started from that dir are killed, so parallel sessions with their own
# Flycast copy (own VMU file) do not disturb each other.
param([int]$Seconds = 40, [string]$KeyTimes = "", [string]$Keys = "ENTER", [string]$KeyTimes2 = "", [string]$Keys2 = "RIGHT",
      [string]$Cdi = "", [string]$FlycastDir = "")

$root = Split-Path $PSScriptRoot -Parent
if ($FlycastDir -eq "") {
    if ($env:BLEEM2_FLYCAST) { $FlycastDir = $env:BLEEM2_FLYCAST }
    elseif (Test-Path "$root\build\flycast_dir.txt") { $FlycastDir = (Get-Content "$root\build\flycast_dir.txt" -TotalCount 1).Trim() }
    else { $FlycastDir = "D:\DC\flycast" }
}
$flycast = Join-Path $FlycastDir "flycast.exe"
$cdi = if ($Cdi -ne "") { $Cdi } else { "$root\build\bleem2.cdi" }
$log = "$root\flycast.log"
$shotdir = "$root\build\shots"
$scripts = $PSScriptRoot
if (-not (Test-Path $shotdir)) { New-Item -ItemType Directory $shotdir | Out-Null }
Remove-Item "$shotdir\*.png" -Force -ErrorAction SilentlyContinue

Get-Process flycast -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $flycast } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
if (Test-Path $log) { Remove-Item $log -Force -ErrorAction SilentlyContinue }

Write-Host "flycast=$flycast cdi=$cdi"
$p = Start-Process -FilePath $flycast -ArgumentList "`"$cdi`"" -WorkingDirectory $root -PassThru
$t0 = Get-Date
$crashed = $false
$n = 0
$sentAt = New-Object System.Collections.Generic.List[int]
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 2
    if ($p.HasExited) { Write-Host "flycast exited at $([int]((Get-Date) - $t0).TotalSeconds)s"; break }
    $n++
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    if ($KeyTimes -ne "") { foreach ($kt in $KeyTimes.Split(",")) { if ([int]$kt -ge $elapsed - 1 -and [int]$kt -le $elapsed + 1 -and -not $sentAt.Contains([int]$kt)) { $sentAt.Add([int]$kt) | Out-Null; & powershell -NoProfile -ExecutionPolicy Bypass -File "$scripts\send_keys.ps1" -Keys $Keys -HoldMs 600 -Repeat 3 -FlycastPid $p.Id } } }
    if ($KeyTimes2 -ne "") { foreach ($kt in $KeyTimes2.Split(",")) { if ([int]$kt -ge $elapsed - 1 -and [int]$kt -le $elapsed + 1 -and -not $sentAt.Contains([int]$kt)) { $sentAt.Add([int]$kt) | Out-Null; & powershell -NoProfile -ExecutionPolicy Bypass -File "$scripts\send_keys.ps1" -Keys $Keys2 -HoldMs 900 -Repeat 2 -FlycastPid $p.Id } } }
    $cap = Start-Process -FilePath "powershell" -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"{2}\capture_flycast.ps1`" `"{0}\shot_{1:D3}`" -FlycastPid {3}" -f $shotdir, $n, $scripts, $p.Id) -PassThru -WindowStyle Hidden
    if (-not $cap.WaitForExit(20000)) { $cap.Kill(); Write-Host "capture timed out at $elapsed s" }
    if (Test-Path $log) {
        $tail = Get-Content $log -Tail 5 -ErrorAction SilentlyContinue
        if ($tail -match "Fatal|Exception:|has stopped") { $crashed = $true; Write-Host "CRASH detected at $([int]((Get-Date) - $t0).TotalSeconds)s"; break }
    }
}
Start-Sleep -Milliseconds 300
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 1
Write-Host "--- flycast.log (filtered) ---"
if (Test-Path $log) { Get-Content $log | Select-String "BOOT|Fatal|Exception|stopped|Error|terminating" | ForEach-Object { $_.Line } }
Write-Host "--- shots ---"
Get-ChildItem $shotdir | ForEach-Object { $_.Name }
Write-Host "--- VMU log ---"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$scripts\read_vmu_log.ps1" -Vmu (Join-Path $FlycastDir "data\vmu_save_A1.bin")
if ($crashed) { exit 2 } else { exit 0 }
