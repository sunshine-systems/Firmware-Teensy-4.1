@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM  USB Host Proxy - Teensy Core Deploy Tool
REM  Pushes firmware\ to the live Teensy core install location,
REM  with one-time pristine backup and restore.
REM ============================================================

REM --- Path resolution (no hardcoded user) ---
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

set "FIRMWARE=%REPO%\firmware"
set "BACKUP_ZIP=%REPO%\teensy_core_backup.zip"
set "CACHE_A=%APPDATA%\Arduino IDE"
set "CACHE_B=%APPDATA%\arduino-ide"

REM Vendor file/dir set (relative to REPO) - the only paths deploy/restore touch
set "VENDOR_ITEMS=cores libraries boards.txt platform.txt keywords.txt installed.json"

REM --- Sanity check: must be run from a firmware repo ---
if not exist "%FIRMWARE%\boards.txt" (
    echo [ERROR] firmware\boards.txt not found at "%FIRMWARE%".
    echo This script must live at the repo root next to the firmware\ folder.
    pause
    exit /b 1
)

:MENU
cls
echo ===============================================
echo   USB Host Proxy - Teensy Core Deploy Tool
echo ===============================================
echo   Repo:   %REPO%
if exist "%BACKUP_ZIP%" (
    echo   Backup: PRESENT   ^(%BACKUP_ZIP%^)
) else (
    echo   Backup: MISSING
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
    echo want to recreate it from the current firmware\ contents.
    pause
    goto MENU
)

REM Source for the pristine backup: firmware\ holds the as-cloned pristine
REM vendor core right after the repo's restructure. We zip from there so
REM that the backup represents the original installed core regardless of
REM whether the user has already deployed.
set "MISSING="
for %%I in (%VENDOR_ITEMS%) do (
    if not exist "%FIRMWARE%\%%I" set "MISSING=!MISSING! %%I"
)
if defined MISSING (
    echo [ABORT] These vendor items are missing under firmware\:!MISSING!
    pause
    goto MENU
)

echo Creating %BACKUP_ZIP% from firmware\ ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%FIRMWARE%\cores','%FIRMWARE%\libraries','%FIRMWARE%\boards.txt','%FIRMWARE%\platform.txt','%FIRMWARE%\keywords.txt','%FIRMWARE%\installed.json' -DestinationPath '%BACKUP_ZIP%' -Force"
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
set /p "OK=Deploy firmware\ to top level? This overwrites the live core. (Y/N): "
if /i not "%OK%"=="Y" goto MENU

call :CLOSE_IDE
call :WIPE_CACHES
call :WIPE_VENDOR

echo Copying firmware\ contents to %REPO% ...
xcopy "%FIRMWARE%\*" "%REPO%\" /E /I /Y /Q >nul
if errorlevel 1 (
    echo [ERROR] xcopy failed.
    call :REOPEN_IDE
    pause
    goto MENU
)
echo [OK] Deploy complete.
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
set /p "OK=Restore pristine core (overwrites top-level vendor files)? (Y/N): "
if /i not "%OK%"=="Y" goto MENU

call :CLOSE_IDE
call :WIPE_CACHES
call :WIPE_VENDOR

echo Expanding %BACKUP_ZIP% to %REPO% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path '%BACKUP_ZIP%' -DestinationPath '%REPO%' -Force"
if errorlevel 1 (
    echo [ERROR] Expand-Archive failed.
    call :REOPEN_IDE
    pause
    goto MENU
)
echo [OK] Restore complete.
call :REOPEN_IDE
pause
goto MENU

REM ============================================================
:CLOSE_IDE
REM ============================================================
set "IDE_WAS_RUNNING=0"
set "IDE_PATH="
tasklist /FI "IMAGENAME eq Arduino IDE.exe" 2>nul | find /I "Arduino IDE.exe" >nul
if errorlevel 1 goto :EOF
set "IDE_WAS_RUNNING=1"

if exist "%LOCALAPPDATA%\Programs\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%LOCALAPPDATA%\Programs\Arduino IDE\Arduino IDE.exe"
) else if exist "%ProgramFiles%\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%ProgramFiles%\Arduino IDE\Arduino IDE.exe"
) else if exist "%ProgramFiles(x86)%\Arduino IDE\Arduino IDE.exe" (
    set "IDE_PATH=%ProgramFiles(x86)%\Arduino IDE\Arduino IDE.exe"
)

echo Arduino IDE is running. Closing ...
taskkill /IM "Arduino IDE.exe" /F >nul 2>&1
REM Brief settle so file handles release
ping -n 3 127.0.0.1 >nul
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
echo Removing existing top-level vendor files ...
for %%I in (%VENDOR_ITEMS%) do (
    if exist "%REPO%\%%I\" (
        rmdir /S /Q "%REPO%\%%I"
    ) else if exist "%REPO%\%%I" (
        del /F /Q "%REPO%\%%I"
    )
)
goto :EOF

REM ============================================================
:REOPEN_IDE
REM ============================================================
if "%IDE_WAS_RUNNING%"=="1" (
    if defined IDE_PATH (
        echo Relaunching Arduino IDE ...
        start "" "%IDE_PATH%"
    ) else (
        echo [WARN] Arduino IDE was running but its install path was not found.
        echo        Please reopen the IDE manually.
    )
)
goto :EOF

:END
endlocal
exit /b 0
