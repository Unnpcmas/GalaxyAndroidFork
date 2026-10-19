@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo Building GalaxyInstaller APK
echo ==============================================
echo.

set "PYTHON_EXE="
where py >nul 2>nul
if not errorlevel 1 set "PYTHON_EXE=py"
if not defined PYTHON_EXE (
    where python >nul 2>nul
    if not errorlevel 1 set "PYTHON_EXE=python"
)
if not defined PYTHON_EXE (
    echo [ERROR] Python 3 was not found. Install Python and make sure "py" or "python" is on PATH.
    if "%1" neq "--no-pause" pause
    exit /b 1
)

"%PYTHON_EXE%" installer-mobile\build.py

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed!
    echo.
    if "%1" neq "--no-pause" pause
    exit /b 1
)

echo.
echo ==============================================
echo [SUCCESS] out-mobile\GalaxyInstaller.apk created!
echo ==============================================
if "%1" neq "--no-pause" pause
