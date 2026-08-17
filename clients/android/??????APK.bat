@echo off
setlocal

cd /d "%~dp0"

echo.
echo ========================================
echo  Guardian Terminal - Build Android APK
echo ========================================
echo.
echo Cleaning old build cache and building Debug APK...
echo.

call gradlew.bat --no-daemon --max-workers=1 clean :app:assembleDebug
if errorlevel 1 goto build_failed

set "APK_PATH=%CD%\app\build\outputs\apk\debug\app-debug.apk"
set "OUT_PATH=%CD%\guardian-debug.apk"

if not exist "%APK_PATH%" goto apk_missing

copy /Y "%APK_PATH%" "%OUT_PATH%" >nul

echo.
echo ========================================
echo  BUILD SUCCESS
echo ========================================
echo.
echo Original APK:
echo %APK_PATH%
echo.
echo Copied APK:
echo %OUT_PATH%
echo.
echo Send guardian-debug.apk to your Android phone and install it.
echo.
pause
exit /b 0

:apk_missing
echo.
echo Build finished, but APK was not found:
echo %APK_PATH%
echo.
pause
exit /b 2

:build_failed
echo.
echo ========================================
echo  BUILD FAILED
echo ========================================
echo.
echo Send the error messages above to Codex for fixing.
echo.
pause
exit /b 1
