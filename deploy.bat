@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  USB Host Proxy - Teensy Core Deploy Tool
REM  Pushes firmware\ to the live Teensy core install location
REM  (auto-detected under %LOCALAPPDATA%\Arduino15\packages\teensy
REM  \hardware\avr\<version>), with one-time pristine backup and
REM  restore.
REM ============================================================

REM --- Path resolution (no hardcoded user) ---
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

set "FIRMWARE=%REPO%\firmware"
set "BACKUP_ZIP=%REPO%\teensy_core_backup.zip"
set "CACHE_A=%APPDATA%\Arduino IDE"
set "CACHE_B=%APPDATA%\arduino-ide"

REM Vendor file/dir set - the only paths deploy/restore/backup touch
set "VENDOR_ITEMS=cores libraries boards.txt platform.txt keywords.txt installed.json"

REM --- Sanity check: firmware\ must exist in the repo ---
if not exist "%FIRMWARE%\boards.txt" (
    echo [ERROR] firmware\boards.txt not found at "%FIRMWARE%".
    echo This script must live at the repo root next to the firmware\ folder.
    pause
    exit /b 1
)

REM --- Detect installed Teensy core (deploy target) ---
set "TEENSY_BASE=%LOCALAPPDATA%\Arduino15\packages\teensy\hardware\avr"
if not exist "%TEENSY_BASE%\" (
    echo [ERROR] Arduino Teensy core base path not found:
    echo         %TEENSY_BASE%
    echo Install the Teensy boards package via Arduino IDE first.
    pause
    exit /b 1
)

set "INSTALL_TARGET="
set "VERSION_COUNT=0"
set "FIRST_VERSION="
for /d %%V in ("%TEENSY_BASE%\*") do (
    set /a VERSION_COUNT+=1
    if not defined FIRST_VERSION set "FIRST_VERSION=%%V"
)

if "%VERSION_COUNT%"=="0" (
    echo [ERROR] No Teensy core versions installed under
    echo         %TEENSY_BASE%
    pause
    exit /b 1
)

if "%VERSION_COUNT%"=="1" (
    set "INSTALL_TARGET=%FIRST_VERSION%"
) else (
    echo Multiple Teensy core versions detected under
    echo   %TEENSY_BASE%
    echo.
    set "IDX=0"
    for /d %%V in ("%TEENSY_BASE%\*") do (
        set /a IDX+=1
        set "VER_!IDX!=%%V"
        echo   [!IDX!] %%~nxV
    )
    echo.
    set "PICKED="
    set /p "PICKED=Select version to target: "
    call set "INSTALL_TARGET=%%VER_!PICKED!%%"
    if not defined INSTALL_TARGET (
        echo Invalid selection.
        pause
        exit /b 1
    )
)

:MENU
cls
echo ===============================================
echo   USB Host Proxy - Teensy Core Deploy Tool
echo ===============================================
echo   Repo:    %REPO%
echo   Target:  %INSTALL_TARGET%
if exist "%BACKUP_ZIP%" (
    echo   Backup:  PRESENT   ^(%BACKUP_ZIP%^)
) else (
    echo   Backup:  MISSING
)
echo ===============================================
echo.
echo   [1] Backup pristine Teensy core (one-time)
echo   [2] Deploy firmware\ to live core
echo   [3] Restore pristine core from backup
echo   [4] Exit
echo.
set "CHOICE="
set /p "CHOICE=Select option: "
if "%CHOICE%"=="1" goto BACKUP
if "%CHOICE%"=="2" goto DEPLOY
if "%CHOICE%"=="3" goto RESTORE
if "%CHOICE%"=="4" goto END
echo Invalid choice.
pause
goto MENU

REM ============================================================
:BACKUP
REM ============================================================
echo.
if exist "%BACKUP_ZIP%" (
    echo [ABORT] Backup already exists: %BACKUP_ZIP%
    echo This is a one-time backup. Delete the zip manually if you really
    echo want to recreate it from the live core's current contents.
    pause
    goto MENU
)

REM Backup source: the live install target (pristine vendor core).
set "MISSING="
for %%I in (%VENDOR_ITEMS%) do (
    if not exist "%INSTALL_TARGET%\%%I" set "MISSING=!MISSING! %%I"
)
if defined MISSING (
    echo [ABORT] These vendor items are missing under
    echo         %INSTALL_TARGET%
    echo         missing:!MISSING!
    pause
    goto MENU
)

echo Creating %BACKUP_ZIP% from %INSTALL_TARGET% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%INSTALL_TARGET%\cores','%INSTALL_TARGET%\libraries','%INSTALL_TARGET%\boards.txt','%INSTALL_TARGET%\platform.txt','%INSTALL_TARGET%\keywords.txt','%INSTALL_TARGET%\installed.json' -DestinationPath '%BACKUP_ZIP%' -Force"
if errorlevel 1 (
    echo [ERROR] Compress-Archive failed.
    pause
    goto MENU
)
for %%A in ("%BACKUP_ZIP%") do set "ZIP_SIZE=%%~zA"
echo.
echo [OK] Backup created. Size: !ZIP_SIZE! bytes
pause
goto MENU

REM ============================================================
:DEPLOY
REM ============================================================
echo.
if not exist "%BACKUP_ZIP%" (
    echo [WARN] No pristine backup found. You will not be able to restore.
    set "CONT="
    set /p "CONT=Proceed without backup? (Y/N): "
    if /i not "!CONT!"=="Y" goto MENU
)
set "OK="
set /p "OK=Deploy firmware\ to %INSTALL_TARGET%? This overwrites the live core. (Y/N): "
if /i not "%OK%"=="Y" goto MENU

call :CLOSE_IDE
call :WIPE_CACHES
call :WIPE_VENDOR

echo Copying firmware\ contents to %INSTALL_TARGET% ...
xcopy "%FIRMWARE%\*" "%INSTALL_TARGET%\" /E /I /Y /Q >nul
if errorlevel 1 (
    echo [ERROR] xcopy failed.
    call :REOPEN_IDE
    pause
    goto MENU
)
echo [OK] Deploy complete.
REM Small settle so filesystem metadata flushes before IDE relaunch.
ping -n 2 127.0.0.1 >nul
call :REOPEN_IDE
pause
goto MENU

REM ============================================================
:RESTORE
REM ============================================================
echo.
if not exist "%BACKUP_ZIP%" (
    echo [ABORT] No backup zip at %BACKUP_ZIP%.
    echo Run [1] before any deploy if you want a restore point.
    pause
    goto MENU
)
set "OK="
set /p "OK=Restore pristine core to %INSTALL_TARGET%? (Y/N): "
if /i not "%OK%"=="Y" goto MENU

call :CLOSE_IDE
call :WIPE_CACHES
call :WIPE_VENDOR

echo Expanding %BACKUP_ZIP% to %INSTALL_TARGET% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path '%BACKUP_ZIP%' -DestinationPath '%INSTALL_TARGET%' -Force"
if errorlevel 1 (
    echo [ERROR] Expand-Archive failed.
    call :REOPEN_IDE
    pause
    goto MENU
)
echo [OK] Restore complete.
REM Small settle so filesystem metadata flushes before IDE relaunch.
ping -n 2 127.0.0.1 >nul
call :REOPEN_IDE
pause
goto MENU

REM ============================================================
:CLOSE_IDE
REM ============================================================
REM Decide whether the visible IDE was actually open, then kill EVERY
REM Arduino-related process (IDE + helpers + arduino-cli daemon +
REM serial-/mdns-/dfu-discovery + language server) so the file copy
REM and cache wipe can proceed cleanly.
set "IDE_WAS_RUNNING=0"
set "IDE_PATH="

REM Electron apps that are actually open always have multiple
REM Arduino IDE.exe instances. A single match is a leftover zombie.
set "IDE_COUNT=0"
for /f %%C in ('powershell -NoProfile -Command "@(Get-Process 'Arduino IDE' -ErrorAction SilentlyContinue).Count"') do set "IDE_COUNT=%%C"
if %IDE_COUNT% GEQ 2 set "IDE_WAS_RUNNING=1"

if exist "%LOCALAPPDATA%\Programs\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%LOCALAPPDATA%\Programs\Arduino IDE\Arduino IDE.exe"
) else if exist "%ProgramFiles%\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%ProgramFiles%\Arduino IDE\Arduino IDE.exe"
) else if exist "%ProgramFiles(x86)%\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%ProgramFiles(x86)%\Arduino IDE\Arduino IDE.exe"
)

echo Closing any running Arduino processes ...
REM Kill any process whose executable path contains 'arduino' (case
REM insensitive). Catches: the IDE itself, Electron helpers, bundled
REM arduino-cli, serial-/mdns-/dfu-discovery, the language server, and
REM any build tools spawned from %APPDATA%\Arduino15 or .arduinoIDE.
REM Loops up to ~10s, re-killing on each pass so Electron helpers do
REM not respawn faster than we can stop them.
powershell -NoProfile -Command "$n=0; while ($n -lt 20) { $p = Get-Process | Where-Object { $_.Path -and $_.Path -match 'arduino' }; if (-not $p) { break }; $p | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500; $n++ }"
goto :EOF

REM ============================================================
:WIPE_CACHES
REM ============================================================
echo Clearing Arduino IDE caches ...
if exist "%CACHE_A%" rmdir /S /Q "%CACHE_A%"
if exist "%CACHE_B%" rmdir /S /Q "%CACHE_B%"
goto :EOF

REM ============================================================
:WIPE_VENDOR
REM ============================================================
echo Removing existing vendor files from %INSTALL_TARGET% ...
for %%I in (%VENDOR_ITEMS%) do (
    if exist "%INSTALL_TARGET%\%%I\" (
        rmdir /S /Q "%INSTALL_TARGET%\%%I"
    ) else if exist "%INSTALL_TARGET%\%%I" (
        del /F /Q "%INSTALL_TARGET%\%%I"
    )
)
goto :EOF

REM ============================================================
:REOPEN_IDE
REM ============================================================
if "%IDE_WAS_RUNNING%"=="1" (
    if defined IDE_PATH (
        echo Relaunching Arduino IDE ...
        REM Use PowerShell Start-Process so the IDE is a fully detached
        REM child of the OS, not of this cmd. Otherwise the IDE inherits
        REM our console handles and dumps its logs into the terminal.
        powershell -NoProfile -Command "Start-Process -FilePath '%IDE_PATH%'"
    ) else (
        echo [WARN] Arduino IDE was running but its install path was not found.
        echo        Please reopen the IDE manually.
    )
)
goto :EOF

:END
endlocal
exit /b 0
