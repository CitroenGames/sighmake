@echo off

:: If cl.exe is already available, skip straight to the main installer
where cl.exe >nul 2>&1
if %errorlevel% equ 0 goto :main

:: Not in a Developer Command Prompt — find one and relaunch
echo cl.exe not found on PATH. Searching for Visual Studio installation...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: vswhere.exe not found. Please install Visual Studio with the
    echo "Desktop development with C++" workload.
    exit /b 1
)

set "VSDEVCMD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if not defined VSDEVCMD set "VSDEVCMD=%%i\Common7\Tools\VsDevCmd.bat"
)

if not defined VSDEVCMD (
    echo Error: No Visual Studio installation with C++ tools found.
    echo Please install Visual Studio with the "Desktop development with C++" workload.
    exit /b 1
)

if not exist "%VSDEVCMD%" (
    echo Error: VsDevCmd.bat not found at "%VSDEVCMD%"
    exit /b 1
)

echo Found: "%VSDEVCMD%"
echo Relaunching inside Developer Command Prompt...
echo.

:: Re-invoke this script inside the developer environment
cmd /s /c ""%VSDEVCMD%" -arch=amd64 -no_logo && "%~f0" %*"
exit /b %errorlevel%

:main
setlocal enabledelayedexpansion

cd /d "%~dp0"

echo === sighmake installer ===
echo.

:: Determine install directory
if "%SIGHMAKE_INSTALL_DIR%"=="" (
    set "INSTALL_DIR=%LOCALAPPDATA%\sighmake"
) else (
    set "INSTALL_DIR=%SIGHMAKE_INSTALL_DIR%"
)

echo Install dir:     %INSTALL_DIR%
echo.

:: Step 1: Compile
echo [1/3] Compiling...

set "SOURCES="
for /r src %%f in (*.cpp) do (
    set "SOURCES=!SOURCES! %%f"
)

cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /MP /I"%~dp0src" /FI "%~dp0src\pch.h" /Fe:sighmake.exe !SOURCES! /link /OUT:sighmake.exe advapi32.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo       Compilation failed. Trying with verbose output...
    cl.exe /std:c++17 /O2 /EHsc /W3 /MP /I"%~dp0src" /FI "%~dp0src\pch.h" /Fe:sighmake.exe !SOURCES! /link /OUT:sighmake.exe advapi32.lib
    exit /b 1
)
echo       OK
echo.

:: Step 2: Install
echo [2/3] Installing to %INSTALL_DIR%...

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /y sighmake.exe "%INSTALL_DIR%\sighmake.exe" >nul
if %errorlevel% neq 0 (
    echo       Failed to copy binary.
    exit /b 1
)
del /q sighmake.exe 2>nul
del /q *.obj 2>nul
echo       OK
echo.

:: Step 3: Add to PATH
echo [3/3] Updating PATH...

powershell -NoProfile -Command "try{$d='%INSTALL_DIR%';$p=[Environment]::GetEnvironmentVariable('Path','User');if($p -and ($p.Split(';')-contains $d)){exit 0}if($p){$n=$p+';'+$d}else{$n=$d};[Environment]::SetEnvironmentVariable('Path',$n,'User');exit 1}catch{Write-Error $_;exit 2}"
if !errorlevel! equ 1 (
    echo       Added %INSTALL_DIR% to user PATH.
) else if !errorlevel! equ 2 (
    echo       Warning: Failed to add to PATH. Add %INSTALL_DIR% to your PATH manually.
) else (
    echo       Already in PATH.
)

echo.
echo Done. Open a new terminal and run 'sighmake --help' to get started.

endlocal
