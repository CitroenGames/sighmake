@echo off
cd /d "%~dp0"

sighmake.exe "sighmake.buildscript" -g makefile

IF ERRORLEVEL 1 (
    echo.
    echo Generation failed with an error.
    pause
) else (
    echo.
    echo Makefiles generated successfully.
)
