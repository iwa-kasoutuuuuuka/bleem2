# mission_control.ps1 (ITERATION 3: FILE-HEARTBEAT)
$flycastPath = "d:\DC\flycast\flycast.exe"
$cdiFile = "d:\DC\bleem2\build\bleem2.cdi"
$reportFile = "d:\DC\bleem2\stability_report.txt"
$hbFile = "d:\DC\bleem2\heartbeat.txt"

Write-Host "`n=== BMCS: STARTING ITERATION 3 (5 RUNS x FILE-HB) ===" -ForegroundColor Cyan
"BLEEM2 ITERATION 3 REPORT - $(Get-Date)" | Out-File $reportFile

# 1. Build
& C:\msys64\usr\bin\bash.exe -c "cd /d/DC/bleem2 && source ../KallistiOS/environ.sh && make clean && make cdi"

$successCount = 0
for ($i = 1; $i -le 5; $i++) {
    Write-Host "`n--- Mission #$($i) / 5 starting ---" -ForegroundColor Yellow
    Get-Process flycast -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Test-Path $hbFile) { Remove-Item $hbFile -Force }
    Start-Sleep -Seconds 5

    $proc = Start-Process d:\DC\flycast\flycast.exe -ArgumentList "d:\DC\bleem2\build\bleem2.cdi" -PassThru -NoNewWindow
    $startTime = Get-Date
    $lastHbTime = Get-Date
    $lastCyc = -1
    $crashed = $false

    try {
        while (-not $proc.HasExited) {
            Start-Sleep -Seconds 1
            if (Test-Path $hbFile) {
                # ファイルからCYCを読み取る
                $hbText = Get-Content $hbFile -Raw -ErrorAction SilentlyContinue
                if ($hbText -match "CYC:([0-9]+)") {
                    $currentCyc = [long]$matches[1]
                    if ($currentCyc -gt $lastCyc) {
                        $lastHbTime = Get-Date
                        $lastCyc = $currentCyc
                    }
                }
            }
            
            # [STABILITY CHECK] 40秒生存で成功
            if (((Get-Date) - $startTime).TotalSeconds -gt 40) {
                Write-Host "[Mission #$($i)] 40s STABILITY CERTIFIED." -ForegroundColor Green
                $successCount++
                $crashData = "SUCCESS"
                break
            }
            
            # [HANG CHECK] 30秒更新がなければハング
            if (((Get-Date) - $lastHbTime).TotalSeconds -gt 30) {
                $crashData = "STALL_TIMEOUT"
                Write-Host "[Mission #$($i)] STALL DETECTED" -ForegroundColor Magenta
                break
            }
        }
    } finally {
        $crashData = if ($crashed) { "FAILED" } else { $crashData }
        "Run #$($i): $crashData (LastCYC: $lastCyc)" | Out-File $reportFile -Append
        if ($proc -and -not $proc.HasExited) { Stop-Process $proc -Force }
    }
}

Write-Host "`n=== ITERATION 3 COMPLETE: $successCount / 5 SUCCESSFUL ===" -ForegroundColor Cyan
Get-Content $reportFile -Raw
