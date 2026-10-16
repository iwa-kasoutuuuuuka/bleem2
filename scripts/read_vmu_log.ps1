# Extract the BLEEMLOG file written by bleem2 from Flycast's VMU image.
# Usage: powershell -File read_vmu_log.ps1 [path-to-vmu_save_A1.bin]
param([string]$Vmu = "D:\DC\flycast\data\vmu_save_A1.bin", [string]$Name = "BLEEMLOG", [string]$OutFile = "")

if (-not (Test-Path $Vmu)) { Write-Host "no vmu image: $Vmu"; exit 1 }
$img = [System.IO.File]::ReadAllBytes($Vmu)
if ($img.Length -lt 256 * 512) { Write-Host "image too small"; exit 1 }

function Block($n) { return $n * 512 }

# root block 255: FAT location at +0x46 (u16), dir location at +0x4A (u16), dir size at +0x4C
$root = Block(255)
$fatBlk = [BitConverter]::ToUInt16($img, $root + 0x46)
$dirBlk = [BitConverter]::ToUInt16($img, $root + 0x4A)
$dirSize = [BitConverter]::ToUInt16($img, $root + 0x4C)
if ($fatBlk -eq 0 -and $dirBlk -eq 0) { $fatBlk = 254; $dirBlk = 253; $dirSize = 13 }

$found = $null
for ($b = 0; $b -lt $dirSize; $b++) {
    $base = Block($dirBlk - $b)
    for ($e = 0; $e -lt 16; $e++) {
        $o = $base + $e * 32
        $type = $img[$o]
        if ($type -eq 0) { continue }
        $first = [BitConverter]::ToUInt16($img, $o + 2)
        $nm = [System.Text.Encoding]::ASCII.GetString($img, $o + 4, 12).TrimEnd([char]0, ' ')
        $size = [BitConverter]::ToUInt16($img, $o + 0x18)
        if ($nm -eq $Name) { $found = @{ first = $first; size = $size }; break }
    }
    if ($found) { break }
}
if (-not $found) { Write-Host "file $Name not found in VMU"; exit 2 }

$fat = Block($fatBlk)
$blk = $found.first
$bytes = New-Object System.Collections.Generic.List[byte]
$guard = 0
while ($blk -lt 0xFFF0 -and $guard -lt 300) {
    $start = Block($blk)
    $bytes.AddRange([byte[]]$img[$start..($start + 511)])
    $blk = [BitConverter]::ToUInt16($img, $fat + $blk * 2)
    $guard++
}
if ($OutFile -ne "") { [System.IO.File]::WriteAllBytes($OutFile, $bytes.ToArray()); Write-Host "wrote $($bytes.Count) bytes to $OutFile"; exit 0 }
$text = [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
$nul = $text.IndexOf([char]0)
if ($nul -ge 0) { $text = $text.Substring(0, $nul) }
Write-Host "--- $Name ($($found.size) blocks, $($text.Length) chars) ---"
Write-Host $text
