param(
    [ValidateSet("install", "rollback", "diag", "cleanup")]
    [string]$Action = "install",
    [ValidateSet("", "tc", "sc")]
    [string]$Lang = ""
)

$ErrorActionPreference = "Stop"
$env:WSL_UTF8 = "1"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$DistroName = "A6400Deployer"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$OfflineDir = Join-Path $RootDir "offline"
$RootfsTar = Join-Path $OfflineDir "a6400-alpine-wsl.tar.gz"
$WslMsi = Join-Path $OfflineDir "wsl_update_x64.msi"
$UsbipdMsi = Join-Path $OfflineDir "usbipd-win.msi"
$InstallBase = Join-Path $env:LOCALAPPDATA "A6400WSL"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [int]$TimeoutSec = 15
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $escaped = foreach ($a in $Arguments) {
        if ($null -eq $a -or $a -eq "") {
            '""'
        } elseif ($a -match '[\s"]') {
            '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
        } else {
            $a
        }
    }
    $psi.Arguments = ($escaped -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    try {
        [void]$proc.Start()
        $proc.StandardInput.Close()
        $outTask = $proc.StandardOutput.ReadToEndAsync()
        $errTask = $proc.StandardError.ReadToEndAsync()

        if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
            try { $proc.Kill() } catch {}
            return [PSCustomObject]@{
                ExitCode = -1
                TimedOut = $true
                Lines    = @()
                Text     = "TIMEOUT after ${TimeoutSec}s"
            }
        }
        $proc.WaitForExit()
        $outStr = $outTask.GetAwaiter().GetResult()
        $errStr = $errTask.GetAwaiter().GetResult()
        $combined = (($outStr + "`n" + $errStr) -replace "`0", "")
        $lines = @($combined -split "`r?`n")
        return [PSCustomObject]@{
            ExitCode = $proc.ExitCode
            TimedOut = $false
            Lines    = $lines
            Text     = $combined.Trim()
        }
    } finally {
        $proc.Dispose()
    }
}

function Find-WslExe {
    $candidates = @(
        "C:\Program Files\WSL\wsl.exe",
        (Join-Path $env:ProgramFiles "WSL\wsl.exe")
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    $cmd = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return "C:\Windows\System32\wsl.exe"
}

function Find-UsbipdExe {
    $cmd = Get-Command usbipd.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "C:\Program Files\usbipd-win\usbipd.exe",
        (Join-Path $env:ProgramFiles "usbipd-win\usbipd.exe")
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Convert-ToWslPath([string]$WinPath) {
    $full = (Resolve-Path $WinPath).Path
    if ($full -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1].ToLower()
        $rest = $Matches[2] -replace '\\', '/'
        return "/mnt/$drive/$rest"
    }
    throw "無法轉換 Windows 路徑至 WSL：$full"
}

function Test-WslReady {
    $build = [Environment]::OSVersion.Version.Build
    if ($build -ge 22000) {
        # On Windows 11, check on-disk MSI or Store MSIX first so we NEVER invoke the blocking inbox stub ("Press any key to install")
        $hasMsi = (Test-Path "C:\Program Files\WSL\wsl.exe") -or (Test-Path (Join-Path $env:ProgramFiles "WSL\wsl.exe"))
        $hasAppx = $false
        if (-not $hasMsi) {
            try {
                $pkg = Get-AppxPackage -Name "MicrosoftCorporationII.WindowsSubsystemForLinux" -ErrorAction SilentlyContinue
                if ($pkg) { $hasAppx = $true }
            } catch {}
        }
        if (-not $hasMsi -and -not $hasAppx) {
            return $false
        }
    }

    $wsl = Find-WslExe
    if (-not (Test-Path $wsl)) { return $false }

    # HCS / VirtualMachinePlatform (`vmcompute`) must be installed for WSL2 VM creation (`wsl --import ... --version 2`)
    if (-not (Get-Service -Name "vmcompute" -ErrorAction SilentlyContinue)) {
        return $false
    }

    $ver = Invoke-Native -FilePath $wsl -Arguments @("--version") -TimeoutSec 8
    if (-not $ver.TimedOut -and $ver.ExitCode -eq 0 -and $ver.Text -and ($ver.Text -notmatch "--update")) {
        return $true
    }

    if ($build -lt 22000) {
        $st = Invoke-Native -FilePath $wsl -Arguments @("--status") -TimeoutSec 8
        if (-not $st.TimedOut -and $st.ExitCode -eq 0 -and ($st.Text -notmatch "--update")) {
            return $true
        }
    }
    return $false
}

function Get-WslDistros {
    $wsl = Find-WslExe
    $res = Invoke-Native -FilePath $wsl -Arguments @("-l", "-q") -TimeoutSec 10
    if ($res.TimedOut -or $res.ExitCode -ne 0) { return @() }
    $list = @()
    foreach ($line in $res.Lines) {
        $clean = $line.Trim()
        if ($clean -and ($clean -notmatch "--update")) {
            $list += $clean
        }
    }
    return $list
}

function Install-FullWslMsiFromMirror {
    $cachedMsi = Join-Path $OfflineDir "wsl.2.4.13.0.x64.msi"
    $urls = @(
        "https://gh-proxy.com/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi",
        "https://ghfast.top/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi",
        "https://ghproxy.net/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi",
        "https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi"
    )
    if (-not (Test-Path $cachedMsi) -or ((Get-Item $cachedMsi).Length -lt 100MB)) {
        New-Item -ItemType Directory -Force -Path $OfflineDir | Out-Null
        $downloaded = $false
        foreach ($u in $urls) {
            Write-Host "[*] 正在從國內高速鏡像下載微軟官方簽章 WSL2 引擎 (約 196 MB，下載後會自動快取)："
            Write-Host "    $u"
            $tmp = "$cachedMsi.part"
            Remove-Item -Force $tmp -ErrorAction SilentlyContinue
            $curlExe = Get-Command curl.exe -ErrorAction SilentlyContinue
            if ($curlExe) {
                $prev = $ErrorActionPreference
                $ErrorActionPreference = "Continue"
                try {
                    & curl.exe -fL --connect-timeout 15 --max-time 600 -# -o $tmp $u
                    $rc = $LASTEXITCODE
                } finally {
                    $ErrorActionPreference = $prev
                }
                if ($rc -eq 0 -and (Test-Path $tmp) -and ((Get-Item $tmp).Length -gt 100MB)) {
                    Move-Item -Force $tmp $cachedMsi
                    $downloaded = $true
                    break
                }
            } else {
                try {
                    Invoke-WebRequest -Uri $u -OutFile $tmp -UseBasicParsing -TimeoutSec 600
                    if ((Test-Path $tmp) -and ((Get-Item $tmp).Length -gt 100MB)) {
                        Move-Item -Force $tmp $cachedMsi
                        $downloaded = $true
                        break
                    }
                } catch {}
            }
            Write-Host "[!] 此鏡像下載失敗，自動切換下一個鏡像..." -ForegroundColor Yellow
            Remove-Item -Force $tmp -ErrorAction SilentlyContinue
        }
        if (-not $downloaded) {
            return $false
        }
    }
    Write-Host "[*] 正在靜默安裝微軟官方簽章 WSL2 MSI ($cachedMsi)，請稍候約 15~30 秒..."
    $p = Start-Process -FilePath "msiexec.exe" -ArgumentList "/i `"$cachedMsi`" /qn /norestart" -Wait -PassThru
    if ($p.ExitCode -eq 0 -or $p.ExitCode -eq 3010) {
        Write-Host "[+] 微軟官方 WSL2 MSI 安裝完成 (rc=$($p.ExitCode))"
        return $true
    }
    Write-Host "[!] WSL2 MSI 安裝返回代碼 $($p.ExitCode)" -ForegroundColor Yellow
    return $false
}

function Ensure-Wsl2Offline {
    Write-Host "[*] 檢查 Windows WSL2 環境..."
    if (Test-WslReady) {
        Write-Host "[+] WSL2 已就緒 ($(Find-WslExe))"
        return
    }

    Write-Host "[*] 尚未就緒，正在確認 Windows 虛擬機器平台與 WSL 功能..."
    $needReboot = $false
    foreach ($feat in @("VirtualMachinePlatform", "Microsoft-Windows-Subsystem-Linux")) {
        $proc = Start-Process -FilePath "dism.exe" -ArgumentList "/online /enable-feature /featurename:$feat /all /norestart" -Wait -PassThru -NoNewWindow
        if ($proc.ExitCode -eq 3010) {
            $needReboot = $true
        } elseif ($proc.ExitCode -ne 0) {
            Write-Host "[!] 啟用 Windows 功能 $feat 返回代碼 $($proc.ExitCode)，請確認 BIOS 已開啟虛擬化（VT-x / SVM）。" -ForegroundColor Yellow
        }
    }

    # 1. If a full modern offline WSL MSI (e.g. wsl.2.x.x.x.x64.msi) exists in offline\, install it directly
    $fullMsi = Get-ChildItem -Path $OfflineDir -Filter "wsl*.x64.msi" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne "wsl_update_x64.msi" -and $_.Length -gt 80MB } |
        Select-Object -First 1
    if ($fullMsi) {
        Write-Host "[*] 偵測到本機完整離線 WSL2 安裝檔 ($($fullMsi.Name))，正在離線安裝..."
        $pFull = Start-Process -FilePath "msiexec.exe" -ArgumentList "/i `"$($fullMsi.FullName)`" /qn /norestart" -Wait -PassThru
        if ($pFull.ExitCode -eq 3010) { $needReboot = $true }
    }

    # 2. On Windows 11 (Build >= 22000), install official signed WSL 2.4.13 MSI via fast China mirrors (cached in offline\, works even without MS Store / LTSC!)
    if (-not (Test-WslReady)) {
        $build = [Environment]::OSVersion.Version.Build
        if ($build -ge 22000) {
            Write-Host "[*] Windows 11 尚未安裝新版 WSL2 引擎，正在透過國內加速鏡像自動補裝微軟官方 WSL2 MSI（免微軟商店、免翻牆）..."
            if (Install-FullWslMsiFromMirror) {
                Start-Sleep -Seconds 2
            }
            if (-not (Test-WslReady)) {
                Write-Host "[*] 嘗試改用微軟內建 wsl.exe --update..."
                $upd = Invoke-Native -FilePath (Find-WslExe) -Arguments @("--update") -TimeoutSec 90
                if ($upd.Text) { Write-Host $upd.Text }
                Start-Sleep -Seconds 2
            }
        } elseif (Test-Path $WslMsi) {
            Write-Host "[*] 正在離線安裝微軟官方 Win10 WSL2 核心套件 (wsl_update_x64.msi)..."
            $p2 = Start-Process -FilePath "msiexec.exe" -ArgumentList "/i `"$WslMsi`" /qn /norestart" -Wait -PassThru
            if ($p2.ExitCode -eq 3010) { $needReboot = $true }
        }
    }

    if ($needReboot) {
        Write-Host ""
        Write-Host "========================================================" -ForegroundColor Green
        Write-Host "  [+] 已完成啟用 WSL2 與安裝微軟 WSL2 引擎！" -ForegroundColor Green
        Write-Host "  [!] Windows 需要「重新開機一次」讓虛擬機器平台生效。" -ForegroundColor Yellow
        Write-Host "  請重新開機後，再次雙擊「一鍵開始安裝.bat」即可直接刷機！" -ForegroundColor Yellow
        Write-Host "========================================================" -ForegroundColor Green
        exit 0
    }

    if (-not (Test-WslReady)) {
        Write-Host ""
        Write-Host "[!] WSL2 尚未完成初始化。" -ForegroundColor Yellow
        Write-Host "    請嘗試：" -ForegroundColor Yellow
        Write-Host "    1. 重新開機一次後再雙擊「一鍵開始安裝.bat」。" -ForegroundColor Yellow
        Write-Host "    2. 或手動將 wsl.2.4.13.0.x64.msi 放進本資料夾的 offline\ 內再重試。" -ForegroundColor Yellow
        exit 1
    }

    Invoke-Native -FilePath (Find-WslExe) -Arguments @("--set-default-version", "2") -TimeoutSec 10 | Out-Null
    Write-Host "[+] WSL2 已就緒 ($(Find-WslExe))"
}

function Ensure-UsbipdOffline {
    $exe = Find-UsbipdExe
    if ($exe) {
        Write-Host "[+] usbipd-win 已安裝 ($exe)"
    } else {
        if (-not (Test-Path $UsbipdMsi)) {
            throw "找不到離線安裝檔：$UsbipdMsi"
        }
        Write-Host "[*] 正在離線安裝微軟簽章 usbipd-win (免聯網、免 Zadig)..."
        $p = Start-Process -FilePath "msiexec.exe" -ArgumentList "/i `"$UsbipdMsi`" /qn /norestart" -Wait -PassThru
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
            throw "usbipd-win 安裝失敗 (rc=$($p.ExitCode))"
        }
        Start-Sleep -Seconds 2
        $exe = Find-UsbipdExe
        if (-not $exe) {
            throw "usbipd-win 安裝後仍找不到 usbipd.exe，請重新開機後再試。"
        }
        Write-Host "[+] usbipd-win 離線安裝完成"
    }
    try {
        $svc = Get-Service -Name "usbipd" -ErrorAction SilentlyContinue
        if ($svc -and $svc.Status -ne "Running") {
            Start-Service -Name "usbipd" -ErrorAction SilentlyContinue
        }
    } catch {}
    return $exe
}

function Ensure-AlpineDistro {
    $wsl = Find-WslExe
    $distros = Get-WslDistros
    if ($distros -notcontains $DistroName) {
        if (-not (Test-Path $RootfsTar)) {
            throw "找不到極輕量 Alpine 系統映像：$RootfsTar"
        }
        Write-Host "[*] 正在匯入極輕量 Alpine Linux 部署環境 ($DistroName)，約需 3~5 秒..."
        New-Item -ItemType Directory -Force -Path $InstallBase | Out-Null
        $imp = Invoke-Native -FilePath $wsl -Arguments @("--import", $DistroName, $InstallBase, $RootfsTar, "--version", "2") -TimeoutSec 120
        if ($imp.ExitCode -ne 0) {
            if ($imp.Text) { Write-Host $imp.Text -ForegroundColor Yellow }
            if ($imp.Text -match "HCS_E_SERVICE_NOT_AVAILABLE|VirtualMachinePlatform|0x80370102|0x800701bc") {
                Write-Host "[*] 偵測到 Windows 虛擬機器平台尚未生效，正在自動補開功能..." -ForegroundColor Yellow
                foreach ($feat in @("VirtualMachinePlatform", "Microsoft-Windows-Subsystem-Linux")) {
                    Start-Process -FilePath "dism.exe" -ArgumentList "/online /enable-feature /featurename:$feat /all /norestart" -Wait -PassThru -NoNewWindow | Out-Null
                }
                Start-Process -FilePath "bcdedit.exe" -ArgumentList "/set hypervisorlaunchtype auto" -Wait -PassThru -NoNewWindow | Out-Null
                Write-Host ""
                Write-Host "========================================================" -ForegroundColor Yellow
                Write-Host "  [!] 已自動開啟 Windows 虛擬機器平台！" -ForegroundColor Yellow
                Write-Host "  請「重新開機一次」（並確認 BIOS 已開啟虛擬化 VT-x / SVM），" -ForegroundColor Yellow
                Write-Host "  重開機後再雙擊「一鍵開始安裝.bat」即可順利刷機！" -ForegroundColor Yellow
                Write-Host "========================================================" -ForegroundColor Yellow
                exit 1
            }
            throw "匯入 $DistroName 失敗 (rc=$($imp.ExitCode))。若電腦剛開啟 WSL2，請先重新開機一次並確認 BIOS 已開啟虛擬化（VT-x / SVM）。"
        }
        Write-Host "[+] 極輕量 Linux 環境匯入完成"
    } else {
        Write-Host "[+] 偵測到極輕量 Linux 環境 ($DistroName) 已就緒"
    }

    # Boot lightweight WSL2 VM and verify USB/IP vhci_hcd kernel support
    $boot = Invoke-Native -FilePath $wsl -Arguments @("-d", $DistroName, "--", "/bin/sh", "-c", "test -e /sys/devices/platform/vhci_hcd.0/status || modprobe vhci-hcd 2>/dev/null || true") -TimeoutSec 30
    if ($boot.ExitCode -ne 0) {
        if ($boot.Text) { Write-Host $boot.Text -ForegroundColor Yellow }
        throw "無法啟動 $DistroName (rc=$($boot.ExitCode))，請確認電腦已重新開機並開啟 BIOS 虛擬化（VT-x / SVM）。"
    }
}

function Parse-SonyUsbipdDevices([string]$UsbipdExe, [switch]$IncludeDisconnected) {
    $found = @()

    # Prefer structured JSON from `usbipd state` (100% immune to Windows locale / column spacing)
    $st = Invoke-Native -FilePath $UsbipdExe -Arguments @("state")
    if ($st.ExitCode -eq 0 -and $st.Text) {
        try {
            $parsed = $st.Text | ConvertFrom-Json
            $items = if ($parsed -and $parsed.PSObject.Properties["Devices"]) { @($parsed.Devices) } else { @($parsed) }
            foreach ($dev in $items) {
                if ($null -eq $dev) { continue }
                if (-not $IncludeDisconnected -and -not $dev.BusId) { continue }
                if ($dev.InstanceId -match 'VID_054C&PID_(0CA8|0D12|0336)') {
                    $pidHex = $Matches[1].ToLower()
                    $stateStr = "Not shared"
                    if ($dev.ClientIPAddress) {
                        $stateStr = "Attached"
                    } elseif ($dev.PersistedGuid) {
                        $stateStr = "Shared"
                    }
                    $found += [PSCustomObject]@{
                        BusId         = if ($dev.BusId) { [string]$dev.BusId } else { "" }
                        PersistedGuid = if ($dev.PersistedGuid) { [string]$dev.PersistedGuid } else { "" }
                        Pid           = $pidHex
                        Desc          = [string]$dev.Description
                        State         = $stateStr
                    }
                }
            }
            return $found
        } catch {}
    }

    # Fallback: text parsing of `usbipd list`
    $lst = Invoke-Native -FilePath $UsbipdExe -Arguments @("list")
    foreach ($line in $lst.Lines) {
        if ($line -match '^\s*(\d+-\d+(?:\.\d+)*)\s+054c:(0ca8|0d12|0336)\s+(.*?)\s{2,}(Not shared|Shared(?:\s*\(forced\))?|Attached)\s*$') {
            $found += [PSCustomObject]@{
                BusId         = $Matches[1]
                PersistedGuid = ""
                Pid           = $Matches[2].ToLower()
                Desc          = $Matches[3].Trim()
                State         = $Matches[4].Trim()
            }
        }
    }
    return $found
}

function Attach-SonyDevicesOnce([string]$UsbipdExe) {
    $devs = @(Parse-SonyUsbipdDevices $UsbipdExe)
    $attachedAny = $false
    foreach ($d in $devs) {
        if ($null -eq $d -or -not $d.BusId) { continue }
        if ($d.State -eq "Not shared") {
            Invoke-Native -FilePath $UsbipdExe -Arguments @("bind", "--force", "--busid", $d.BusId) | Out-Null
        }
        if ($d.State -ne "Attached") {
            $att = Invoke-Native -FilePath $UsbipdExe -Arguments @("attach", "--wsl", "--busid", $d.BusId)
            if ($att.ExitCode -eq 0) {
                Write-Host "[+] 已透過 usbipd 將相機 (054c:$($d.Pid), bus $($d.BusId)) 透傳至 Linux"
                $attachedAny = $true
            }
        } else {
            $attachedAny = $true
        }
    }
    return $attachedAny
}

function Detach-SonyDevices([string]$UsbipdExe) {
    try {
        # Include disconnected/persisted devices! When 054C:0CA8/0D12 switches to 054C:0336,
        # the Mass Storage PID loses its BusId while keeping its forced PersistedGuid in usbipd.
        $devs = @(Parse-SonyUsbipdDevices $UsbipdExe -IncludeDisconnected)
        foreach ($d in $devs) {
            if ($null -eq $d) { continue }
            if ($d.BusId -and $d.State -eq "Attached") {
                Invoke-Native -FilePath $UsbipdExe -Arguments @("detach", "--busid", $d.BusId) | Out-Null
            }
            if ($d.BusId -and $d.State -ne "Not shared") {
                Invoke-Native -FilePath $UsbipdExe -Arguments @("unbind", "--busid", $d.BusId) | Out-Null
            }
            if ($d.PersistedGuid) {
                Invoke-Native -FilePath $UsbipdExe -Arguments @("unbind", "--guid", $d.PersistedGuid) | Out-Null
            }
        }
    } catch {}
}

function Select-PackageDir([string]$LangChoice) {
    $tc = Join-Path $RootDir "local-boot-package-tc"
    $sc = Join-Path $RootDir "local-boot-package-sc"
    $single = Join-Path $RootDir "local-boot-package"

    if ($LangChoice -eq "sc" -and (Test-Path $sc)) { return $sc }
    if ($LangChoice -eq "tc" -and (Test-Path $tc)) { return $tc }

    if ((Test-Path $tc) -and (Test-Path $sc)) {
        Write-Host ""
        Write-Host "請選擇要安裝的介面語言版本（務必與相機系統語言一致）："
        Write-Host "  [1] 繁體中文版（相機系統語言請設為「繁體中文」）"
        Write-Host "  [2] 簡體中文版（相機系統語言請設為「简体中文」）"
        while ($true) {
            $ans = (Read-Host "請輸入 1 或 2（直接按 Enter 預設 1 繁體中文）").Trim().ToLower()
            if ($ans -in @("", "1", "tc")) {
                Write-Host "[*] 已選擇：繁體中文版"
                return $tc
            }
            if ($ans -in @("2", "sc")) {
                Write-Host "[*] 已選擇：簡體中文版"
                return $sc
            }
            Write-Host "輸入無效，請輸入 1 或 2。"
        }
    }
    if (Test-Path $tc) { return $tc }
    if (Test-Path $sc) { return $sc }
    if (Test-Path $single) { return $single }
    throw "找不到開機套件目錄 (local-boot-package-tc / local-boot-package-sc)"
}

if (-not (Test-IsAdmin)) {
    throw "請以系統管理員身分執行本腳本。"
}

if ($Action -eq "cleanup") {
    $usbipd = Find-UsbipdExe
    if ($usbipd) { Detach-SonyDevices $usbipd }
    $distros = Get-WslDistros
    if ($distros -contains $DistroName) {
        Write-Host "[*] 正在移除極輕量 Linux 部署環境 ($DistroName)..."
        Invoke-Native -FilePath (Find-WslExe) -Arguments @("--unregister", $DistroName) | Out-Null
    }
    if (Test-Path $InstallBase) {
        Remove-Item -Recurse -Force $InstallBase -ErrorAction SilentlyContinue
    }
    Write-Host "[+] 已清理完畢，不佔用硬碟空間。"
    exit 0
}

Write-Host "========================================================"
Write-Host "  Sony A6400 / A6100 — WSL2 極輕量 Linux 全離線部署工具"
Write-Host "  特點：免 Zadig、免 libusb-win32、免疫 Win11 核心隔離、"
Write-Host "        全離線不需翻牆、刷完自動恢復正常讀卡機！"
Write-Host "  事前確認："
Write-Host "  1. 相機：USB連線 = 大容量儲存、USB LUN = 多重"
Write-Host "  2. 記憶卡最上層已建好空資料夾 /A6400/TEST/"
Write-Host "  3. 相機插線開機後靜置 8 秒再繼續"
Write-Host "========================================================"

Ensure-Wsl2Offline
$usbipdExe = Ensure-UsbipdOffline
Ensure-AlpineDistro
$wslExe = Find-WslExe

$deployerPy = "/opt/a6400/pmca_deploy_v3_clite.py"
$localDeployer = Join-Path $ScriptDir "pmca_deploy_v3_clite.py"
if (Test-Path $localDeployer) {
    $deployerPy = Convert-ToWslPath $localDeployer
}
$wslArgs = @($deployerPy, "--pmca-dir", "/opt/pmca-re", "--safe-serial", "--yes")
if ($Action -eq "rollback") {
    $wslArgs += "--rollback"
} elseif ($Action -eq "diag") {
    $wslArgs += "--diag"
} else {
    $pkgWinDir = Select-PackageDir $Lang
    $pkgWslDir = Convert-ToWslPath $pkgWinDir
    $wslArgs += $pkgWslDir
    $customHook = Join-Path $RootDir "a6400-autoexec.so"
    if (Test-Path $customHook) {
        $wslArgs += @("--install-hook", (Convert-ToWslPath $customHook))
    } else {
        $wslArgs += @("--auto-hook-stub", "/opt/a6400/hook-stub.bin")
    }
}

# Keep WSL2 VM awake while attaching Sony USB and running deployer
$keepAlive = Start-Process -FilePath $wslExe -ArgumentList "-d $DistroName -- sleep 600" -WindowStyle Hidden -PassThru

$watcher = $null
$rc = 1
try {
    Write-Host "[*] 正在偵測 Sony 相機 USB (054c:0ca8 / 0d12 / 0336) 並透傳至 Linux..."
    $waitEnd = (Get-Date).AddSeconds(20)
    $ready = $false
    while ((Get-Date) -lt $waitEnd) {
        if (Attach-SonyDevicesOnce $usbipdExe) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 800
    }
    if (-not $ready) {
        Write-Host "[!] 20 秒內未在 Windows 偵測到 Sony 相機 USB (054c:0ca8 / 0d12 / 0336)。" -ForegroundColor Yellow
        Write-Host "    請確認：相機已開機、USB 連線設為「大容量儲存」、使用可傳資料的 USB 線。" -ForegroundColor Yellow
        exit 1
    }

    Start-Sleep -Seconds 1

    # Background auto-attacher: automatically binds & attaches 054c:0336 within 0.7s when camera switches to Service Mode
    $watcher = Start-Job -ScriptBlock {
        param($exe)
        $ErrorActionPreference = "Continue"
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        while ($true) {
            try {
                $raw = (& $exe state 2>$null) -join "`n"
                if ($raw) {
                    $parsed = $raw | ConvertFrom-Json
                    $items = if ($parsed -and $parsed.PSObject.Properties["Devices"]) { @($parsed.Devices) } else { @($parsed) }
                    foreach ($dev in $items) {
                        if ($null -eq $dev -or -not $dev.BusId) { continue }
                        if ($dev.InstanceId -match 'VID_054C&PID_(0CA8|0D12|0336)') {
                            if (-not $dev.PersistedGuid) {
                                & $exe bind --force --busid $dev.BusId *> $null
                            }
                            if (-not $dev.ClientIPAddress) {
                                & $exe attach --wsl --busid $dev.BusId *> $null
                            }
                        }
                    }
                }
            } catch {}
            Start-Sleep -Milliseconds 700
        }
    } -ArgumentList $usbipdExe

    Write-Host "[*] 已開始在極輕量 Linux 內執行部署（切換服務模式時會自動重新透傳，請勿拔線）..."
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $wslExe -d $DistroName -- python3 -u @wslArgs
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }

    if ($rc -eq 1) {
        Write-Host "[*] 相機剛切換至服務模式 (054c:0336)，等待 Linux 接管後自動重試..."
        Start-Sleep -Seconds 3
        Attach-SonyDevicesOnce $usbipdExe | Out-Null
        Start-Sleep -Seconds 1
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $wslExe -d $DistroName -- python3 -u @wslArgs
            $rc = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prev
        }
    }
} finally {
    if ($watcher) {
        Stop-Job -Job $watcher -ErrorAction SilentlyContinue
        Remove-Job -Job $watcher -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[*] 正在解除 USB 透傳，恢復 Windows 正常讀卡機..."
    Detach-SonyDevices $usbipdExe
    if ($keepAlive -and -not $keepAlive.HasExited) {
        Stop-Process -Id $keepAlive.Id -Force -ErrorAction SilentlyContinue
    }
}

if ($rc -eq 0) {
    Write-Host ""
    Write-Host "========================================================" -ForegroundColor Green
    Write-Host "  [+] 部署成功！相機已自動恢復 Windows 正常讀卡機模式。" -ForegroundColor Green
    Write-Host "  下一步：拔線 → 關機扣電池 10 秒 → 開機等 35~40 秒 →" -ForegroundColor Green
    Write-Host "          輕觸螢幕鬆開一次 → 等 2 秒 → 長按 MENU+放大鏡 2 秒！" -ForegroundColor Green
    Write-Host "========================================================" -ForegroundColor Green
}
exit $rc
