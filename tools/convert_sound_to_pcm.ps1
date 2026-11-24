param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

Add-Type -Language CSharp @"
using System;
using System.IO;
using System.Text;

public static class SonySdsEncoder {
    public const int SampleRate = 24000;
    public const int SamplesPerFrame = 31;
    public const int TotalFrames = 930; // 29,760 bytes (Dslr_Emnt_WPS_PBC.bin)

    public static short[] ReadWavResampled24kMono(byte[] raw) {
        if (raw.Length < 44 || Encoding.ASCII.GetString(raw, 0, 4) != "RIFF" || Encoding.ASCII.GetString(raw, 8, 4) != "WAVE")
            throw new InvalidDataException("Not a valid RIFF WAVE file.");

        int pos = 12;
        int fmt = 0, channels = 0, rate = 0, bits = 0;
        int dataOffset = -1, dataLen = 0;
        while (pos + 8 <= raw.Length) {
            string cid = Encoding.ASCII.GetString(raw, pos, 4);
            int csz = BitConverter.ToInt32(raw, pos + 4);
            if (cid == "fmt " && csz >= 16) {
                fmt = BitConverter.ToUInt16(raw, pos + 8);
                channels = BitConverter.ToUInt16(raw, pos + 10);
                rate = BitConverter.ToInt32(raw, pos + 12);
                bits = BitConverter.ToUInt16(raw, pos + 22);
                if (fmt == 0xFFFE && csz >= 40) {
                    fmt = BitConverter.ToUInt16(raw, pos + 32);
                }
            } else if (cid == "data") {
                dataOffset = pos + 8;
                dataLen = Math.Min(csz, raw.Length - dataOffset);
                break;
            }
            pos += 8 + csz + (csz & 1);
        }
        if (dataOffset < 0 || (fmt != 1 && fmt != 3) || channels < 1 || rate < 1000)
            throw new InvalidDataException("Unsupported WAV format (need 16-bit PCM or 32-bit float WAV, or install ffmpeg).");

        int bytesPerSample = bits / 8;
        if ((fmt == 1 && bits != 16 && bits != 24 && bits != 32) || (fmt == 3 && bits != 32))
            throw new InvalidDataException("Unsupported WAV bit depth.");

        int frameBytes = channels * bytesPerSample;
        int srcFrames = dataLen / frameBytes;
        if (srcFrames <= 0) return new short[0];

        double[] monoSrc = new double[srcFrames];
        for (int i = 0; i < srcFrames; i++) {
            double sum = 0;
            for (int c = 0; c < channels; c++) {
                int off = dataOffset + i * frameBytes + c * bytesPerSample;
                double v = 0;
                if (fmt == 3) {
                    v = BitConverter.ToSingle(raw, off) * 32767.0;
                } else if (bits == 16) {
                    v = BitConverter.ToInt16(raw, off);
                } else if (bits == 24) {
                    int s24 = (raw[off] | (raw[off + 1] << 8) | ((sbyte)raw[off + 2] << 16));
                    v = s24 / 256.0;
                } else if (bits == 32) {
                    v = BitConverter.ToInt32(raw, off) / 65536.0;
                }
                sum += v;
            }
            monoSrc[i] = sum / channels;
        }

        if (rate == SampleRate) {
            short[] direct = new short[srcFrames];
            for (int i = 0; i < srcFrames; i++)
                direct[i] = (short)Math.Max(-32768, Math.Min(32767, (int)Math.Round(monoSrc[i])));
            return direct;
        }

        int dstFrames = Math.Max(1, (int)Math.Round((double)srcFrames * SampleRate / rate));
        short[] dst = new short[dstFrames];
        for (int i = 0; i < dstFrames; i++) {
            double srcPos = (double)i * rate / SampleRate;
            int i0 = Math.Min((int)Math.Floor(srcPos), srcFrames - 1);
            int i1 = Math.Min(i0 + 1, srcFrames - 1);
            double frac = srcPos - i0;
            double interp = monoSrc[i0] * (1.0 - frac) + monoSrc[i1] * frac;
            dst[i] = (short)Math.Max(-32768, Math.Min(32767, (int)Math.Round(interp)));
        }
        return dst;
    }

    public static byte[] EncodeSonySds(short[] samples, out double activeSec) {
        int totalSamples = TotalFrames * SamplesPerFrame;
        int start = 0;
        for (int i = 0; i < samples.Length; i++) {
            if (Math.Abs((int)samples[i]) > 150) {
                start = Math.Max(0, i - (int)(0.005 * SampleRate));
                break;
            }
        }
        int workLen = samples.Length - start;
        int[] work = new int[Math.Max(0, workLen)];
        int peak = 0;
        for (int i = 0; i < work.Length; i++) {
            int s = samples[start + i];
            work[i] = s;
            int a = Math.Abs(s);
            if (a > peak) peak = a;
        }
        double gain = peak > 0 ? (29000.0 / peak) : 1.0;
        for (int i = 0; i < work.Length; i++) {
            work[i] = Math.Max(-32767, Math.Min(32767, (int)Math.Round(work[i] * gain)));
        }

        int endIdx = work.Length;
        for (int i = work.Length - 1; i >= 0; i--) {
            if (Math.Abs(work[i]) > 80) {
                endIdx = Math.Min(work.Length, i + (int)(0.025 * SampleRate));
                break;
            }
        }
        int activeLen = Math.Min(endIdx, totalSamples);
        activeSec = (double)activeLen / SampleRate;

        int fadeLen = (int)(0.010 * SampleRate);
        for (int i = 0; i < fadeLen; i++) {
            int idx = activeLen - fadeLen + i;
            if (idx >= 0 && idx < activeLen) {
                work[idx] = (int)Math.Round(work[idx] * (1.0 - (double)(i + 1) / fadeLen));
            }
        }

        int[] padded = new int[totalSamples];
        Array.Copy(work, 0, padded, 0, activeLen);

        byte[] encoded = new byte[TotalFrames * 32];
        int prev = 0;
        byte[] bestBytes = new byte[32];
        byte[] candBytes = new byte[32];

        for (int f = 0; f < TotalFrames; f++) {
            int baseIdx = f * SamplesPerFrame;
            bool allZero = (prev == 0);
            if (allZero) {
                for (int j = 0; j < SamplesPerFrame; j++) {
                    if (padded[baseIdx + j] != 0) { allZero = false; break; }
                }
            }
            if (allZero) continue; // already 0x00 * 32

            long bestErr = long.MaxValue;
            int bestPrev = prev;

            for (int mode = 0; mode <= 1; mode++) {
                for (int sh = 0; sh <= 8; sh++) {
                    int p = prev;
                    long err = 0;
                    candBytes[0] = (byte)((mode << 4) | sh);
                    int step = 1 << sh;
                    for (int j = 0; j < SamplesPerFrame; j++) {
                        int target = padded[baseIdx + j];
                        int pred = (mode == 0) ? 0 : ((p * 15) / 16);
                        int diff = target - pred;
                        int q = (int)Math.Round((double)diff / step, MidpointRounding.AwayFromZero);
                        if (q < -128) q = -128;
                        if (q > 127) q = 127;
                        int rec = pred + (q << sh);
                        if (rec < -32768) rec = -32768;
                        if (rec > 32767) rec = 32767;
                        long d = target - rec;
                        err += d * d;
                        candBytes[1 + j] = (byte)(q & 0xFF);
                        p = rec;
                    }
                    if (err < bestErr) {
                        bestErr = err;
                        bestPrev = p;
                        Buffer.BlockCopy(candBytes, 0, bestBytes, 0, 32);
                    }
                }
            }
            Buffer.BlockCopy(bestBytes, 0, encoded, f * 32, 32);
            prev = bestPrev;
        }
        return encoded;
    }
}
"@

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
    $base = [System.IO.Path]::GetFileNameWithoutExtension($item.Name) -replace '[\\/:*?"<>|\s]+', '_'
    if ([string]::IsNullOrWhiteSpace($base)) { $base = "custom_shutter" }
    $outPath = Join-Path $item.DirectoryName "$base.pcm"

    $tmpWav = $null
    try {
        if ($ffmpeg) {
            $tmpWav = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "a6400_sds_" + [Guid]::NewGuid().ToString("N") + ".wav")
            & $ffmpeg.Source -y -i $item.FullName -ac 1 -ar 24000 -sample_fmt s16 $tmpWav 2>$null
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path $tmpWav)) {
                throw "ffmpeg 轉碼失敗：$($item.Name)"
            }
            $wavBytes = [System.IO.File]::ReadAllBytes($tmpWav)
        } elseif ($item.Extension -ieq ".wav") {
            $wavBytes = [System.IO.File]::ReadAllBytes($item.FullName)
        } else {
            Write-Host "[!] 找不到 ffmpeg.exe，無法直接轉換 $($item.Extension) 檔：$($item.Name)" -ForegroundColor Yellow
            Write-Host "    請執行：winget install Gyan.FFmpeg  或先將音檔轉為 .wav 再拖進來。" -ForegroundColor Yellow
            continue
        }

        $samples = [SonySdsEncoder]::ReadWavResampled24kMono($wavBytes)
        $activeSec = 0.0
        $sdsBytes = [SonySdsEncoder]::EncodeSonySds($samples, [ref]$activeSec)
        [System.IO.File]::WriteAllBytes($outPath, $sdsBytes)
        $secStr = $activeSec.ToString("0.00")
        Write-Host "[+] 完成 (Sony 24kHz SDS ADPCM)：$outPath ($($sdsBytes.Length) bytes, 有效長度 ${secStr}s)" -ForegroundColor Green
    } catch {
        Write-Host "[X] 轉換失敗 ($($item.Name))：$($_.Exception.Message)" -ForegroundColor Red
    } finally {
        if ($tmpWav -and (Test-Path $tmpWav)) {
            Remove-Item -Force $tmpWav -ErrorAction SilentlyContinue
        }
    }
}
