# sighmake

sighmake is a build system generator for C, C++, and C# projects. It reads concise
`.buildscript` files, basic CMake projects, and supported conversion inputs, then
generates project files for Visual Studio, CMake, or Make. C# support currently
targets Visual Studio with SDK-style .NET projects.

The goal is to keep build configuration readable, portable, and easy to review in
version control.

## Features

- Human-readable INI-style buildscript format
- Visual Studio project and solution generation (`.vcxproj`, `.csproj`, `.sln`, `.slnx`)
- Makefile and CMake generation
- Direct build command with `sighmake --build`
- C and C++ language support, plus C# executables and libraries in Visual Studio
- Mixed native/managed solutions, managed assembly references, and automatic .NET restore
- Debug and Release defaults when no explicit configs are provided
- Per-config, per-platform, and per-file settings
- Project dependencies with `PUBLIC`, `PRIVATE`, and `INTERFACE` visibility
- Wildcard source matching and platform-specific file filters
- Android cross-compilation with the NDK (`platforms = Android`)
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

### Linux x64
```bash
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-linux-x64.tar.gz | sudo tar -xz -C /usr/local/bin
```
### Linux arm64
```bash
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-linux-arm64.tar.gz | sudo tar -xz -C /usr/local/bin
```
### macOS (Apple Silicon)
```bash
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-macos-arm64.tar.gz | sudo tar -xz -C /usr/local/bin
```
### macOS (Intel)
```bash
curl -fsSL https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-macos-x64.tar.gz | sudo tar -xz -C /usr/local/bin
```

### Windows x64
```powershell
$installer = Join-Path $env:TEMP 'sighmake-windows-x64-setup.exe'; Invoke-WebRequest 'https://github.com/CitroenGames/sighmake/releases/latest/download/sighmake-windows-x64-setup.exe' -OutFile $installer; Start-Process $installer -Wait
```

Alternatively, build and install from a Developer Command Prompt:

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

### C# Quick Start

Install Visual Studio and a .NET SDK supporting your chosen target framework.
For example, save this as `managed.buildscript`:

```ini
[solution]
name = ManagedApp
platforms = x64

[project:ManagedApp]
language = C#
type = exe
target_framework = net10.0
sources = src/**/*.cs
nullable = enable
implicit_usings = enable
```

Generate and build from the directory containing the buildscript:

```powershell
sighmake managed.buildscript -g vcxproj
sighmake --build . --config Release --platform x64
```

Use `type = dll` for managed libraries and `target_link_libraries(PRIVATE Contracts)`
to reference another managed project. `dependencies = Contracts` adds build ordering
only. `--project ManagedApp` selects a single project and builds its references.

See the [mixed C++/C# example](examples/CSharp/managed.buildscript) and
[C# settings reference](usage.md#c-projects) for unsafe code, runtime configuration
files, and other options. C# generation for Makefile/CMake, importing `.csproj`
files, and NuGet package declarations are not yet supported.

## Command Summary

```text
sighmake <input-file> [options]
sighmake --build <dir> [build-options]
sighmake --convert <file.sln|.slnx|.vcxproj|.vcproj> [options]
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
--platform <name>          Visual Studio platform, for example x64 or Win32 (x86 aliases Win32)
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
- [Mixed C++/C# example](examples/CSharp/managed.buildscript)

## License

This project uses PugiXML, which is licensed under the MIT License.
