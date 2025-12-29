@echo off
cd /d "%~dp0"

sighmake.exe "sighmake.buildscript"

IF ERRORLEVEL 1 (
    echo.
    echo Build failed with an error.
    pause
)
