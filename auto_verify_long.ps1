# auto_verify_long.ps1 (Fixed Environment)
$root = "d:\DC\bleem2"
$flycast_exe = "d:\DC\flycast\flycast.exe"
$cdi = "$root\build\bleem2.cdi"
$logFile = "$root\flycast.log"

# 1. ビルド (KOS 環境を読み込み)
Write-Host "--- Starting Build (120s Version) ---" -ForegroundColor Cyan
& C:\msys64\usr\bin\bash.exe -c "cd /d/DC/bleem2 && source ../KallistiOS/environ.sh && make clean && make cdi"
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED!" -ForegroundColor Red
    exit 1
}

# 2. ログリセット
if (Test-Path $logFile) { Remove-Item $logFile }

# 3. Flycast 起動 (120秒監視)
Write-Host "--- Launching Flycast (120s Test) ---" -ForegroundColor Cyan
$process = Start-Process $flycast_exe -ArgumentList $cdi -PassThru -NoNewWindow
$startTime = Get-Date
$passed60 = $false

Write-Host "--- Monitoring Ultimate Stability ---" -ForegroundColor Yellow

try {
    while (-not $process.HasExited) {
        if (Test-Path $logFile) {
            $lines = Get-Content $logFile -Tail 10 -ErrorAction SilentlyContinue
            foreach ($line in $lines) {
                if ($line -match "Fatal:|SH4 exception|Exception:|EXC:") {
                    Write-Host "`n[! CRASH DETECTED !]" -ForegroundColor Red
                    Write-Host "LOG: $line"
                    Stop-Process $process -Force
                    exit 1
                }
                if ($line -match "\[60s_MILESTONE\]") {
                    if (-not $passed60) {
                        Write-Host "[OK] 60s Milestone Reached!" -ForegroundColor Green
                        $passed60 = $true
                    }
                }
            }
        }
        
        $elapsed = (Get-Date) - $startTime
        Write-Progress -Activity "120s Endurance Test..." -Status "Time: $([int]$elapsed.TotalSeconds)s"
        
        if ($elapsed.TotalSeconds -gt 120) {
            Write-Host "`n[+++ ULTIMATE SUCCESS: 120s STABILITY CONFIRMED +++]" -ForegroundColor Green
            Stop-Process $process -Force
            exit 0
        }
        Start-Sleep -Seconds 1
    }
} finally {
    if (-not $process.HasExited) { Stop-Process $process -Force }
}
