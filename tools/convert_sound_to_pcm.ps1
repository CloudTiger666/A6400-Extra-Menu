param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$MaxBytes = 29760 # 16000 Hz * 2 bytes * 0.93s
$ffmpeg = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
if (-not $ffmpeg) {
    $localFfmpeg = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "ffmpeg.exe"
    if (Test-Path $localFfmpeg) { $ffmpeg = Get-Command $localFfmpeg }
}

foreach ($inPath in $Files) {
    if (-not (Test-Path $inPath)) {
        Write-Host "[!] 找不到檔案：$inPath" -ForegroundColor Yellow
        continue
    }
    $item = Get-Item $inPath
    $base = [System.IO.Path]::GetFileNameWithoutExtension($item.Name) -replace '[^A-Za-z0-9_\-]', '_'
    if ([string]::IsNullOrWhiteSpace($base)) { $base = "custom_shutter" }
    $outPath = Join-Path $item.DirectoryName "$base.pcm"

    if ($ffmpeg) {
        & $ffmpeg.Source -y -i $item.FullName -ar 16000 -ac 1 -f s16le -t 0.93 $outPath 2>$null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $outPath)) {
            $bytes = [System.IO.File]::ReadAllBytes($outPath)
            if ($bytes.Length -gt $MaxBytes) {
                $trimmed = New-Object byte[] $MaxBytes
                [Array]::Copy($bytes, 0, $trimmed, 0, $MaxBytes)
                [System.IO.File]::WriteAllBytes($outPath, $trimmed)
            }
            $finalLen = (Get-Item $outPath).Length
            Write-Host "[+] 完成：$outPath ($finalLen bytes)" -ForegroundColor Green
            continue
        }
    }

    if ($item.Extension -ieq ".wav") {
        $raw = [System.IO.File]::ReadAllBytes($item.FullName)
        if ($raw.Length -lt 44 -or [System.Text.Encoding]::ASCII.GetString($raw, 0, 4) -ne "RIFF") {
            Write-Host "[X] 不支援的 WAV 格式：$($item.Name)" -ForegroundColor Red
            continue
        }
        $pos = 12
        $channels = 1; $rate = 16000; $bits = 16; $fmt = 1
        $dataOffset = -1; $dataLen = 0
        while ($pos + 8 -le $raw.Length) {
            $cid = [System.Text.Encoding]::ASCII.GetString($raw, $pos, 4)
            $csz = [BitConverter]::ToInt32($raw, $pos + 4)
            if ($cid -eq "fmt ") {
                $fmt = [BitConverter]::ToUInt16($raw, $pos + 8)
                $channels = [BitConverter]::ToUInt16($raw, $pos + 10)
                $rate = [BitConverter]::ToInt32($raw, $pos + 12)
                $bits = [BitConverter]::ToUInt16($raw, $pos + 22)
            } elseif ($cid -eq "data") {
                $dataOffset = $pos + 8
                $dataLen = [Math]::Min($csz, $raw.Length - $dataOffset)
                break
            }
            $pos += 8 + $csz + ($csz % 2)
        }
        if ($dataOffset -lt 0 -or $fmt -ne 1 -or $bits -ne 16 -or $channels -lt 1 -or $rate -lt 1000) {
            Write-Host "[X] 僅支援 16-bit PCM WAV，或請先安裝 ffmpeg (winget install Gyan.FFmpeg)：$($item.Name)" -ForegroundColor Red
            continue
        }
        $frameBytes = $channels * 2
        $srcFrames = [int]($dataLen / $frameBytes)
        $dstFrames = [int][Math]::Min([Math]::Floor($srcFrames * 16000.0 / $rate), $MaxBytes / 2)
        $outBuf = New-Object byte[] ($dstFrames * 2)
        for ($i = 0; $i -lt $dstFrames; $i++) {
            $srcIdx = [int][Math]::Min([Math]::Floor($i * $rate / 16000.0), $srcFrames - 1)
            $sum = 0
            for ($c = 0; $c -lt $channels; $c++) {
                $sum += [BitConverter]::ToInt16($raw, $dataOffset + $srcIdx * $frameBytes + $c * 2)
            }
            $mono = [int16][Math]::Max(-32768, [Math]::Min(32767, [int]($sum / $channels)))
            $b = [BitConverter]::GetBytes($mono)
            $outBuf[$i * 2] = $b[0]
            $outBuf[$i * 2 + 1] = $b[1]
        }
        [System.IO.File]::WriteAllBytes($outPath, $outBuf)
        Write-Host "[+] 完成 (內建 WAV 轉換)：$outPath ($($outBuf.Length) bytes)" -ForegroundColor Green
    } else {
        Write-Host "[!] 找不到 ffmpeg.exe，無法直接轉 $($item.Extension) 檔。" -ForegroundColor Yellow
        Write-Host "    請執行：winget install Gyan.FFmpeg  或先轉成 .wav 後再拖進來。" -ForegroundColor Yellow
    }
}
