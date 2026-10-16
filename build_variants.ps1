$root = "d:\DC\bleem2"
$mips_h = "$root\src\mips.h"

function Set-Variant($hud, $autoboot, $io) {
    $content = Get-Content $mips_h -Raw
    $content = $content -replace '#define MASTER_HUD\s+\d', "#define MASTER_HUD      $hud"
    $content = $content -replace '#define MASTER_AUTOBOOT\s+\d', "#define MASTER_AUTOBOOT $autoboot"
    $content = $content -replace '#define MASTER_IO_STABLE\s+\d', "#define MASTER_IO_STABLE $io"
    [System.IO.File]::WriteAllText($mips_h, $content)
    Start-Sleep -Milliseconds 500
}

function Build-CDI($name) {
    Write-Host "=== Building: $name ===" -ForegroundColor Yellow
    & C:\msys64\usr\bin\bash.exe -c "cd /d/DC/bleem2 && source ../KallistiOS/environ.sh && make clean && make cdi" 2>&1 | Out-Null
    $src = "$root\build\bleem2.cdi"
    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Seconds 2
        if (Test-Path $src) {
            try { [System.IO.File]::OpenRead($src).Close(); break } catch {}
        }
    }
    $dst = "$root\build\$name"
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        if (Test-Path $dst) { Write-Host "OK: $name" -ForegroundColor Green; return }
    }
    Write-Host "FAILED: $name" -ForegroundColor Red
}

# CDI 1: 直接FB + BIOSパス
Set-Variant 0 0 0
Build-CDI "bleem2_v7_direct.cdi"

# CDI 2: 直接FB + Autoboot
Set-Variant 0 1 0
Build-CDI "bleem2_v7_auto.cdi"

# CDI 3: 直接FB + 毎フレームログ
Set-Variant 0 0 1
Build-CDI "bleem2_v7_trace.cdi"

Write-Host "`n=== Done ===" -ForegroundColor Cyan
Get-ChildItem "$root\build\bleem2_v7_*.cdi" | Format-Table Name, Length
