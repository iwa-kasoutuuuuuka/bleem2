# Bleem2 Automated Verification Script (Fixed)
$root = "d:\DC\bleem2"
$flycast_exe = "d:\DC\flycast\flycast.exe"
$cdi = "$root\build\bleem2.cdi"
$logFile = "$root\flycast.log"
$consoleLog = "$root\console.log"
$reportFile = "$root\CRASH_REPORT.md"

function Write-Report($msg) {
    $now = Get-Date -Format "yyyy/MM/dd HH:mm:ss"
    Add-Content $reportFile "`n[$now] $msg"
}

# 1. ビルド
Write-Host "--- Starting Build ---" -ForegroundColor Cyan
& C:\msys64\usr\bin\bash.exe -c "cd /d/DC/bleem2 && source ../KallistiOS/environ.sh && make clean && make cdi"
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED!" -ForegroundColor Red
    Write-Report "Build Failed."
    exit
}

# 2. ログリセット
Write-Host "--- Resetting Logs ---" -ForegroundColor Cyan
taskkill /F /IM flycast.exe 2>$null
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }
if (Test-Path $consoleLog) { Remove-Item $consoleLog -Force -ErrorAction SilentlyContinue }

# 3. Flycast 起動
Write-Host "--- Launching Flycast ---" -ForegroundColor Cyan
if (Test-Path $consoleLog) { Remove-Item $consoleLog }
$process = Start-Process $flycast_exe -ArgumentList $cdi -PassThru -NoNewWindow -RedirectStandardOutput $consoleLog -RedirectStandardError "$root\error.log"
$startTime = Get-Date

# 4. 監視
Write-Host "--- Monitoring Watchdog ---" -ForegroundColor Yellow
$lastPC = "00000000"
$lastMS = "0"

try {
    while (-not $process.HasExited) {
        $checkFiles = @($logFile, $consoleLog)
        foreach ($f in $checkFiles) {
            if (Test-Path $f) {
                $lines = Get-Content $f -Tail 20 -ErrorAction SilentlyContinue
                foreach ($line in $lines) {
                    # [HEARTBEAT] MS:123 PC:ABCDE...
                    if ($line -match "\[HEARTBEAT\] MS:(\d+) PC:([0-9A-F]+)") {
                        $lastMS = $matches[1]
                        $lastPC = $matches[2]
                    }
                    
                    if ($line -match "Fatal:|SH4 exception|Exception:|STALL_DETECTED") {
                        Write-Host "`n[! CRASH/STALL DETECTED !]" -ForegroundColor Red
                        Write-Report "CRASH AT PC:$lastPC, TIME:$lastMS ms. LOG: $line"
                        Stop-Process $process -Force
                        exit
                    }
                }
            }
        }
        
        $elapsed = (Get-Date) - $startTime
        Write-Progress -Activity "Verifying Bleem2..." -Status "Time: $($elapsed.Seconds)s, PC: $lastPC"
        
        if ($elapsed.TotalSeconds -gt 60) {
            Write-Host "`n[+ SUCCESS: 60s PASSED +]" -ForegroundColor Green
            Write-Report "SUCCESS: Stable for 60s. Last PC: $lastPC"
            Stop-Process $process -Force
            exit
        }
        Start-Sleep -Seconds 1
    }
} catch {
    Write-Host "Monitor Error: $_"
} finally {
    if (-not $process.HasExited) { Stop-Process $process -Force }
}
