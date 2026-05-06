@echo off
setlocal enabledelayedexpansion

:: Tag the current HEAD as a sighmake release and push the tag, which triggers
:: CI to build all platforms and publish the GitHub release.
::
:: Usage:
::   release.bat 0.3.0
::   release.bat v0.3.0
::   release.bat 0.3.0 --yes      (skip confirmation prompt)

cd /d "%~dp0"

if "%~1"=="" goto :usage
if /i "%~1"=="-h"     goto :usage
if /i "%~1"=="/?"     goto :usage
if /i "%~1"=="--help" goto :usage

set "ARG=%~1"
set "ASSUME_YES=0"
if /i "%~2"=="--yes" set "ASSUME_YES=1"
if /i "%~2"=="-y"    set "ASSUME_YES=1"

:: Strip optional leading "v"
set "VERSION=%ARG%"
if /i "!VERSION:~0,1!"=="v" set "VERSION=!VERSION:~1!"
set "TAG=v!VERSION!"

:: Validate version: digits + dots, optional -suffix (e.g. 0.3.0 or 1.2.3-rc1)
echo !VERSION!| findstr /r /x "[0-9][0-9.]*[0-9a-zA-Z.\-]*" >nul
if errorlevel 1 (
    echo Error: "!VERSION!" does not look like a version ^(expected e.g. 0.3.0^).
    exit /b 1
)

:: Must be in a git repo
git rev-parse --git-dir >nul 2>&1
if errorlevel 1 (
    echo Error: not inside a git repository.
    exit /b 1
)

:: Working tree clean?
for /f "delims=" %%s in ('git status --porcelain') do (
    echo Error: working tree has uncommitted changes. Commit or stash first.
    git status --short
    exit /b 1
)

:: Tag must not already exist locally
git rev-parse -q --verify "refs/tags/!TAG!" >nul 2>&1
if not errorlevel 1 (
    echo Error: tag !TAG! already exists locally. Delete it first if you really mean it:
    echo     git tag -d !TAG!
    exit /b 1
)

:: Tag must not already exist on origin
git ls-remote --exit-code --tags origin "refs/tags/!TAG!" >nul 2>&1
if not errorlevel 1 (
    echo Error: tag !TAG! already exists on origin.
    exit /b 1
)

:: Show what'll happen
for /f "delims=" %%s in ('git rev-parse --short HEAD') do set "SHORT=%%s"
for /f "delims=" %%s in ('git rev-parse --abbrev-ref HEAD') do set "BRANCH=%%s"

echo.
echo About to create release !TAG!
echo   commit:  !SHORT!
echo   branch:  !BRANCH!
echo   pushing: origin !TAG!
echo.

if "!BRANCH!" neq "master" (
    echo Warning: you are not on master ^(currently on !BRANCH!^).
    echo.
)

if "!ASSUME_YES!"=="0" (
    set /p CONFIRM="Proceed? [y/N] "
    if /i "!CONFIRM!" neq "y" (
        echo Aborted.
        exit /b 1
    )
)

echo.
echo Creating tag !TAG!...
git tag -a "!TAG!" -m "Release !TAG!"
if errorlevel 1 (
    echo Error: git tag failed.
    exit /b 1
)

echo Pushing tag to origin...
git push origin "!TAG!"
if errorlevel 1 (
    echo Error: git push failed. Cleaning up local tag.
    git tag -d "!TAG!" >nul 2>&1
    exit /b 1
)

echo.
echo Done. CI will now build and publish the !TAG! release.
echo Watch progress: https://github.com/CitroenGames/sighmake/actions
exit /b 0

:usage
echo Usage: release.bat ^<version^> [--yes]
echo.
echo   ^<version^>   Semver-style version, with or without leading "v" ^(e.g. 0.3.0 or v0.3.0^).
echo   --yes, -y   Skip the confirmation prompt.
echo.
echo Tags HEAD as v^<version^> and pushes the tag, triggering CI to publish a GitHub release.
exit /b 1
