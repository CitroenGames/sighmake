# sighmake

A flexible build system generator that converts buildscript files and CMakeLists.txt into Visual Studio project files (.vcxproj, .sln, .slnx) and Makefiles. Designed for simplicity and cross-platform development.

## Features

- **Simple, readable buildscript format** - Human-friendly INI-style syntax
- **CMake support** - Parse CMakeLists.txt files and generate project files
- **Multiple generators** - Visual Studio projects (.vcxproj/.sln/.slnx) and Makefiles
- **Cross-platform** - Windows (MSVC), Linux (GCC), and macOS (Clang) support
- **C and C++ language support** - First-class support for both C and C++ projects with appropriate standards
- **Build command** - Build generated projects directly with `sighmake --build` (like `cmake --build`)
- **Configuration-specific settings** - Per-config compiler options, optimization, etc.
- **Configuration templates** - Inherit settings from base configurations with `Template:` syntax
- **Conditional compilation blocks** - `if(Windows)`, `if(Linux)`, `if(macOS)` platform conditionals
- **Per-file compiler settings** - Customize compilation flags for individual files
- **Wildcard support** - Use glob patterns for source files (*.cpp, **/*.cpp)
- **Platform-specific source files** - Inline conditions like `file.cpp [windows]`
- **Project dependencies** - Automatic dependency tracking with PUBLIC/PRIVATE/INTERFACE visibility
- **find_package()** - Locate external SDKs (Vulkan, SDL2, SDL3, DirectX, OpenGL)
- **File inclusion** - Share common settings across buildscripts
- **Solution folders** - Organize projects in Visual Studio Solution Explorer
- **Dependency export** - Generate HTML dependency reports with `--export-deps`
- **Assembly support** - MASM and NASM assembler integration
- **Kernel-mode drivers** - Build Windows kernel drivers (sys) and kernel-mode static libraries (sys_lib)
- **Bidirectional conversion** - Convert Visual Studio solutions to buildscripts

## Quick Start

### Windows
```batch
# Generate Visual Studio project from buildscript
sighmake project.buildscript

# Build directly
sighmake --build . --config Release

# Generate from CMakeLists.txt
sighmake CMakeLists.txt

# Specify toolset
sighmake project.buildscript -t msvc2022
```

### Linux
```bash
# Generate Makefiles (default on Linux)
./sighmake project.buildscript

# Build directly
./sighmake --build . --config Release

# Or build manually
make -C build Release
```

### Even Simpler: Auto-Populated Configurations

If you don't define any `[config:...]` sections, sighmake automatically provides Debug and Release configurations with sensible defaults:

```ini
[solution]
name = MyApp

[project:MyApp]
type = exe
sources = src/*.cpp
includes = include
std = 17
```

This minimal buildscript automatically gets optimized Debug and Release configurations.

## Installation

### Prerequisites
- C++17 compatible compiler (MSVC on Windows, GCC/Clang on Linux/macOS)
- Windows: Visual Studio with C++ workload
- Linux: `build-essential` (or equivalent)
- macOS: Xcode Command Line Tools (`xcode-select --install`)

### Quick Install

#### Windows
Run from an **elevated** Developer Command Prompt (right-click, "Run as administrator"):
```batch
install.bat
```
Installs to `C:\Program Files\sighmake` and adds it to the system PATH.

Set `SIGHMAKE_INSTALL_DIR` to customize the install location:
```batch
set SIGHMAKE_INSTALL_DIR=C:\Tools\sighmake
install.bat
```

#### Linux / macOS
```bash
./install.sh
```
Installs to `/usr/local/bin/sighmake` by default. Will prompt for `sudo` if needed.

Set `PREFIX` to customize the install location:
```bash
PREFIX=$HOME/.local ./install.sh
```

### Building from Source (without installing)

#### Windows
```batch
GenerateSolution.bat
:: Open the generated .sln in Visual Studio and build
```

#### Linux
```bash
./generate_linux.sh
make -f build/sighmake.Release
```

#### macOS
```bash
./generate_macos.sh
make -f build/sighmake.Release
```

## Usage

```
sighmake <buildscript|CMakeLists.txt> [options]
sighmake --build <dir> [--config <cfg>] [--clean]
sighmake --convert <solution.sln|solution.slnx> [options]

Options:
  -g, --generator <type>     Generator type (vcxproj, makefile)
  -t, --toolset <name>       Default toolset (msvc2022, msvc2019, etc)
  -b, --build <dir>          Build using previously generated project files
      --config <cfg>         Build configuration (with --build, e.g. Release)
      --target <tgt>         Build specific target (with --build)
      --clean                Clean build artifacts (with --build)
      --clean-first          Clean before building (with --build)
  -j, --parallel <N>         Parallel build jobs (with --build)
  -c, --convert              Convert Visual Studio .sln/.slnx to buildscripts
      --export-deps          Export project dependency report as HTML
      --list-toolsets        List available toolsets
  -l, --list                 List available generators
  -h, --help                 Show help message
```

### Command Examples

**Generate Visual Studio project:**
```batch
sighmake project.buildscript -g vcxproj
```

**Build directly (like cmake --build):**
```bash
sighmake --build . --config Release --parallel 8
```

**Generate with specific toolset:**
```batch
sighmake project.buildscript -t msvc2019
```

**Convert Visual Studio solution to buildscripts:**
```batch
sighmake --convert solution.slnx
```

**Export dependency report:**
```batch
sighmake project.buildscript --export-deps
```

**Generate Makefiles:**
```bash
./sighmake project.buildscript -g makefile
```

**Parse CMake file:**
```batch
sighmake CMakeLists.txt -g vcxproj
```

### Environment Variables

You can set a default toolset using an environment variable:
```batch
# Windows
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022

# Linux
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

## Buildscript Format

Buildscripts use an INI-style syntax with sections for solutions and projects.

### Basic Structure

```ini
[solution]
name = MySolution
configurations = Debug, Release
platforms = Win32, x64

[project:MyProject]
type = exe
sources = src/*.cpp
headers = include/*.h
includes = include, external/include
defines = MY_DEFINE, _CRT_SECURE_NO_WARNINGS
std = 17
```

### Solution Settings

```ini
[solution]
name = MySolution              # Solution name
configurations = Debug, Release # Build configurations
platforms = Win32, x64         # Target platforms
defines = COMMON_DEFINE        # Defines for all projects (supports bracket notation)
```

**Platform filtering by generator:**
- **vcxproj generator**: Only includes Win32, x64, ARM, ARM64 platforms
- **makefile generator**: Only includes non-Windows platforms like Linux, macOS, Darwin

This allows a single buildscript to define both Windows and Unix configurations:
```ini
platforms = x64, Linux
```

### Project Types

| Type | Description | Extension (Win) | Extension (Linux) |
|------|-------------|-----------------|-------------------|
| `exe` | Executable | .exe | (none) |
| `lib` | Static library | .lib | .a |
| `dll` | Dynamic library | .dll | .so |
| `sys` | Kernel-mode driver | .sys | .sys |
| `sys_lib` | Kernel-mode static library | .lib | .a |

### Project Settings

#### Basic Project Properties

```ini
[project:ProjectName]
type = exe                      # Project type: exe, lib, dll, sys, sys_lib
sources = src/*.cpp             # Source files (supports wildcards)
headers = include/*.h           # Header files
includes = include, external    # Include directories
defines = MYDEFINE              # Preprocessor definitions
std = 17                        # C++ standard (14, 17, 20, 23)
```

#### Source Organization

```ini
sources = src/*.cpp             # Source files
headers = include/*.h           # Header files
resources = res/*.rc            # Resource files (.rc)
masm = asm/*.asm                # MASM assembly files
nasm = asm/*.asm                # NASM assembly files
mc = src/events.mc              # Message Compiler files
idl = src/MyInterface.idl       # IDL/MIDL files
```

#### C Language Projects

Sighmake supports both C and C++ projects with appropriate compiler settings:

```ini
[project:GLAD]
type = lib
language = C                   # Declare project as C
c_standard = 11                # C standard: 89, 11 (99/17/23 fall back with warning on MSVC)
sources = src/glad.c
headers = include/**/*.h
public_includes = include
```

**Auto-detection:**
- Projects with only `.c` files are detected as C projects
- Projects with `.cpp` files are detected as C++ projects
- Explicitly set `language = C` or `language = C++` to override

#### Dependencies and Libraries

Dependencies support CMake-style visibility (PUBLIC, PRIVATE, INTERFACE):

```ini
# Modern dependency syntax with visibility
target_link_libraries(
    SDL3 INTERFACE          # Headers propagate to dependents, not added to this target
    entt PUBLIC             # Headers + libs added to this target AND propagate to dependents
    GLAD PRIVATE            # Headers + libs added to this target, but NOT to dependents
)

# Legacy syntax (defaults to PUBLIC)
depends = OtherProject      # Project dependencies
libs = user32.lib, gdi32.lib   # Library dependencies
libdirs = lib, external/lib    # Library search directories
```

#### find_package()

Locate external SDKs and libraries on your system:

```ini
find_package(Vulkan REQUIRED)
find_package(SDL2)

includes = include, ${Vulkan_INCLUDE_DIRS}
libs = ${Vulkan_LIBRARIES}
```

Supported packages: `Vulkan`, `OpenGL`, `SDL2`, `SDL3`, `DirectX9`, `DirectX10`, `DirectX11`, `DirectX12`

Packages can also propagate automatically via `target_link_libraries()`:
```ini
find_package(Vulkan REQUIRED)
target_link_libraries(Vulkan)  # Includes, libs, libdirs applied automatically
```

#### Compiler Settings

```ini
warning_level = Level3         # Level0, Level1, Level2, Level3, Level4
multiprocessor = true          # Enable multi-processor compilation
utf8 = true                    # UTF-8 source encoding (required by spdlog, fmt)
exception_handling = Sync      # Sync, Async, or false
rtti = true                    # Runtime type information
optimization = MaxSpeed        # Disabled, MinSize, MaxSpeed, Full
runtime_library = MultiThreaded # MultiThreaded, MultiThreadedDebug, etc.
debug_info = ProgramDatabase   # None, ProgramDatabase, EditAndContinue
floating_point = Fast          # Precise, Fast, Strict
whole_program_optimization = true  # /GL + /LTCG
compile_as = CompileAsC        # Force compilation language
cflags = /wd4668               # Additional compiler flags
```

#### Linker Settings

```ini
subsystem = Console            # Console, Windows, Native
generate_debug_info = true     # Generate debug information
link_incremental = false       # Incremental linking
module_def = exports.def       # Module definition file for DLL exports
base_address = 0x10000000      # Preferred DLL load base address
entry_point = DriverEntry      # Entry point symbol
ignore_all_default_libraries = true  # /NODEFAULTLIB
ldflags = /MANIFEST            # Additional linker flags
```

#### Output Settings

```ini
outdir = bin/Release           # Output directory
intdir = obj/Release           # Intermediate directory
target_name = MyApp            # Output file name
target_ext = .exe              # Output file extension
```

### Configuration-Specific Settings

Use `[Config|Platform]` syntax to specify settings per configuration:

```ini
[project:MyProject]
# Per-configuration optimization
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed

# Per-platform defines
defines[Win32] = WIN32, _WINDOWS
defines[x64] = WIN64, _WIN64

# Per-configuration AND platform
runtime_library[Debug|Win32] = MultiThreadedDebug
runtime_library[Release|x64] = MultiThreaded

# Wildcard: applies to ALL configurations and platforms
pch[*] = Use
```

### Configuration Templates

Create new configurations that inherit all settings from a base:

```ini
[config:Release]
optimization = MaxSpeed
runtime_library = MultiThreaded
defines = NDEBUG

# Test inherits from Release, overrides defines
[config:Test] : Template:Release
defines = NDEBUG, ENABLE_TESTING
```

### Conditional Compilation Blocks

Use `if(Platform)` syntax for platform-specific settings:

```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp

if(Windows) {
    subsystem = Windows
    libs = user32.lib, gdi32.lib
}

if(Linux) {
    libs = pthread, dl
}

if(macOS) {
    libs = -framework Metal, -framework Cocoa
}
```

Supported conditions: `if(Windows)`, `if(Linux)`, `if(macOS)`, `if(unix)`, `if(Win32)`, `if(x64)`, `if(!Windows)`, etc.

### Platform-Specific Source Files

Conditionally include source files with inline conditions:

```ini
sources = {
    src/**/*.cpp
    src/platform/win_impl.cpp [windows]
    src/platform/linux_impl.cpp [linux]
    src/platform/mac_impl.mm [osx]
    src/platform/posix_impl.cpp [!windows]
}
```

### Wildcard Patterns

Sighmake supports glob patterns for file matching:

```ini
sources = src/*.cpp            # All .cpp files in src/
sources = src/**/*.cpp         # All .cpp files in src/ and subdirectories
headers = include/**/*.h       # All .h files recursively
```

### Per-File Settings

Apply settings to specific files:

```ini
# Precompiled header
pch.cpp:pch = Create
pch.cpp:pch_header = pch.h

# Per-file defines
utils.cpp:defines = EXTRA_DEFINE

# Per-file optimization
slow_file.cpp:optimization = Disabled

# Exclude file from build
debug_helpers.cpp:excluded[Release] = true
```

### File Inclusion

Share common settings across multiple buildscripts:

```ini
[project:MyProject]
type = exe
sources = src/*.cpp

# Include common settings from another file
include = common_settings.buildscript
```

### Solution Folders

Organize projects in Visual Studio Solution Explorer:

```ini
folder("Engine") {
    include = engine/core.buildscript
    include = engine/renderer.buildscript
}

folder("Tools") {
    include = tools/editor.buildscript
}
```

### Custom Build Rules

For files that need custom build commands:

```ini
custom_build(src/schema.xsd,
    command = xsd.exe /c /language:CS %(FullPath)
    outputs = %(Filename).cs
    description = Generating code from %(Filename)
)
```

### Assembly Support

**MASM (x64):**
```ini
masm[x64] = getstackptr64.masm
```

**NASM:**
```ini
nasm = boot/boot.asm
nasm_format = bin
nasm_flags = -w+all
nasm_includes = boot/include
```

### Multi-Project Example

```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = Win32, x64

# Shared library project
[project:MathLib]
type = dll
sources = mathlib/*.cpp
headers = mathlib/*.h
includes = mathlib
defines = MATHLIB_EXPORTS
std = 17

# Executable that depends on the library
[project:Calculator]
type = exe
sources = calculator/*.cpp
includes = mathlib
depends = MathLib              # Build MathLib first
libs = MathLib.lib
libdirs = output/Release       # Where to find MathLib.lib
subsystem = Console
```

### Conditional File Inclusion

Include files based on configuration:

```ini
[project:MyProject]
include[Debug|x64] = debug_x64_settings.buildscript
include[Release|x64] = release_x64_settings.buildscript
```

## CMake Support

Sighmake can parse basic CMakeLists.txt files and generate project files:

```bash
# Generate Visual Studio project from CMake
sighmake CMakeLists.txt -g vcxproj

# Generate Makefiles from CMake
sighmake CMakeLists.txt -g makefile
```

**Note:** CMake support is experimental and covers basic project configurations. Complex CMake features may not be fully supported.

## Toolsets

Sighmake supports multiple Visual Studio toolsets:

| Toolset   | Description           |
|-----------|-----------------------|
| msvc2026  | Visual Studio 2026    |
| msvc2022  | Visual Studio 2022 (default) |
| msvc2019  | Visual Studio 2019    |
| msvc2017  | Visual Studio 2017    |
| msvc2015  | Visual Studio 2015    |
| msvc2013  | Visual Studio 2013    |
| msvc2012  | Visual Studio 2012    |
| msvc2010  | Visual Studio 2010    |

**List available toolsets:**
```bash
sighmake --list-toolsets
```

**Specify toolset:**
```bash
sighmake project.buildscript -t msvc2019
```

## Converting Visual Studio Solutions

Convert existing Visual Studio solutions to buildscripts:

```bash
# Convert solution to buildscripts
sighmake --convert MyProject.slnx

# This will generate:
# - MyProject.buildscript (solution-level settings)
# - ProjectName.buildscript (for each project)
```

This feature is useful for:
- Migrating from Visual Studio to sighmake
- Creating portable build configurations
- Understanding existing project structure

## Workflow Examples

### Simple Console Application

**myapp.buildscript:**
```ini
[solution]
name = MyApp
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/*.cpp
includes = include
std = 17
subsystem = Console
```

Generate and build:
```batch
sighmake myapp.buildscript
sighmake --build . --config Release
```

### Cross-Platform Project

**myproject.buildscript:**
```ini
[solution]
name = MyProject
configurations = Debug, Release
platforms = x64, Linux

[project:MyProject]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17
subsystem = Console
```

**Build on Windows:**
```batch
sighmake myproject.buildscript -g vcxproj
sighmake --build . --config Release
```

**Build on Linux:**
```bash
./sighmake myproject.buildscript -g makefile
./sighmake --build . --config Release
```

## Tips and Best Practices

1. **Use wildcards** for source files to automatically include new files
2. **Share common settings** using the `include` directive
3. **Organize outputs** by configuration using `outdir` and `intdir`
4. **Version control** - Commit .buildscript files, not generated .vcxproj/.sln files
5. **Automate generation** - Add sighmake to your build scripts or CI pipeline
6. **Configuration-specific settings** - Use `[Config|Platform]` for fine-grained control
7. **Use `target_link_libraries()`** for automatic dependency management
8. **Use `find_package()`** for external SDK discovery

## Troubleshooting

### Common Issues

**Wildcard patterns not matching:**
- Ensure paths are relative to the buildscript location
- Use `**` for recursive matching

**Project dependencies not working:**
- Make sure the dependency project is defined before the dependent project
- Use `depends = ProjectName` in the dependent project
- Ensure library directories are set correctly with `libdirs`

**Toolset not found:**
- List available toolsets with `--list-toolsets`
- Specify toolset explicitly with `-t` flag
- Check Visual Studio installation

**Generated files not updating:**
- Regenerate project files after modifying buildscripts
- Close Visual Studio before regenerating to avoid file locks

## Example Projects

See sighmake in action with real-world projects:

### [SnakeGame](https://github.com/CitroenGames/SnakeGame)

A complete game project demonstrating advanced sighmake features:
- **Multi-project structure** - Game executable, Engine library, and 3rd-party dependencies
- **Dependency management** - PUBLIC/PRIVATE/INTERFACE visibility propagation
- **Modern C++20** - SDL3
- **Cross-platform** - Builds on Windows and Linux

**Project structure:**
```
snakegame/
├── engine/
│   ├── 3rdparty/          # Libraries (SDL3 wrappers)
│   └── src/               # C++ game engine
└── game/                  # Snake game executable
```

**Key features demonstrated:**
- Transitive dependency propagation (SDL3 INTERFACE)
- Multi-directory buildscript organization
- Public includes for library API exposure

## Documentation

For comprehensive documentation on all features, see [usage.md](usage.md):
- Detailed syntax reference
- Configuration-specific settings
- Per-file compiler flags
- Precompiled headers
- Build events
- Toolset configuration
- CMake support
- Assembly support (MASM, NASM)
- Kernel-mode driver support
- find_package() for external SDKs
- CI/CD integration
- And much more

## License

This project uses PugiXML, which is licensed under the MIT License.
