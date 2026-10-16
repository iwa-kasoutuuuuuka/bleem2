# Launch Flycast with the bleem2 CDI, capture screenshots every 2s, stop, dump VMU log.
# Usage: powershell -File run_and_capture.ps1 [seconds]
param([int]$Seconds = 40, [string]$KeyTimes = "", [string]$Keys = "ENTER", [string]$KeyTimes2 = "", [string]$Keys2 = "RIGHT")

$flycast = "D:\DC\flycast\flycast.exe"
$cdi = "D:\DC\bleem2\build\bleem2.cdi"
$log = "D:\DC\bleem2\flycast.log"
$shotdir = "D:\DC\bleem2\build\shots"
if (-not (Test-Path $shotdir)) { New-Item -ItemType Directory $shotdir | Out-Null }
Remove-Item "$shotdir\*.png" -Force -ErrorAction SilentlyContinue

taskkill /F /IM flycast.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 500
if (Test-Path $log) { Remove-Item $log -Force -ErrorAction SilentlyContinue }

$p = Start-Process -FilePath $flycast -ArgumentList "`"$cdi`"" -WorkingDirectory "D:\DC\bleem2" -PassThru
$t0 = Get-Date
$crashed = $false
$n = 0
$sentAt = New-Object System.Collections.Generic.List[int]
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 2
    if ($p.HasExited) { Write-Host "flycast exited at $([int]((Get-Date) - $t0).TotalSeconds)s"; break }
    $n++
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    if ($KeyTimes -ne "") { foreach ($kt in $KeyTimes.Split(",")) { if ([int]$kt -ge $elapsed - 1 -and [int]$kt -le $elapsed + 1 -and -not $sentAt.Contains([int]$kt)) { $sentAt.Add([int]$kt) | Out-Null; & powershell -NoProfile -ExecutionPolicy Bypass -File "D:\DC\bleem2\scripts\send_keys.ps1" -Keys $Keys -HoldMs 600 -Repeat 3 } } }
    if ($KeyTimes2 -ne "") { foreach ($kt in $KeyTimes2.Split(",")) { if ([int]$kt -ge $elapsed - 1 -and [int]$kt -le $elapsed + 1 -and -not $sentAt.Contains([int]$kt)) { $sentAt.Add([int]$kt) | Out-Null; & powershell -NoProfile -ExecutionPolicy Bypass -File "D:\DC\bleem2\scripts\send_keys.ps1" -Keys $Keys2 -HoldMs 900 -Repeat 2 } } }
    $cap = Start-Process -FilePath "powershell" -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"D:\DC\bleem2\scripts\capture_flycast.ps1`" `"{0}\shot_{1:D3}`"" -f $shotdir, $n) -PassThru -WindowStyle Hidden
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
& powershell -NoProfile -ExecutionPolicy Bypass -File "D:\DC\bleem2\scripts\read_vmu_log.ps1"
if ($crashed) { exit 2 } else { exit 0 }
