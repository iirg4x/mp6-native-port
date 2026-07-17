@echo off
setlocal
rem mp6-native local setup tool -- Windows launcher. See setup/README.md.
set "SCRIPT_DIR=%~dp0"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
    py -3 "%SCRIPT_DIR%setup\setup.py" %*
    exit /b %ERRORLEVEL%
)

where python >nul 2>nul
if %ERRORLEVEL%==0 (
    python "%SCRIPT_DIR%setup\setup.py" %*
    exit /b %ERRORLEVEL%
)

echo.
echo Python 3 was not found on PATH.
echo Install it from https://www.python.org/downloads/ (or the Microsoft
echo Store "Python 3" package), then re-run this script.
echo.
pause
exit /b 1
