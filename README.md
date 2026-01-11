# sighmake

A flexible build system generator that converts buildscript files and CMakeLists.txt into Visual Studio project files (.vcxproj and .sln) and Makefiles. Designed for simplicity and cross-platform development.

## Features

- **Simple, readable buildscript format** - Human-friendly INI-style syntax
- **CMake support** - Parse CMakeLists.txt files and generate project files
- **Multiple generators** - Visual Studio projects (.vcxproj/.sln) and Makefiles
- **Cross-platform** - Windows (MSVC) and Linux (GCC/Clang) support
- **Configuration-specific settings** - Per-config compiler options, optimization, etc.
- **Per-file compiler settings** - Customize compilation flags for individual files
- **Wildcard support** - Use glob patterns for source files (*.cpp, **/*.cpp)
- **Project dependencies** - Automatic dependency tracking between projects
- **File inclusion** - Share common settings across buildscripts
- **Bidirectional conversion** - Convert Visual Studio solutions to buildscripts

## Quick Start

### Windows
```batch
# Generate Visual Studio project from buildscript
sighmake project.buildscript

# Generate from CMakeLists.txt
sighmake CMakeLists.txt

# Specify toolset
sighmake project.buildscript -t msvc2022
```

### Linux
```bash
# Generate Makefiles (default on Linux)
./sighmake project.buildscript

# Build the generated Makefile
make -f build/ProjectName.Release
```

## Installation

### Prerequisites
- C++17 compatible compiler (MSVC on Windows, GCC/Clang on Linux)

### Building from Source

#### Windows
```batch
# Use the provided executable
sighmake.exe

# Or generate and build from source
GenerateSolution.bat
# Then open and build in Visual Studio
```

#### Linux
```bash
# Bootstrap script to compile and generate Makefiles
chmod +x generate_linux.sh
./generate_linux.sh
make -f build/sighmake.Release
```

## Usage

```
sighmake <buildscript|CMakeLists.txt> [options]
sighmake --convert <solution.sln> [options]

Options:
  -g, --generator <type>     Generator type (vcxproj, makefile)
  -t, --toolset <name>       Default toolset (msvc2022, msvc2019, etc)
  -c, --convert              Convert Visual Studio solution to buildscripts
      --list-toolsets        List available toolsets
  -l, --list                 List available generators
  -h, --help                 Show help message
```

### Command Examples

**Generate Visual Studio project:**
```batch
sighmake project.buildscript -g vcxproj
```

**Generate with specific toolset:**
```batch
sighmake project.buildscript -t msvc2019
```

**Convert Visual Studio solution to buildscripts:**
```batch
sighmake --convert solution.sln
```

**Generate Makefiles:**
```bash
./sighmake project.buildscript -g makefile
# Output: build/ProjectName.Debug, build/ProjectName.Release
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
```

### Project Settings

#### Basic Project Properties

```ini
[project:ProjectName]
type = exe                      # Project type: exe, lib, dll
sources = src/*.cpp             # Source files (supports wildcards)
headers = include/*.h           # Header files
includes = include, external    # Include directories
defines = MYDEFINE              # Preprocessor definitions
std = 17                        # C++ standard (14, 17, 20, 23)
```

#### Dependencies and Libraries

```ini
depends = OtherProject         # Project dependencies
libs = user32.lib, gdi32.lib   # Library dependencies
libdirs = lib, external/lib    # Library search directories
```

#### Compiler Settings

```ini
warning_level = Level3         # Level0, Level1, Level2, Level3, Level4
multiprocessor = true          # Enable multi-processor compilation
exception_handling = Sync      # Sync, Async, or false
rtti = true                    # Runtime type information
optimization = MaxSpeed        # Disabled, MinSize, MaxSpeed, Full
runtime_library = MultiThreaded # MultiThreaded, MultiThreadedDebug, etc.
debug_info = ProgramDatabase   # None, ProgramDatabase, EditAndContinue
```

#### Linker Settings

```ini
subsystem = Console            # Console, Windows
generate_debug_info = true     # Generate debug information
link_incremental = false       # Incremental linking
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
optimization[Debug|Win32] = Disabled
optimization[Debug|x64] = Disabled
optimization[Release|Win32] = MaxSpeed
optimization[Release|x64] = MaxSpeed

# Per-configuration runtime library
runtime_library[Debug|Win32] = MultiThreadedDebug
runtime_library[Release|Win32] = MultiThreaded

# Per-configuration output directories
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release
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
utils.cpp:defines[Debug|Win32] = DEBUG_UTILS

# Per-file optimization
slow_file.cpp:optimization = Disabled
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

**common_settings.buildscript:**
```ini
# Common compiler settings
std = 17
warning_level = Level3
multiprocessor = true
defines = _CRT_SECURE_NO_WARNINGS
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
sighmake --convert MyProject.sln

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
# Open MyApp.sln in Visual Studio and build
```

### Cross-Platform Project

**myproject.buildscript:**
```ini
[solution]
name = MyProject
configurations = Debug, Release
platforms = Win32, x64

[project:MyProject]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17

# Windows-specific
subsystem[Win32] = Console
subsystem[x64] = Console

# Output directories
outdir[Debug|x64] = bin/Debug
outdir[Release|x64] = bin/Release
```

**Build on Windows:**
```batch
sighmake myproject.buildscript -g vcxproj
# Build in Visual Studio
```

**Build on Linux:**
```bash
./sighmake myproject.buildscript -g makefile
make -f build/MyProject.Release
```

### Multi-Configuration Build

```batch
# Generate project
sighmake project.buildscript

# Build specific configurations using MSBuild
msbuild project.sln /p:Configuration=Debug /p:Platform=x64
msbuild project.sln /p:Configuration=Release /p:Platform=x64
```

## Tips and Best Practices

1. **Use wildcards** for source files to automatically include new files
2. **Share common settings** using the `include` directive
3. **Organize outputs** by configuration using `outdir` and `intdir`
4. **Version control** - Commit .buildscript files, not generated .vcxproj/.sln files
5. **Automate generation** - Add sighmake to your build scripts or CI pipeline
6. **Configuration-specific settings** - Use `[Config|Platform]` for fine-grained control

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

## License

This project uses PugiXML, which is licensed under the MIT License.