# Bleem2 Final Execution & Deep Log Capture

$ProjectDir = "d:\DC\bleem2"
$FlycastPath = "d:\DC\flycast\flycast.exe"
$CdiPath = Join-Path $ProjectDir "build\bleem2.cdi"
$LogPath = "D:\DC\flycast3\flycast.log"

Write-Host "--- 1. Cleanup & Build ---" -ForegroundColor Cyan
Stop-Process -Name "flycast" -ErrorAction SilentlyContinue
Set-Location $ProjectDir
C:\msys64\usr\bin\bash.exe --login -c "cd /d/DC/bleem2 && make clean && make && make cdi"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build Failed!" -ForegroundColor Red
    exit 1
}

# Flycast ネイティブログの監視
$logPath = "D:\DC\flycast3\flycast.log"
if (Test-Path $logPath) { Remove-Item $logPath -Force }

Write-Host "Monitoring native flycast.log for critical exceptions (60s)..."
$proc = Start-Process $FlycastPath -ArgumentList "`"$CdiPath`"" -WorkingDirectory "D:\DC\flycast3" -PassThru

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    
    if (Test-Path $logPath) {
        $logContent = Get-Content $logPath -Raw
        
        # 1. 致命的エラーの文字列検知
        if ($logContent -match "Exception|Fatal|Address Error|Illegal Instruction|Stopped") {
            Write-Host "!!! CRITICAL SH4 EXCEPTION DETECTED !!!" -ForegroundColor Red
            Get-Content $logPath | Select-String "Exception|Fatal|Address Error" | ForEach-Object { Write-Host $_.Line -ForegroundColor Yellow }
            $proc | Stop-Process -Force
            exit 1
        }
    }

    # プロセスが予期せず終了していないか
    if ($proc.HasExited) {
        Write-Host "!!! Flycast terminated early !!!" -ForegroundColor Red
        exit 1
    }
    
    Write-Progress -Activity "Validating Bleem2 Lifecycle" -Status "Elapsed: $i/40s" -PercentComplete ($i * 100 / 40)
}

Write-Host "--- 3. Validation Successful (Safe Runtime) ---" -ForegroundColor Green
$proc | Stop-Process -Force

Write-Host "--- Cycle Complete ---" -ForegroundColor Green
