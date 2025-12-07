@echo off
REM Build script for sighmake

setlocal enabledelayedexpansion

echo ========================================
echo sighmake Build Script
echo ========================================
echo.

REM Parse command line arguments
set BUILD_TYPE=Release
set CLEAN=0
set GENERATOR=

:parse_args
if "%~1"=="" goto end_parse_args
if /I "%~1"=="debug" (
    set BUILD_TYPE=Debug
    shift
    goto parse_args
)
if /I "%~1"=="release" (
    set BUILD_TYPE=Release
    shift
    goto parse_args
)
if /I "%~1"=="clean" (
    set CLEAN=1
    shift
    goto parse_args
)
if /I "%~1"=="-G" (
    set GENERATOR=%~2
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--help" (
    goto show_help
)
if /I "%~1"=="-h" (
    goto show_help
)
echo Unknown argument: %~1
goto show_help

:end_parse_args

REM Clean if requested
if %CLEAN%==1 (
    echo Cleaning build directory...
    if exist build (
        rmdir /s /q build
        echo Build directory removed.
    ) else (
        echo Build directory does not exist.
    )
    echo.
)

REM Create build directory
if not exist build (
    echo Creating build directory...
    mkdir build
)

REM Create project

sighmake.exe sighmake.buildscript -b build

if errorlevel 1 (
    echo.
    echo ERROR: Build failed!
    cd ..
    exit /b 1
)

cd build

REM todo auto build project here

echo.
echo ========================================
echo Build completed successfully!
echo ========================================
echo   Build Type: %BUILD_TYPE%
if exist build\bin\%BUILD_TYPE%\sighmake.exe (
    echo   Executable: build\bin\%BUILD_TYPE%\sighmake.exe
) else if exist build\bin\sighmake.exe (
    echo   Executable: build\bin\sighmake.exe
) else (
    echo   Executable: build\%BUILD_TYPE%\sighmake.exe
)
echo.
echo To run sighmake:
echo   build\bin\sighmake.exe [buildscript] [options]
echo.

exit /b 0

:show_help
echo Usage: build.bat [options]
echo.
echo Options:
echo   debug       Build in Debug mode
echo   release     Build in Release mode (default)
echo   clean       Clean build directory before building
echo   -G [gen]    Specify CMake generator
echo   -h, --help  Show this help message
echo.
echo Examples:
echo   build.bat                    Build in Release mode
echo   build.bat debug              Build in Debug mode
echo   build.bat clean release      Clean and build in Release mode
echo   build.bat -G "Visual Studio 17 2022"
echo.
exit /b 0
