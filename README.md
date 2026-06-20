# sighmake

sighmake is a build system generator for C and C++ projects. It reads concise
`.buildscript` files, basic CMake projects, and supported conversion inputs, then
generates project files for Visual Studio, CMake, or Make.

The goal is to keep build configuration readable, portable, and easy to review in
version control.

## Features

- Human-readable INI-style buildscript format
- Visual Studio project and solution generation (`.vcxproj`, `.sln`, `.slnx`)
- Makefile and CMake generation
- Direct build command with `sighmake --build`
- C and C++ language support
- Debug and Release defaults when no explicit configs are provided
- Per-config, per-platform, and per-file settings
- Project dependencies with `PUBLIC`, `PRIVATE`, and `INTERFACE` visibility
- Wildcard source matching and platform-specific file filters
- Visual Studio solution folders and project filters
- `find_package()` support for common SDKs such as Vulkan, SDL, DirectX, and OpenGL
- Visual Studio solution conversion back to buildscripts
- Valve VPC conversion entry point
- Release updater with `sighmake update`

## Quick Install

Download the latest release archive for your platform:

```text
https://github.com/CitroenGames/sighmake/releases/latest
```

Linux and macOS can install the prebuilt binary directly into `/usr/local/bin`:

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

Windows users can either download `sighmake.exe` from GitHub Releases or build
and install from a Developer Command Prompt:

```batch
install.bat
```

Verify the install:

```bash
sighmake --version
```

For source builds, custom install locations, tests, and release packaging, see
[docs/BUILD.md](docs/BUILD.md).

## Quick Start

Create `myapp.buildscript`:

```ini
[solution]
name = MyApp

[project:MyApp]
type = exe
sources = src/*.cpp
headers = include/*.h
includes = include
std = 17
subsystem = Console
```

Generate project files:

```bash
sighmake myapp.buildscript
```

Build the generated project:

```bash
sighmake --build . --config Release --parallel 8
```

On Windows, the default generator writes Visual Studio files under `build/`.
On Linux and macOS, the default generator writes Makefiles under `build/`.

## Command Summary

```text
sighmake <input-file> [options]
sighmake --build <dir> [build-options]
sighmake --convert <solution.sln|solution.slnx> [options]
sighmake convert vpc <file.vpc> [options]
```

Common generation options:

```text
-g, --generator <type>     Generator type: vcxproj, cmake, makefile, buildscript
-B, --build-dir <dir>      Visual Studio output subdirectory, default: build
-D <NAME>=<VALUE>          Define a variable for ${NAME} substitution
-t, --toolset <name>       Default toolset, for example msvc2022 or msvc2019
    --export-deps          Export a dependency report as HTML
```

Common build options:

```text
--config <cfg>             Build configuration, for example Debug or Release
--target <tgt>             Build a target
--project <name|file>      Build one generated project
--no-project-references    Do not build referenced projects with --project
--clean                    Clean without building
--clean-first              Clean before building
-j, --parallel <N>         Parallel build jobs
```

Useful info commands:

```bash
sighmake --help
sighmake --list
sighmake --list-toolsets
sighmake update --check-only
```

Set a default Visual Studio toolset with:

```batch
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

```bash
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

## Buildscript Example

```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = x64, Linux

[project:MathLib]
type = lib
sources = mathlib/*.cpp
headers = mathlib/*.h
public_includes = mathlib
std = 17

[project:Calculator]
type = exe
sources = calculator/*.cpp
includes = calculator
subsystem = Console

target_link_libraries(
    MathLib PUBLIC
)
```

Buildscripts support:

- `sources`, `headers`, `resources`, `masm`, `nasm`, `idl`, and `mc` inputs
- Per-config settings such as `optimization[Release] = MaxSpeed`
- Per-platform settings such as `defines[x64] = WIN64`
- Conditional blocks such as `if(Windows) { ... }`
- Per-file settings such as `pch.cpp:pch = Create`
- Shared settings with `include = common_settings.buildscript`
- Solution folders with `folder("Tools") { ... }`

The full syntax reference is in [usage.md](usage.md).

## Documentation

- [Build and installation guide](docs/BUILD.md)
- [Full usage guide](usage.md)
- [VS Code extension](editors/vscode/README.md)
- [Multi-project example](examples/Multi%20Project%20Example)

## License

This project uses PugiXML, which is licensed under the MIT License.
