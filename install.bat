@echo off
setlocal enabledelayedexpansion

:: Check for administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo This installer requires administrator privileges.
    echo Right-click and select "Run as administrator", or run from an elevated command prompt.
    exit /b 1
)

cd /d "%~dp0"

echo === sighmake installer ===
echo.

:: Check for cl.exe (MSVC compiler)
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: cl.exe not found. Please run this from a Visual Studio
    echo Developer Command Prompt, or run:
    echo.
    echo   "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    echo.
    echo Then re-run this script.
    exit /b 1
)

:: Determine install directory
if "%SIGHMAKE_INSTALL_DIR%"=="" (
    set "INSTALL_DIR=%ProgramFiles%\sighmake"
) else (
    set "INSTALL_DIR=%SIGHMAKE_INSTALL_DIR%"
)

echo Platform:        Windows
echo Compiler:        MSVC ^(cl.exe^)
echo Install dir:     %INSTALL_DIR%
echo.

:: Step 1: Bootstrap — compile sighmake from source
echo [1/4] Compiling sighmake from source...

:: Collect all .cpp files
set "SOURCES="
for /r src %%f in (*.cpp) do (
    set "SOURCES=!SOURCES! %%f"
)

cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /Isrc\ /FI src\pch.h /Fe:sighmake_bootstrap.exe !SOURCES! /link /OUT:sighmake_bootstrap.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo       Compilation failed. Trying with verbose output...
    cl.exe /std:c++17 /O2 /EHsc /W3 /Isrc\ /FI src\pch.h /Fe:sighmake_bootstrap.exe !SOURCES! /link /OUT:sighmake_bootstrap.exe
    exit /b 1
)
echo       Bootstrap compilation successful.
echo.

:: Step 2: Generate Visual Studio solution
echo [2/4] Generating Visual Studio solution...
sighmake_bootstrap.exe sighmake.buildscript
if %errorlevel% neq 0 (
    echo       Generation failed.
    del /q sighmake_bootstrap.exe 2>nul
    exit /b 1
)
del /q sighmake_bootstrap.exe 2>nul
del /q *.obj 2>nul
echo.

:: Step 3: Build Release
echo [3/4] Building Release...

:: Find MSBuild
set "MSBUILD="
for /f "usebackq delims=" %%i in (`where msbuild.exe 2^>nul`) do (
    if "!MSBUILD!"=="" set "MSBUILD=%%i"
)

if "!MSBUILD!"=="" (
    :: Try vswhere
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        if "!MSBUILD!"=="" set "MSBUILD=%%i"
    )
)

if "!MSBUILD!"=="" (
    echo       Error: MSBuild not found.
    exit /b 1
)

:: Find the .sln file
set "SLN_FILE="
for %%f in (*.sln) do (
    if "!SLN_FILE!"=="" set "SLN_FILE=%%f"
)

if "!SLN_FILE!"=="" (
    echo       Error: No .sln file found.
    exit /b 1
)

"!MSBUILD!" "!SLN_FILE!" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if %errorlevel% neq 0 (
    echo       Build failed.
    exit /b 1
)
echo.

:: Step 4: Install
echo [4/4] Installing to %INSTALL_DIR%...

:: Find the built binary
set "BUILT_BIN="
for /r build\bin %%f in (sighmake.exe) do (
    if "!BUILT_BIN!"=="" set "BUILT_BIN=%%f"
)

if "!BUILT_BIN!"=="" (
    :: Also check root output dirs
    for /r bin %%f in (sighmake.exe) do (
        if "!BUILT_BIN!"=="" set "BUILT_BIN=%%f"
    )
)

if "!BUILT_BIN!"=="" (
    echo       Error: Built binary not found.
    exit /b 1
)

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /y "!BUILT_BIN!" "%INSTALL_DIR%\sighmake.exe" >nul
if %errorlevel% neq 0 (
    echo       Failed to copy binary.
    exit /b 1
)

:: Add to system PATH if not already there
echo %PATH% | findstr /i /c:"%INSTALL_DIR%" >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo       Adding %INSTALL_DIR% to system PATH...
    for /f "usebackq tokens=2,*" %%A in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PATH 2^>nul`) do set "SYS_PATH=%%B"
    if defined SYS_PATH (
        reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PATH /t REG_EXPAND_SZ /d "!SYS_PATH!;%INSTALL_DIR%" /f >nul 2>&1
    ) else (
        reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PATH /t REG_EXPAND_SZ /d "%INSTALL_DIR%" /f >nul 2>&1
    )
    echo       NOTE: Restart your terminal for PATH changes to take effect.
)

echo.
echo sighmake installed to %INSTALL_DIR%\sighmake.exe
echo Run 'sighmake --help' to get started.

endlocal
