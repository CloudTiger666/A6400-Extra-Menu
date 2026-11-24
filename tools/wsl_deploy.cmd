@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "WSL_UTF8=1"
chcp 65001 >nul 2>&1
set "ACTION=%~1"
if "!ACTION!"=="" set "ACTION=install"
set "LANG_ARG=%~2"

if not defined DISTRO set "DISTRO=A6400Deployer"
set "SCRIPT_DIR=%~dp0"
if "!SCRIPT_DIR:~-1!"=="\" set "SCRIPT_DIR=!SCRIPT_DIR:~0,-1!"
for %%I in ("!SCRIPT_DIR!\..") do set "ROOT_DIR=%%~fI"
set "OFFLINE_DIR=!ROOT_DIR!\offline"
set "ROOTFS_TAR=!OFFLINE_DIR!\a6400-alpine-wsl.tar.gz"
set "WSL_FULL_MSI=!OFFLINE_DIR!\wsl.2.4.13.0.x64.msi"
set "WSL_WIN10_MSI=!OFFLINE_DIR!\wsl_update_x64.msi"
set "USBIPD_MSI=!OFFLINE_DIR!\usbipd-win.msi"
set "INSTALL_BASE=!LOCALAPPDATA!\A6400WSL"
set "WATCH_CMD=!SCRIPT_DIR!\usbipd_watch.cmd"
set "RUN_ID=%RANDOM%%RANDOM%"
set "WATCH_LOCK=!TEMP!\a6400_watch_!RUN_ID!.lock"
set "TMP_OUT=!TEMP!\a6400_out_!RUN_ID!.txt"
set "TMP_JSON=!TEMP!\a6400_state_!RUN_ID!.json"
set "TMP_GUIDS=!TEMP!\a6400_guids_!RUN_ID!.txt"
set "NEED_REBOOT=0"
set "WIN_BUILD=0"

reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber >"!TMP_OUT!" 2>nul
for /f "tokens=3" %%B in ('findstr /i "CurrentBuildNumber" "!TMP_OUT!" 2^>nul') do set "WIN_BUILD=%%B"
del /f /q "!TMP_OUT!" >nul 2>&1

fltmc >nul 2>&1
if errorlevel 1 (
    echo [!] Please right-click the .bat file and choose "Run as administrator".
    exit /b 1
)

if /i "!ACTION!"=="cleanup" goto :do_cleanup

echo ========================================================
echo   Sony A6400 / A6100 - WSL2 Offline One-Click Deployer
echo   1. Camera: USB Connection = Mass Storage, LUN = Multi
echo   2. SD Card: /A6400/TEST/ folder created
echo   3. Power on camera, plug USB cable, wait 8 seconds
echo ========================================================
call :warn_bt_red
echo.

echo [Check 1/6] Admin and Windows Build...
echo   [+] Admin OK (Windows Build !WIN_BUILD!)

echo [Check 2/6] Offline package files...
if not exist "!ROOTFS_TAR!" goto :err_missing_rootfs
if not exist "!USBIPD_MSI!" goto :err_missing_usbipd
if exist "!WSL_FULL_MSI!" goto :msg_full_offline
echo   [+] Lite package OK (will auto-download WSL2 MSI via mirror if needed)
goto :check_3

:msg_full_offline
echo   [+] Full offline package OK (includes Microsoft WSL2 2.4.13 MSI)
goto :check_3

:err_missing_rootfs
echo   [X] Missing !ROOTFS_TAR!
echo       Please extract the entire ZIP to a folder first.
exit /b 1

:err_missing_usbipd
echo   [X] Missing !USBIPD_MSI! - please re-extract the ZIP.
exit /b 1

:check_3
echo [Check 3/6] Hypervisor boot setting...
bcdedit /enum {current} >"!TMP_OUT!" 2>nul
findstr /i "hypervisorlaunchtype" "!TMP_OUT!" 2>nul | findstr /i "Off" >nul 2>&1
if not errorlevel 1 goto :fix_hypervisor
del /f /q "!TMP_OUT!" >nul 2>&1
echo   [+] Hypervisor launch type OK
goto :check_4

:fix_hypervisor
del /f /q "!TMP_OUT!" >nul 2>&1
echo   [!] hypervisorlaunchtype was Off, resetting to auto...
bcdedit /set hypervisorlaunchtype auto >nul 2>&1
set "NEED_REBOOT=1"
echo   [+] Fixed hypervisorlaunchtype=auto (reboot required once)

:check_4
echo [Check 4/6] Windows VirtualMachinePlatform (vmcompute)...
sc query vmcompute >nul 2>&1
if not errorlevel 1 goto :vmcompute_exists

echo   [*] Enabling VirtualMachinePlatform offline (/LimitAccess)...
net stop wuauserv >nul 2>&1
net stop bits >nul 2>&1
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /LimitAccess /norestart
set "D1=!errorlevel!"
if !D1! NEQ 0 if !D1! NEQ 3010 (
    echo   [!] Local-only DISM returned !D1!, retrying with Windows Update...
    net start wuauserv >nul 2>&1
    dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
    set "D1=!errorlevel!"
)
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /LimitAccess /norestart >nul 2>&1
set "D2=!errorlevel!"
if !D2! NEQ 0 if !D2! NEQ 3010 (
    dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart >nul 2>&1
    set "D2=!errorlevel!"
)
net start wuauserv >nul 2>&1
if !D1! EQU 0 set "NEED_REBOOT=1"
if !D1! EQU 3010 set "NEED_REBOOT=1"
if !D2! EQU 3010 set "NEED_REBOOT=1"
if !D1! NEQ 0 if !D1! NEQ 3010 goto :err_dism_failed
echo   [+] VirtualMachinePlatform enabled (reboot will be requested below)
goto :check_5

:err_dism_failed
echo   [X] Failed to enable VirtualMachinePlatform (code !D1!).
echo       If 0x800f080c / 0x800f081f, this Windows image lacks WSL2 components.
exit /b 1

:vmcompute_exists
sc config vmcompute start= demand >nul 2>&1
net start vmcompute >nul 2>&1
echo   [+] VirtualMachinePlatform (vmcompute) OK

:check_5
echo [Check 5/6] Microsoft WSL2 engine...
call :find_wsl
set "WSL_OK=0"
if exist "C:\Program Files\WSL\wsl.exe" set "WSL_OK=1"
if "!WSL_OK!"=="0" if exist "%ProgramFiles%\WSL\wsl.exe" set "WSL_OK=1"
if "!WSL_OK!"=="0" (
    dir /b "!LOCALAPPDATA!\Packages\MicrosoftCorporationII.WindowsSubsystemForLinux*" >nul 2>&1
    if not errorlevel 1 set "WSL_OK=1"
)
if "!WSL_OK!"=="1" goto :wsl_ok_done

echo   [*] Installing Microsoft WSL2 engine...
call :ensure_full_wsl_msi
if errorlevel 1 goto :err_wsl_install
if !WIN_BUILD! LSS 22000 if exist "!WSL_WIN10_MSI!" (
    echo   [*] Installing Win10 WSL2 kernel fallback...
    msiexec.exe /i "!WSL_WIN10_MSI!" /qn /norestart
)
call :find_wsl
echo   [+] WSL2 engine installed (!WSL_EXE!)
goto :check_6

:err_wsl_install
echo   [X] Failed to install WSL2 engine. Pause 360/antivirus and retry.
exit /b 1

:wsl_ok_done
echo   [+] WSL2 engine OK (!WSL_EXE!)

:check_6
echo [Check 6/6] usbipd-win USB passthrough service...
call :find_usbipd
if not "!USBIPD_EXE!"=="" goto :usbipd_ok_done
echo   [*] Installing usbipd-win offline...
msiexec.exe /i "!USBIPD_MSI!" /qn /norestart
set "URC=!errorlevel!"
if !URC! NEQ 0 if !URC! NEQ 3010 (
    echo   [!] Silent install blocked rc=!URC!, launching GUI installer...
    msiexec.exe /i "!USBIPD_MSI!"
)
call :find_usbipd
if "!USBIPD_EXE!"=="" goto :err_usbipd_missing
echo   [+] usbipd-win installed
goto :usbipd_start_svc

:err_usbipd_missing
echo   [X] usbipd.exe still not found. Temporarily pause antivirus and retry.
exit /b 1

:usbipd_ok_done
echo   [+] usbipd-win OK (!USBIPD_EXE!)

:usbipd_start_svc
net start usbipd >nul 2>&1

if "!NEED_REBOOT!"=="1" goto :prompt_reboot
goto :check_alpine

:prompt_reboot
echo.
echo ========================================================
echo   [+] Missing Windows features have been installed!
echo   [!] Please RESTART Windows once (click Restart, not Shut Down),
echo       then run this installer again.
echo ========================================================
exit /b 0

:check_alpine
echo.
echo [*] Checking Alpine Linux environment (!DISTRO!)...
"!WSL_EXE!" --set-default-version 2 >nul 2>&1
"!WSL_EXE!" -l -v >nul 2>&1
"!WSL_EXE!" -d !DISTRO! -- /bin/sh -c "test -f /opt/a6400/pmca_deploy_v3_clite.py" >nul 2>&1
if !errorlevel! EQU 0 goto :alpine_ready

rem Reuse any already-working A6400Deployer_* instance if primary is broken
"!WSL_EXE!" -l -q >"!TMP_OUT!" 2>nul
for /f "usebackq tokens=1" %%D in ("!TMP_OUT!") do (
    set "_DN=%%D"
    if /i "!_DN:~0,13!"=="A6400Deployer" (
        "!WSL_EXE!" -d %%D -- /bin/sh -c "test -f /opt/a6400/pmca_deploy_v3_clite.py" >nul 2>&1
        if !errorlevel! EQU 0 (
            set "DISTRO=%%D"
            goto :alpine_ready
        )
    )
)
del /f /q "!TMP_OUT!" >nul 2>&1

"!WSL_EXE!" --shutdown >nul 2>&1
"!WSL_EXE!" --unregister !DISTRO! >nul 2>&1
if exist "!INSTALL_BASE!" rmdir /s /q "!INSTALL_BASE!" >nul 2>&1
echo [*] Importing Alpine Linux (!DISTRO!), takes ~3-5 seconds...
if not exist "!INSTALL_BASE!" mkdir "!INSTALL_BASE!"
"!WSL_EXE!" --import !DISTRO! "!INSTALL_BASE!" "!ROOTFS_TAR!" --version 2 >"!TMP_OUT!" 2>&1
if !errorlevel! EQU 0 goto :alpine_imported

type "!TMP_OUT!"
findstr /i "ERROR_ALREADY_EXISTS" "!TMP_OUT!" >nul 2>&1
if !errorlevel! NEQ 0 goto :err_import_failed

set "DISTRO=A6400Deployer_!RUN_ID!"
set "INSTALL_BASE=!LOCALAPPDATA!\A6400WSL_!RUN_ID!"
echo [*] Retrying import with fresh instance name !DISTRO!...
if not exist "!INSTALL_BASE!" mkdir "!INSTALL_BASE!"
"!WSL_EXE!" --import !DISTRO! "!INSTALL_BASE!" "!ROOTFS_TAR!" --version 2
if !errorlevel! NEQ 0 goto :err_import_failed

:alpine_imported
del /f /q "!TMP_OUT!" >nul 2>&1
echo [+] Alpine Linux imported, refreshing WSL service...
"!WSL_EXE!" -l -v >nul 2>&1
ping -n 3 127.0.0.1 >nul
goto :alpine_boot_check

:err_import_failed
echo.
echo ========================================================
echo   [X] Failed to import !DISTRO!.
echo   1. If you just enabled VirtualMachinePlatform, RESTART PC once.
echo   2. Check BIOS virtualization (Intel VT-x / AMD SVM) is Enabled.
echo   3. Pause 360/antivirus if it blocks virtualization.
echo ========================================================
exit /b 1

:alpine_ready
echo [+] Alpine Linux (!DISTRO!) OK

:alpine_boot_check
set "BOOT_OK=0"
for /l %%B in (1,1,3) do (
    if "!BOOT_OK!"=="0" (
        "!WSL_EXE!" -l -v >nul 2>&1
        "!WSL_EXE!" -d !DISTRO! -- /bin/sh -c "test -e /sys/devices/platform/vhci_hcd.0/status || modprobe vhci-hcd 2>/dev/null || true"
        if !errorlevel! EQU 0 (
            set "BOOT_OK=1"
        ) else (
            ping -n 3 127.0.0.1 >nul
        )
    )
)
if "!BOOT_OK!"=="0" (
    echo [X] Cannot boot !DISTRO!. Restart PC and verify BIOS VT-x / SVM is Enabled.
    exit /b 1
)

set "PKG_WSL="
set "HOOK_WSL="
set "DIAG_WSL="
if /i "!ACTION!"=="rollback" goto :start_usb_phase
if /i "!ACTION!"=="diag" (
    call :win_to_wsl "!ROOT_DIR!\a6400-camera-log.txt" DIAG_WSL
    goto :start_usb_phase
)

call :select_package
if errorlevel 1 exit /b 1
call :win_to_wsl "!PKG_WIN!" PKG_WSL
if "!PKG_WSL!"=="" (
    echo [X] Failed to convert path to WSL: !PKG_WIN!
    exit /b 1
)
if exist "!ROOT_DIR!\a6400-autoexec.so" (
    call :win_to_wsl "!ROOT_DIR!\a6400-autoexec.so" HOOK_WSL
)

:start_usb_phase
start "" /b "!WSL_EXE!" -d !DISTRO! -- timeout 600 /bin/sh -c "touch /tmp/.a6400_keepalive; while [ -f /tmp/.a6400_keepalive ]; do sleep 1; done" <nul >nul 2>&1
ping -n 2 127.0.0.1 >nul

echo.
echo [*] Waiting for Sony camera USB (054c:0ca8 / 0d12 / 0336)...
set "ATTACHED=0"
for /l %%T in (1,1,20) do (
    if "!ATTACHED!"=="0" (
        call :attach_sony_once
        if "!ATTACH_HIT!"=="1" (
            set "ATTACHED=1"
        ) else (
            ping -n 2 127.0.0.1 >nul
        )
    )
)

if "!ATTACHED!"=="0" (
    echo [!] No Sony camera USB detected within 20s [054c:0ca8 / 0d12 / 0336].
    echo     Check: camera ON, USB = Mass Storage, data-capable USB cable.
    set "DEPLOY_RC=1"
    goto :cleanup_session
)

call :wait_linux_usb
call :sync_deployer_scripts
echo running>"!WATCH_LOCK!"
start "" /b cmd /c call "!WATCH_CMD!" "!USBIPD_EXE!" "!WATCH_LOCK!" >nul 2>&1

echo [*] Running Linux deployer (do not unplug while switching to Service Mode)...
call :run_python_deployer
set "DEPLOY_RC=!errorlevel!"

if "!DEPLOY_RC!"=="1" (
    for /l %%R in (1,1,2) do (
        if "!DEPLOY_RC!"=="1" (
            echo [*] Camera switched to Service Mode [054c:0336], waiting to re-attach [retry %%R/2]...
            set "REATTACH_OK=0"
            for /l %%W in (1,1,18) do (
                if "!REATTACH_OK!"=="0" (
                    call :attach_sony_once
                    if "!ATTACH_HIT!"=="1" (
                        set "REATTACH_OK=1"
                    ) else (
                        ping -n 2 127.0.0.1 >nul
                    )
                )
            )
            call :wait_linux_usb
            call :run_python_deployer
            set "DEPLOY_RC=!errorlevel!"
        )
    )
)

:cleanup_session
if exist "!WATCH_LOCK!" del /f /q "!WATCH_LOCK!" >nul 2>&1
ping -n 2 127.0.0.1 >nul
echo [*] Detaching USB passthrough and restoring Windows card reader...
call :detach_all_sony
"!WSL_EXE!" -d !DISTRO! -- rm -f /tmp/.a6400_keepalive <nul >nul 2>&1
del /f /q "!TMP_OUT!" "!TMP_JSON!" "!TMP_GUIDS!" >nul 2>&1

if "!DEPLOY_RC!"=="0" (
    if /i "!ACTION!"=="diag" (
        echo.
        echo ========================================================
        echo   [+] Camera log saved to: !ROOT_DIR!\a6400-camera-log.txt
        echo ========================================================
        exit /b 0
    )
    if /i "!ACTION!"=="rollback" (
        echo.
        echo ========================================================
        echo   [+] FACTORY RESTORE COMPLETE! Windows card reader restored.
        echo   Next: Unplug USB -^> Power off -^> Pull battery 10s -^>
        echo         Power on. Camera is now 100%% stock factory state.
        echo ========================================================
        exit /b 0
    )
    echo.
    echo ========================================================
    echo   [+] SUCCESS! Windows card reader restored.
    echo ========================================================
    call :warn_bt_red
    echo   Next: Unplug USB -^> Make sure Bluetooth=ON ^& Touch=ON -^>
    echo         Power off -^> Pull battery 10s -^> Power on -^>
    echo         Wait 10s -^> Tap screen once -^> Wait 2s -^>
    echo         Hold MENU + Magnifier [AEL] for 2s
    echo ========================================================
)
exit /b !DEPLOY_RC!

:warn_bt_red
powershell -NoProfile -EncodedCommand VwByAGkAdABlAC0ASABvAHMAdAAgACcAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACcAIAAtAEYAbwByAGUAZwByAG8AdQBuAGQAQwBvAGwAbwByACAAUgBlAGQAOwBXAHIAaQB0AGUALQBIAG8AcwB0ACAAJwAgACAAWwDNkYGJxV+LlSEAXQAgAPh2X2oATppbgYmLlV9VDDDNhVlyn1L9gCAAPQAgAIuVDTAoANuYKoIhag9fIAA9ACAA3JUpACEAJwAgAC0ARgBvAHIAZQBnAHIAbwB1AG4AZABDAG8AbABvAHIAIABSAGUAZAA7AFcAcgBpAHQAZQAtAEgAbwBzAHQAIAAnACAAIABbAEkATQBQAE8AUgBUAEEATgBUAF0AIABDAEEATQBFAFIAQQAgAEIATABVAEUAVABPAE8AVABIACAATQBVAFMAVAAgAEIARQAgAE8ATgAgACgAQQBpAHIAcABsAGEAbgBlAD0ATwBGAEYAKQAhACcAIAAtAEYAbwByAGUAZwByAG8AdQBuAGQAQwBvAGwAbwByACAAUgBlAGQAOwBXAHIAaQB0AGUALQBIAG8AcwB0ACAAJwAgACAALQAgAIxbaFENTih1TZENXEtiX2oM/+pTgYkHUjBSDDCLlQ0wG/8NTouVzYVZciAAMQAwADAAJQAgAJJseJCuVQH/JwAgAC0ARgBvAHIAZQBnAHIAbwB1AG4AZABDAG8AbABvAHIAIABSAGUAZAA7AFcAcgBpAHQAZQAtAEgAbwBzAHQAIAAnACAAIAAtACAADFRCZrp4jYoMMPiJp2PNZFxPIAA9ACAAi5UNMAz/N1KMW9Ri+5ZgbCAAMQAwACAA0nmNURWP+Imih1VeAf8nACAALQBGAG8AcgBlAGcAcgBvAHUAbgBkAEMAbwBsAG8AcgAgAFIAZQBkADsAVwByAGkAdABlAC0ASABvAHMAdAAgACcAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACEAIQAhACcAIAAtAEYAbwByAGUAZwByAG8AdQBuAGQAQwBvAGwAbwByACAAUgBlAGQA 2>nul
if not errorlevel 1 exit /b 0
echo ========================================================
echo   [IMPORTANT] CAMERA BLUETOOTH MUST BE ON [Airplane=OFF]
echo   No phone pairing needed. Without Bluetooth=ON, NO MENU.
echo   Also make sure Touch Operation = ON, pull battery 10s.
echo ========================================================
exit /b 0

:run_python_deployer
if /i "!ACTION!"=="rollback" (
    "!WSL_EXE!" -d !DISTRO! -- python3 -u /opt/a6400/pmca_deploy_v3_clite.py --pmca-dir /opt/pmca-re --safe-serial --yes --rollback
    exit /b !errorlevel!
)
if /i "!ACTION!"=="diag" (
    if not "!DIAG_WSL!"=="" (
        "!WSL_EXE!" -d !DISTRO! -- python3 -u /opt/a6400/pmca_deploy_v3_clite.py --pmca-dir /opt/pmca-re --safe-serial --yes --diag --diag-out "!DIAG_WSL!"
    ) else (
        "!WSL_EXE!" -d !DISTRO! -- python3 -u /opt/a6400/pmca_deploy_v3_clite.py --pmca-dir /opt/pmca-re --safe-serial --yes --diag
    )
    exit /b !errorlevel!
)
if not "!HOOK_WSL!"=="" (
    "!WSL_EXE!" -d !DISTRO! -- python3 -u /opt/a6400/pmca_deploy_v3_clite.py --pmca-dir /opt/pmca-re --safe-serial --yes "!PKG_WSL!" --install-hook "!HOOK_WSL!"
    exit /b !errorlevel!
)
"!WSL_EXE!" -d !DISTRO! -- python3 -u /opt/a6400/pmca_deploy_v3_clite.py --pmca-dir /opt/pmca-re --safe-serial --yes "!PKG_WSL!" --auto-hook-stub /opt/a6400/hook-stub.bin
exit /b !errorlevel!

:do_cleanup
echo [*] Cleaning up Sony USB bindings and !DISTRO!...
call :find_usbipd
call :find_wsl
if not "!USBIPD_EXE!"=="" call :detach_all_sony
if not "!WSL_EXE!"=="" (
    "!WSL_EXE!" --shutdown <nul >nul 2>&1
    "!WSL_EXE!" --unregister !DISTRO! <nul >nul 2>&1
    "!WSL_EXE!" -l -q <nul >"!TMP_OUT!" 2>nul
    for /f "usebackq tokens=1" %%D in ("!TMP_OUT!") do (
        set "_DN=%%D"
        if /i "!_DN:~0,13!"=="A6400Deployer" "!WSL_EXE!" --unregister %%D <nul >nul 2>&1
    )
)
for /d %%P in ("!LOCALAPPDATA!\A6400WSL*") do rmdir /s /q "%%P" >nul 2>&1
del /f /q "!TMP_OUT!" "!TMP_JSON!" "!TMP_GUIDS!" >nul 2>&1
echo [+] Cleanup complete.
exit /b 0

:find_wsl
set "WSL_EXE="
if exist "C:\Program Files\WSL\wsl.exe" (
    set "WSL_EXE=C:\Program Files\WSL\wsl.exe"
    exit /b 0
)
if exist "%ProgramFiles%\WSL\wsl.exe" (
    set "WSL_EXE=%ProgramFiles%\WSL\wsl.exe"
    exit /b 0
)
if exist "C:\Windows\System32\wsl.exe" (
    set "WSL_EXE=C:\Windows\System32\wsl.exe"
    exit /b 0
)
exit /b 0

:find_usbipd
set "USBIPD_EXE="
if exist "C:\Program Files\usbipd-win\usbipd.exe" (
    set "USBIPD_EXE=C:\Program Files\usbipd-win\usbipd.exe"
    exit /b 0
)
if exist "%ProgramFiles%\usbipd-win\usbipd.exe" (
    set "USBIPD_EXE=%ProgramFiles%\usbipd-win\usbipd.exe"
    exit /b 0
)
where usbipd.exe >"!TMP_OUT!" 2>nul
for /f "usebackq delims=" %%P in ("!TMP_OUT!") do (
    if "!USBIPD_EXE!"=="" set "USBIPD_EXE=%%P"
)
del /f /q "!TMP_OUT!" >nul 2>&1
exit /b 0

:ensure_full_wsl_msi
if exist "!WSL_FULL_MSI!" goto :install_full_wsl_msi
echo   [*] Downloading Microsoft WSL2 MSI (~196 MB, cached to offline\)...
set "DL_OK=0"
for %%U in (
    "https://gh-proxy.com/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi"
    "https://ghfast.top/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi"
    "https://ghproxy.net/https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi"
    "https://github.com/microsoft/WSL/releases/download/2.4.13/wsl.2.4.13.0.x64.msi"
) do (
    if "!DL_OK!"=="0" (
        echo   [*] Trying mirror: %%~U
        del /f /q "!WSL_FULL_MSI!.part" >nul 2>&1
        curl.exe -fL --connect-timeout 15 --max-time 600 -# -o "!WSL_FULL_MSI!.part" "%%~U"
        if not errorlevel 1 (
            for %%S in ("!WSL_FULL_MSI!.part") do (
                if %%~zS GTR 100000000 (
                    move /y "!WSL_FULL_MSI!.part" "!WSL_FULL_MSI!" >nul
                    set "DL_OK=1"
                )
            )
        )
        if "!DL_OK!"=="0" del /f /q "!WSL_FULL_MSI!.part" >nul 2>&1
    )
)
if "!DL_OK!"=="0" exit /b 1

:install_full_wsl_msi
echo   [*] Installing Microsoft WSL2 MSI (15-30s)...
msiexec.exe /i "!WSL_FULL_MSI!" /qn /norestart
set "MRC=!errorlevel!"
if !MRC! EQU 3010 set "NEED_REBOOT=1"
if !MRC! EQU 0 exit /b 0
if !MRC! EQU 3010 exit /b 0
echo   [!] Silent install returned !MRC!, launching GUI installer...
msiexec.exe /i "!WSL_FULL_MSI!"
if exist "C:\Program Files\WSL\wsl.exe" exit /b 0
exit /b 1

:select_package
set "PKG_WIN="
set "TC_DIR=!ROOT_DIR!\local-boot-package-tc"
set "SC_DIR=!ROOT_DIR!\local-boot-package-sc"
set "SINGLE_DIR=!ROOT_DIR!\local-boot-package"
if /i "!LANG_ARG!"=="sc" if exist "!SC_DIR!" (
    set "PKG_WIN=!SC_DIR!"
    exit /b 0
)
if /i "!LANG_ARG!"=="tc" if exist "!TC_DIR!" (
    set "PKG_WIN=!TC_DIR!"
    exit /b 0
)
if exist "!TC_DIR!" if exist "!SC_DIR!" goto :ask_lang_prompt
if exist "!TC_DIR!" (
    set "PKG_WIN=!TC_DIR!"
    exit /b 0
)
if exist "!SC_DIR!" (
    set "PKG_WIN=!SC_DIR!"
    exit /b 0
)
if exist "!SINGLE_DIR!" (
    set "PKG_WIN=!SINGLE_DIR!"
    exit /b 0
)
echo [X] Missing local-boot-package-tc / local-boot-package-sc
exit /b 1

:ask_lang_prompt
echo.
echo Select UI Language (must match camera menu language):
echo   [1] Traditional Chinese (TC)
echo   [2] Simplified Chinese  (SC)
:ask_lang_loop
set "ANS="
set /p "ANS=Enter 1 or 2 (default 1 = TC): "
if "!ANS!"=="" set "ANS=1"
if /i "!ANS!"=="1" goto :pick_tc
if /i "!ANS!"=="tc" goto :pick_tc
if /i "!ANS!"=="2" goto :pick_sc
if /i "!ANS!"=="sc" goto :pick_sc
echo Invalid input, enter 1 or 2.
goto :ask_lang_loop

:pick_tc
echo [*] Selected: Traditional Chinese (TC)
set "PKG_WIN=!TC_DIR!"
exit /b 0

:pick_sc
echo [*] Selected: Simplified Chinese (SC)
set "PKG_WIN=!SC_DIR!"
exit /b 0

:win_to_wsl
set "_WIN_IN=%~1"
set "_OUT_VAR=%~2"
set "!_OUT_VAR!="
del /f /q "!TMP_OUT!" >nul 2>&1
"!WSL_EXE!" -d !DISTRO! -- python3 -c "import sys; p=sys.argv[1].replace(chr(92),'/'); print('/mnt/'+p[0].lower()+p[2:] if len(p)>2 and p[1]==':' else '')" "!_WIN_IN!" <nul >"!TMP_OUT!" 2>nul
if exist "!TMP_OUT!" (
    for /f "usebackq delims=" %%W in ("!TMP_OUT!") do set "!_OUT_VAR!=%%W"
    del /f /q "!TMP_OUT!" >nul 2>&1
)
exit /b 0

:sync_deployer_scripts
if exist "!SCRIPT_DIR!\pmca_deploy_v3_clite.py" (
    call :win_to_wsl "!SCRIPT_DIR!" TOOLS_WSL
    if not "!TOOLS_WSL!"=="" (
        "!WSL_EXE!" -d !DISTRO! -- python3 -c "import os,shutil,sys; src=sys.argv[1]; dst='/opt/a6400'; [shutil.copy2(os.path.join(src,f), os.path.join(dst,f)) for f in ('pmca_deploy_v3_clite.py','build_autoexec_hook.py') if os.path.isfile(os.path.join(src,f))]; os.chmod(os.path.join(dst,'pmca_deploy_v3_clite.py'),0o755); shutil.rmtree(os.path.join(dst,'__pycache__'),ignore_errors=True)" "!TOOLS_WSL!" <nul >nul 2>&1
    )
)
exit /b 0

:wait_linux_usb
for /l %%U in (1,1,6) do (
    "!WSL_EXE!" -d !DISTRO! -- /bin/sh -c "grep -qi 054c /sys/bus/usb/devices/*/idVendor 2>/dev/null" <nul >nul 2>&1
    if !errorlevel! EQU 0 exit /b 0
    ping -n 2 127.0.0.1 >nul
)
exit /b 0

:attach_sony_once
set "ATTACH_HIT=0"
"!USBIPD_EXE!" list >"!TMP_OUT!" 2>nul
for /f "usebackq delims=" %%L in ("!TMP_OUT!") do (
    set "_LINE=%%L"
    for /f "tokens=1,2" %%A in ("%%L") do (
        set "_VP=%%B"
        set "_HIT=0"
        if /i "!_VP!"=="054c:0ca8" set "_HIT=1"
        if /i "!_VP!"=="054c:0d12" set "_HIT=1"
        if /i "!_VP!"=="054c:0336" set "_HIT=1"
        if "!_HIT!"=="1" (
            if not "!_LINE:Attached=!"=="!_LINE!" (
                set "ATTACH_HIT=1"
            ) else (
                if not "!_LINE:Not shared=!"=="!_LINE!" (
                    "!USBIPD_EXE!" bind --force --busid %%A >nul 2>&1
                )
                "!USBIPD_EXE!" attach --wsl --busid %%A >nul 2>&1
                if !errorlevel! EQU 0 (
                    echo [+] Attached camera %%B bus %%A to Linux
                    set "ATTACH_HIT=1"
                ) else (
                    "!USBIPD_EXE!" list 2>nul | findstr /i "%%A" | findstr /i "Attached" >nul 2>&1 && set "ATTACH_HIT=1"
                )
            )
        )
    )
)
del /f /q "!TMP_OUT!" >nul 2>&1
exit /b 0

:detach_all_sony
"!USBIPD_EXE!" list >"!TMP_OUT!" 2>nul
for /f "usebackq tokens=1,2" %%A in ("!TMP_OUT!") do (
    set "_VP=%%B"
    set "_HIT=0"
    if /i "!_VP!"=="054c:0ca8" set "_HIT=1"
    if /i "!_VP!"=="054c:0d12" set "_HIT=1"
    if /i "!_VP!"=="054c:0336" set "_HIT=1"
    if "!_HIT!"=="1" (
        "!USBIPD_EXE!" detach --busid %%A >nul 2>&1
        "!USBIPD_EXE!" unbind --busid %%A >nul 2>&1
    )
)
del /f /q "!TMP_OUT!" >nul 2>&1
if "!WSL_EXE!"=="" exit /b 0
"!USBIPD_EXE!" state >"!TMP_JSON!" 2>nul
if not exist "!TMP_JSON!" exit /b 0
"!WSL_EXE!" -d !DISTRO! -- python3 -c "import sys,json,re; raw=sys.stdin.read().strip(); d=json.loads(raw) if raw else {}; items=d.get('Devices',d) if isinstance(d,dict) else d; [print(x['PersistedGuid']) for x in items if isinstance(x,dict) and x.get('PersistedGuid') and re.search(r'VID_054C&PID_(0CA8|0D12|0336)',str(x.get('InstanceId','')),re.I)]" <"!TMP_JSON!" >"!TMP_GUIDS!" 2>nul
if exist "!TMP_GUIDS!" (
    for /f "usebackq delims=" %%G in ("!TMP_GUIDS!") do (
        "!USBIPD_EXE!" unbind --guid %%G >nul 2>&1
    )
)
del /f /q "!TMP_JSON!" "!TMP_GUIDS!" >nul 2>&1
exit /b 0
