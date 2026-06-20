# Build and Installation Guide

This document covers installing sighmake, building it from source, running the
test project, and publishing releases.

## Prerequisites

Windows:

- Visual Studio with the "Desktop development with C++" workload
- `cl.exe` and MSBuild, usually through a Developer Command Prompt

Linux:

- `g++`, `make`, `curl`, and standard build tools
- On Debian or Ubuntu, `build-essential` is sufficient for the compiler and Make

macOS:

- Xcode Command Line Tools
- `clang++` and `make`

## Install a Release Build

Download release archives from:

```text
https://github.com/CitroenGames/sighmake/releases/latest
```

Linux and macOS one-line installs:

```bash
# Linux x64
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-linux-x64.tar.gz | sudo tar -xz -C /usr/local/bin

# Linux arm64
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-linux-arm64.tar.gz | sudo tar -xz -C /usr/local/bin

# macOS Apple Silicon
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-macos-arm64.tar.gz | sudo tar -xz -C /usr/local/bin

# macOS Intel
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-macos-x64.tar.gz | sudo tar -xz -C /usr/local/bin
```

Check the installed binary:

```bash
sighmake --version
sighmake --help
```

Installed release builds can update themselves:

```bash
sighmake update --check-only
sighmake update
```

## Install from Source

### Windows

Run from a Developer Command Prompt:

```batch
install.bat
```

The installer compiles `src/**/*.cpp`, installs `sighmake.exe` to
`%LOCALAPPDATA%\sighmake`, and adds that directory to the user `PATH`.

Set a custom install directory with:

```batch
set SIGHMAKE_INSTALL_DIR=C:\Tools\sighmake
install.bat
```

If `install.bat` is started outside a Developer Command Prompt, it tries to find
Visual Studio with `vswhere.exe` and relaunch itself inside `VsDevCmd.bat`.

### Linux and macOS

```bash
./install.sh
```

The default install prefix is `/usr/local`, so the installed binary is normally:

```text
/usr/local/bin/sighmake
```

Set a custom prefix with:

```bash
PREFIX="$HOME/.local" ./install.sh
```

The Unix installer compiles a release binary directly from `src/**/*.cpp`.
Set `CXX`, `CXXFLAGS`, or `JOBS` to override the compiler, flags, or parallelism:

```bash
CXX=clang++ JOBS=8 PREFIX="$HOME/.local" ./install.sh
```

## Build the Repository with sighmake

Once a `sighmake` binary is available, the repository can generate and build its
own project files from `sighmake.buildscript`.

### Windows

Generate Visual Studio files:

```batch
sighmake sighmake.buildscript
```

The Visual Studio generator writes generated files under `build/` by default.
Depending on the detected Visual Studio version, open one of:

```text
build\sighmake_.sln
build\sighmake_.slnx
```

Or build directly:

```batch
sighmake --build . --config Debug --parallel 8
sighmake --build . --config Release --parallel 8
```

The helper script does the generation step:

```batch
GenerateSolution.bat
```

Use `-B` when you want the generated Visual Studio files in a different
subdirectory:

```batch
sighmake sighmake.buildscript -B vsbuild
```

### Linux

If a local `./sighmake` binary exists, generate Makefiles with:

```bash
./generate_linux.sh
```

Then build with either sighmake or Make:

```bash
./sighmake --build . --config Release -j 8
make -C build Release -j 8
```

If there is no local `./sighmake` binary yet, use `./install.sh` first or install
a release binary from GitHub Releases.

### macOS

The macOS helper bootstraps `sighmake_macos` with `clang++` when needed, then
generates Makefiles:

```bash
./generate_macos.sh
```

Build with:

```bash
./sighmake_macos --build . --config Release -j 8
make -C build Release -j 8
```

## Build Other Generators

Generate with an explicit generator:

```bash
sighmake sighmake.buildscript -g vcxproj
sighmake sighmake.buildscript -g makefile
sighmake sighmake.buildscript -g cmake
```

Convert supported input formats:

```bash
sighmake CMakeLists.txt -g buildscript
sighmake --convert MySolution.slnx
sighmake convert vpc project.vpc
```

## Run Tests

The test project is defined in `tests/tests.buildscript` and is currently
configured for Visual Studio platforms.

From the repository root on Windows:

```batch
cd tests
sighmake tests.buildscript
sighmake --build . --config Debug --parallel 8
build\bin\x64\Debug\sighmake_tests.exe
cd ..
```

Run the Release test binary with:

```batch
cd tests
sighmake --build . --config Release --parallel 8
build\bin\x64\Release\sighmake_tests.exe
cd ..
```

## Release a Version

Release scripts tag the current `HEAD` and push the tag to `origin`. The tag
triggers CI to build platform archives and publish the GitHub release.

The working tree must be clean before running either script.

Windows:

```batch
release.bat 0.3.0
release.bat 0.3.0 --yes
```

Linux or macOS:

```bash
./release.sh 0.3.0
./release.sh 0.3.0 --yes
```

Both scripts accept versions with or without a leading `v`.

## Troubleshooting

`cl.exe` not found:

- Start a Developer Command Prompt, or let `install.bat` locate Visual Studio
  with `vswhere.exe`.

Generated Visual Studio files are missing:

- Run `sighmake sighmake.buildscript` again.
- Check the `-B` output directory if you used one.

`sighmake --build` cannot find a solution or Makefile:

- Regenerate first. `--build` uses the build cache written by the generation
  step.

Makefile has no targets:

- The Makefile generator skips Windows-only platforms. Add a non-Windows
  platform such as `Linux` to the buildscript when generating Makefiles.

File locks on Windows:

- Close Visual Studio or other tools that may be holding generated project files
  or binaries open, then regenerate or rebuild.
