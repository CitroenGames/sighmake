# sighmake Usage Guide

**A comprehensive guide to using sighmake, a flexible build system generator for C++ projects.**

---

## Table of Contents

1. [Introduction & Quick Start](#1-introduction--quick-start)
2. [Command-Line Interface](#2-command-line-interface)
3. [Buildscript Format Reference](#3-buildscript-format-reference)
4. [Configuration-Specific Settings](#4-configuration-specific-settings)
5. [Per-File Settings](#5-per-file-settings)
6. [File Inclusion](#6-file-inclusion)
7. [Project Types](#7-project-types)
8. [Dependencies and Linking](#8-dependencies-and-linking)
9. [Precompiled Headers (PCH)](#9-precompiled-headers-pch)
10. [Wildcard Patterns](#10-wildcard-patterns)
11. [Generators](#11-generators)
12. [Toolsets](#12-toolsets)
13. [Converting from Visual Studio](#13-converting-from-visual-studio)
14. [Cross-Platform Development](#14-cross-platform-development)
15. [CMake Integration](#15-cmake-integration)
16. [Multi-Project Solutions](#16-multi-project-solutions)
17. [Advanced Patterns](#17-advanced-patterns)
18. [Version Control Integration](#18-version-control-integration)
19. [CI/CD Integration](#19-cicd-integration)
20. [Best Practices](#20-best-practices)
21. [Common Patterns & Recipes](#21-common-patterns--recipes)
22. [Troubleshooting](#22-troubleshooting)
23. [Reference Tables](#23-reference-tables)

---

## 1. Introduction & Quick Start

### What is sighmake?

**sighmake** is a build system generator that converts human-readable buildscript files (INI-style format) and CMakeLists.txt files into Visual Studio project files (.vcxproj and .sln) and Makefiles. It's designed to simplify cross-platform C++ development and provide a version-control-friendly alternative to maintaining Visual Studio project files directly.

### Why use sighmake?

- **Simple, readable format**: INI-style syntax instead of verbose XML
- **Version control friendly**: Easy to diff and merge buildscripts
- **Cross-platform**: Generate Visual Studio projects on Windows, Makefiles on Linux
- **Bidirectional**: Convert existing Visual Studio solutions back to buildscripts
- **Flexible**: Per-configuration and per-file settings
- **Powerful**: Wildcard support, project dependencies, file inclusion

### 5-Minute Quick Start

#### Your First Buildscript

Create a file named `myapp.buildscript`:

```ini
[solution]
name = MyApp
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/*.cpp
headers = include/*.h
includes = include
std = 17
subsystem = Console
```

#### Generate and Build (Windows)

```batch
# Generate Visual Studio project
sighmake myapp.buildscript

# This creates:
#   MyApp_.sln       (solution file)
#   MyApp_.vcxproj   (project file)

# Open in Visual Studio and build, or use MSBuild:
msbuild MyApp_.sln /p:Configuration=Release /p:Platform=x64
```

#### Generate and Build (Linux)

```bash
# Generate Makefile
./sighmake myapp.buildscript -g makefile

# This creates:
#   build/MyApp.Debug    (Debug configuration Makefile)
#   build/MyApp.Release  (Release configuration Makefile)

# Build with make
make -f build/MyApp.Release
```

That's it! You now have a working build system. Continue reading for in-depth coverage of all features.

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

This minimal buildscript automatically gets:

**Debug Configuration:**
- Optimization: Disabled
- Runtime Library: MultiThreadedDebug
- Debug Info: EditAndContinue (Win32) or ProgramDatabase (x64)
- Incremental Linking: Enabled

**Release Configuration:**
- Optimization: MaxSpeed
- Runtime Library: MultiThreaded
- Debug Info: ProgramDatabase
- Function-Level Linking, Intrinsic Functions, COMDAT Folding, Optimize References: All enabled

Perfect for quick prototyping! To customize configurations, define your own `[config:Debug]` or `[config:Release]` sections.

---

## 2. Command-Line Interface

### Basic Syntax

```
sighmake <buildscript|CMakeLists.txt> [options]
sighmake --convert <solution.sln> [options]
```

### Options Reference

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-g <type>` | `--generator <type>` | Specify generator type (vcxproj, makefile) |
| `-t <name>` | `--toolset <name>` | Specify default toolset (msvc2022, msvc2019, etc.) |
| `-c` | `--convert` | Convert Visual Studio solution to buildscripts |
| | `--list-toolsets` | List all available Visual Studio toolsets |
| `-l` | `--list` | List all available generators |
| `-h` | `--help` | Display help message |

### Generator Selection

The generator determines what type of project files are created.

**Visual Studio Projects (vcxproj):**
```batch
sighmake project.buildscript -g vcxproj
```
- Creates `.vcxproj` (project) and `.sln` (solution) files
- Default on Windows

**Makefiles:**
```bash
sighmake project.buildscript -g makefile
```
- Creates GNU Makefiles in the `build/` directory
- Default on Linux

**List available generators:**
```bash
sighmake --list
```

### Toolset Configuration

Toolsets specify which version of Visual Studio to target.

**Specify toolset explicitly:**
```batch
sighmake project.buildscript -t msvc2019
```

**Set default toolset via environment variable:**
```batch
# Windows
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022

# Linux
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

**List available toolsets:**
```batch
sighmake --list-toolsets
```

Available toolsets: `msvc2026`, `msvc2022`, `msvc2019`, `msvc2017`, `msvc2015`, `msvc2013`, `msvc2012`, `msvc2010`

### Solution Conversion

Convert existing Visual Studio solutions to buildscripts:

```batch
sighmake --convert MySolution.sln
```

This generates:
- `MySolution.buildscript` (solution-level settings)
- `Project1.buildscript`, `Project2.buildscript`, etc. (one per project)

### Environment Variables

**SIGHMAKE_DEFAULT_TOOLSET**

Sets the default Visual Studio toolset globally:

```batch
# Windows
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022

# Linux/macOS
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

This is useful for team environments where everyone uses the same Visual Studio version.

### Command Examples

**Generate with default settings:**
```batch
sighmake project.buildscript
```

**Generate Visual Studio 2019 project:**
```batch
sighmake project.buildscript -t msvc2019
```

**Generate Makefiles on Windows:**
```batch
sighmake project.buildscript -g makefile
```

**Parse CMakeLists.txt and generate Visual Studio project:**
```batch
sighmake CMakeLists.txt -g vcxproj
```

**Convert existing solution:**
```batch
sighmake --convert MyProject.sln
```

---

## 3. Buildscript Format Reference

Buildscripts use an **INI-style syntax** with sections for solutions and projects.

### File Structure

```ini
[solution]
# Solution-level settings

[project:ProjectName]
# Project-level settings

[project:AnotherProject]
# Another project
```

### Solution Section

The `[solution]` section defines solution-level settings.

**Syntax:**
```ini
[solution]
name = MySolution
configurations = Debug, Release
platforms = Win32, x64
```

**Settings:**

| Setting | Description | Example |
|---------|-------------|---------|
| `name` | Solution name | `name = MySolution` |
| `configurations` | Build configurations (comma-separated) | `configurations = Debug, Release, Profile` |
| `platforms` | Target platforms (comma-separated) | `platforms = Win32, x64` |

**Common configuration names:**
- `Debug` - Debug build with symbols
- `Release` - Optimized release build
- `RelWithDebInfo` - Release with debug info
- `MinSizeRel` - Optimized for size
- `Profile` - Profiling build
- Custom names are supported

**Common platform names:**
- `Win32` - 32-bit x86 (vcxproj only)
- `x64` - 64-bit x86-64 (vcxproj only)
- `ARM` - ARM 32-bit (vcxproj only)
- `ARM64` - ARM 64-bit (vcxproj only)
- `Linux` - Linux platform (makefile only)

**Platform filtering by generator:**
Platforms are automatically filtered by generator type:
- **vcxproj generator**: Only includes Win32, x64, ARM, ARM64 platforms (skips Linux)
- **makefile generator**: Only includes Linux platform (skips Windows platforms)

This allows a single buildscript to define both Windows and Linux configurations:
```ini
platforms = x64, Linux

[config:Debug|x64]
defines = _DEBUG, _WIN32

[config:Debug|Linux]
defines = _DEBUG, __linux__
```

### Project Section

The `[project:ProjectName]` section defines a project.

**Syntax:**
```ini
[project:MyProject]
type = exe
sources = src/*.cpp
headers = include/*.h
```

### Basic Project Properties

| Setting | Description | Valid Values | Default |
|---------|-------------|--------------|---------|
| `type` | Project type | `exe`, `lib`, `dll` | Required |
| `target_name` | Output file name (without extension) | Any string | Project name |
| `target_ext` | Output file extension | `.exe`, `.dll`, `.lib`, etc. | Based on type |
| `std` | C++ standard version | `14`, `17`, `20`, `23` | Compiler default |

**Example:**
```ini
[project:MyEngine]
type = lib
target_name = engine
target_ext = .lib
std = 20
```

### Source Organization

| Setting | Description | Supports Wildcards |
|---------|-------------|-------------------|
| `sources` | Source files to compile | Yes |
| `headers` | Header files (for IDE organization) | Yes |
| `includes` | Include directories (comma-separated) | No |
| `defines` | Preprocessor definitions (comma-separated) | No |

**Example:**
```ini
sources = src/*.cpp, external/lib/*.cpp
headers = include/**/*.h
includes = include, external/include, C:/Libraries/boost
defines = MY_DEFINE, DEBUG_ENABLED, VERSION=1.0
```

### Output Directories

| Setting | Description | Example |
|---------|-------------|---------|
| `outdir` | Output directory for binaries | `outdir = bin/Release` |
| `intdir` | Intermediate directory for object files | `intdir = obj/Release` |

**Example:**
```ini
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release
intdir[Debug|x64] = obj/x64/Debug
intdir[Release|x64] = obj/x64/Release
```

### Precompiled Headers

| Setting | Description | Valid Values |
|---------|-------------|--------------|
| `pch` | PCH mode | `Use`, `Create`, `NotUsing` |
| `pch_header` | PCH header file name | Filename (e.g., `pch.h`) |
| `pch_output` | PCH output file path | Path (e.g., `obj/pch.pch`) |

**Example:**
```ini
pch = Use
pch_header = src/pch.h
pch_output[Debug|Win32] = obj/Debug/pch.pch
pch_output[Release|Win32] = obj/Release/pch.pch
```

### Compiler Settings

| Setting | Description | Valid Values |
|---------|-------------|--------------|
| `warning_level` | Warning level | `Level0`, `Level1`, `Level2`, `Level3`, `Level4` |
| `multiprocessor` | Multi-processor compilation | `true`, `false` |
| `utf8` | UTF-8 source/execution encoding | `true`, `false` |
| `exception_handling` | Exception handling mode | `Sync`, `Async`, `false` |
| `rtti` | Runtime type information | `true`, `false` |
| `optimization` | Optimization level | `Disabled`, `MinSize`, `MaxSpeed`, `Full` |
| `runtime_library` | Runtime library linkage | See table below |
| `debug_info` | Debug information format | `None`, `ProgramDatabase`, `EditAndContinue` |

**Runtime Library Values:**
- `MultiThreaded` - Static, release
- `MultiThreadedDebug` - Static, debug
- `MultiThreadedDLL` - Dynamic, release
- `MultiThreadedDebugDLL` - Dynamic, debug

**Example:**
```ini
warning_level = Level4
multiprocessor = true
exception_handling = Sync
rtti = true
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
runtime_library[Debug] = MultiThreadedDebug
runtime_library[Release] = MultiThreaded
debug_info[Debug] = EditAndContinue
debug_info[Release] = ProgramDatabase
```

### UTF-8 Source Encoding

Enable UTF-8 encoding for source files and execution character sets. This is required by some libraries (e.g., spdlog, fmt) that use Unicode characters.

**Syntax:**
```ini
utf8 = true
```

**What it does:**
- **MSVC**: Adds `/utf-8` compiler flag
  - Sets source file encoding to UTF-8
  - Sets execution character set to UTF-8
- **GCC/Clang**: Adds `-finput-charset=UTF-8 -fexec-charset=UTF-8`
  - Ensures source files are read as UTF-8
  - Sets execution character set to UTF-8

**When to use:**
- When using libraries that require UTF-8 compilation (spdlog, fmt, etc.)
- When your source files contain Unicode characters
- When you see errors like: `error C2338: Unicode support requires compiling with /utf-8`

**Example:**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp
std = 20
utf8 = true                    # Enable UTF-8 encoding

# Or enable per-configuration
utf8[Debug] = true
utf8[Release] = true
```

**Full example with spdlog:**
```ini
[project:LoggingApp]
type = exe
sources = src/**/*.cpp
includes =
    include
    external/spdlog/include
std = 20
utf8 = true                    # Required by spdlog
multiprocessor = true
```

### Linker Settings

| Setting | Description | Valid Values |
|---------|-------------|--------------|
| `subsystem` | Subsystem type | `Console`, `Windows` |
| `generate_debug_info` | Generate debug information | `true`, `false` |
| `link_incremental` | Incremental linking | `true`, `false` |

**Example:**
```ini
subsystem = Console
generate_debug_info[Debug] = true
generate_debug_info[Release] = true
link_incremental[Debug] = true
link_incremental[Release] = false
```

---

## 4. Configuration-Specific Settings

Many settings can be specified per-configuration and/or per-platform using bracket notation.

### Syntax

```ini
setting[Configuration|Platform] = value
```

**Components:**
- `Configuration` - The build configuration (e.g., Debug, Release)
- `Platform` - The target platform (e.g., Win32, x64)
- Both are optional, use `|` to separate when both are specified

### Examples

**Per-configuration:**
```ini
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
```

**Per-platform:**
```ini
defines[Win32] = WIN32_BUILD
defines[x64] = X64_BUILD
```

**Per-configuration AND platform:**
```ini
optimization[Debug|Win32] = Disabled
optimization[Debug|x64] = Disabled
optimization[Release|Win32] = MaxSpeed
optimization[Release|x64] = Full
```

### Common Patterns

**Different optimization per configuration:**
```ini
[project:MyProject]
# Debug builds: no optimization
optimization[Debug|Win32] = Disabled
optimization[Debug|x64] = Disabled

# Release builds: maximum speed
optimization[Release|Win32] = MaxSpeed
optimization[Release|x64] = MaxSpeed
```

**Different runtime library per configuration:**
```ini
# Debug: link to debug runtime
runtime_library[Debug|Win32] = MultiThreadedDebug
runtime_library[Debug|x64] = MultiThreadedDebug

# Release: link to release runtime
runtime_library[Release|Win32] = MultiThreaded
runtime_library[Release|x64] = MultiThreaded
```

**Different output directories:**
```ini
outdir[Debug|Win32] = bin/Win32/Debug
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Release|x64] = bin/x64/Release

intdir[Debug|Win32] = obj/Win32/Debug
intdir[Debug|x64] = obj/x64/Debug
intdir[Release|Win32] = obj/Win32/Release
intdir[Release|x64] = obj/x64/Release
```

**Platform-specific preprocessor defines:**
```ini
defines = COMMON_DEFINE
defines[Win32] = WIN32, _WINDOWS
defines[x64] = X64, _WIN64
defines[Debug] = DEBUG, _DEBUG
defines[Release] = NDEBUG
```

**Debug information format:**
```ini
# Edit and Continue only works with Win32 Debug
debug_info[Debug|Win32] = EditAndContinue
debug_info[Debug|x64] = ProgramDatabase
debug_info[Release|Win32] = ProgramDatabase
debug_info[Release|x64] = ProgramDatabase
```

### Fallback Behavior

Settings are resolved in this order:
1. Configuration + Platform specific: `setting[Debug|x64]`
2. Configuration specific: `setting[Debug]`
3. Platform specific: `setting[x64]`
4. Default (no bracket): `setting`

**Example:**
```ini
optimization = MinSize              # Default fallback
optimization[Release] = MaxSpeed    # All Release builds
optimization[Release|x64] = Full    # Only Release x64
```

With the above settings:
- `Debug|Win32`: Uses `MinSize` (default fallback)
- `Debug|x64`: Uses `MinSize` (default fallback)
- `Release|Win32`: Uses `MaxSpeed` (configuration-specific)
- `Release|x64`: Uses `Full` (most specific match)

### Configuration Templates

You can define configuration templates to reduce duplication when creating custom configurations. This allows you to create new configurations that inherit all settings from an existing template configuration.

**Syntax:**
```ini
[config:DerivedConfig|Platform] : Template:BaseConfig
[config:DerivedConfig] : Template:BaseConfig
```

**Features:**
- Templates are regular configurations that other configs inherit from
- Derived configs inherit **all** settings from the template
- Individual settings can be overridden in the derived config
- Works with platform-specific (`[config:Test|Win32]`) and all-platform (`[config:Test]`) syntax

**Example - Single Platform Template:**
```ini
[solution]
configurations = Debug, Release, Test
platforms = Win32, x64

[project:MyApp]
type = exe
sources = src/*.cpp

# Define Release as a base template
[config:Release|Win32]
optimization = MaxSpeed
runtime_library = MultiThreaded
whole_program_optimization = true
defines = NDEBUG

# Test config inherits all Release settings for Win32
[config:Test|Win32] : Template:Release
defines = NDEBUG, TEST_MODE  # Override only defines
```

In this example, `Test|Win32` will have:
- `optimization = MaxSpeed` (from Release)
- `runtime_library = MultiThreaded` (from Release)
- `whole_program_optimization = true` (from Release)
- `defines = NDEBUG, TEST_MODE` (overridden in Test)

**Example - All Platforms Template:**
```ini
[solution]
configurations = Debug, Release, Profile
platforms = Win32, x64

[project:MyApp]
type = exe
sources = src/*.cpp

# Define Release settings for all platforms
[config:Release]
optimization = MaxSpeed
runtime_library = MultiThreaded
defines = NDEBUG

# Profile inherits from Release for ALL platforms
[config:Profile] : Template:Release
defines = NDEBUG, PROFILE_MODE  # Override defines
```

In this example:
- `Profile|Win32` inherits all settings from `Release|Win32`
- `Profile|x64` inherits all settings from `Release|x64`
- Both platforms override `defines` to include `PROFILE_MODE`

**Example - Multiple Custom Configurations:**
```ini
[solution]
configurations = Debug, Release, Test, Staging, Production
platforms = Win32, x64

[project:MyApp]
type = exe
sources = src/*.cpp

# Base Release configuration
[config:Release]
optimization = MaxSpeed
runtime_library = MultiThreaded
function_level_linking = true
enable_comdat_folding = true
defines = NDEBUG

# Test: Like Release but with test defines
[config:Test] : Template:Release
defines = NDEBUG, ENABLE_TESTING

# Staging: Like Release but with staging URL
[config:Staging] : Template:Release
defines = NDEBUG, STAGING_ENV, API_URL="https://staging.api.com"

# Production: Like Release with production settings
[config:Production] : Template:Release
defines = NDEBUG, PRODUCTION_ENV, API_URL="https://api.com"
whole_program_optimization = true  # Extra optimization for production
```

**Important Notes:**
- Templates must be defined before they are referenced (define `[config:Release]` before using `: Template:Release`)
- Templates remain buildable configurations themselves
- Override order: Derived config settings → Template settings → System defaults
- Circular template references (e.g., `Test : Template:Test`) are detected and produce an error

### Conditional Compilation Blocks

sighmake supports conditional compilation blocks using `if(Platform)` syntax. This provides an alternative to the bracket notation for platform-specific settings.

**Syntax:**
```ini
if(Platform)
{
    setting = value
    another_setting = value
}
```

**Supported conditions:**
- `if(Windows)` - Settings apply to all Windows platforms (Win32, x64)
- `if(Linux)` - Settings apply to Linux platforms
- `if(Win32)` - Settings apply only to Win32 platform
- `if(x64)` - Settings apply only to x64 platform

**Example:**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp
includes = include
std = 20

# Common settings
subsystem = Console

# Windows-specific settings
if(Windows)
{
    subsystem = Windows
    defines = WIN32_LEAN_AND_MEAN
    libs = user32.lib, gdi32.lib
}

# Linux-specific settings
if(Linux)
{
    defines = __LINUX__
    libs = pthread, dl
}
```

**Comparison with bracket notation:**

These two styles are equivalent:

**Conditional blocks:**
```ini
if(Windows)
{
    defines = PLATFORM_WINDOWS
    libs = user32.lib
}
```

**Bracket notation:**

Bracket notation supports four formats:

| Format | Example | Description |
|--------|---------|-------------|
| `[Config\|Platform]` | `defines[Debug\|Win32]` | Applies to specific configuration and platform |
| `[Platform]` | `defines[Win32]` | Applies to all configurations for that platform |
| `[Config]` | `defines[Debug]` | Applies to specific configuration (all platforms) |
| `[*]` | `pch[*]` | Applies to all configurations and all platforms |

```ini
# Wildcard: applies to ALL configurations and ALL platforms
pch[*] = Use

# Platform-only: applies to Debug|Linux AND Release|Linux
defines[Linux] = __linux__, PLATFORM_LINUX
libs[Linux] = pthread, dl

# Platform-only: applies to Debug|Win32 AND Release|Win32 AND Debug|x64 AND Release|x64
defines[Win32] = PLATFORM_WINDOWS
defines[x64] = PLATFORM_WINDOWS
libs[Win32] = user32.lib
libs[x64] = user32.lib

# Full config|platform: applies only to Debug|Win32
defines[Debug|Win32] = _DEBUG, WIN32_DEBUG
```

**When to use conditional blocks:**
- Use conditional blocks when you have multiple settings that apply to the same platform
- Use bracket notation for single settings or when you need fine-grained control per configuration

**Important Notes:**
- Conditional blocks can be nested
- Settings inside blocks override settings defined outside
- Platform-only bracket syntax (`[Linux]`, `[Win32]`) expands to all configurations for that platform
- Wildcard bracket syntax (`[*]`) expands to all configuration and platform combinations

---

## 5. Per-File Settings

You can apply settings to individual source files using per-file syntax.

### Syntax

```ini
filename.cpp:setting = value
filename.cpp:setting[Configuration|Platform] = value
filename.cpp:setting[*] = value   # Applies to all configurations/platforms
```

### Use Cases

**1. Precompiled Header Creation**

One file creates the PCH, others use it:

```ini
[project:MyProject]
pch = Use
pch_header = pch.h

# This file creates the PCH
pch.cpp:pch = Create
pch.cpp:pch_header = pch.h
```

**2. Excluding Files from PCH**

Third-party libraries often don't work with PCH:

```ini
# Exclude third-party code from PCH
external/pugixml.cpp:pch = NotUsing
external/json.cpp:pch = NotUsing
```

**3. Per-File Optimization**

Disable optimization for specific files:

```ini
# Optimize everything by default
optimization[Release] = MaxSpeed

# But disable optimization for debugging-heavy code
debug_utils.cpp:optimization[Release] = Disabled
```

**4. Per-File Defines**

Add extra defines to specific files:

```ini
# All files get these defines
defines = COMMON_DEFINE

# This file gets an extra define
platform_windows.cpp:defines = PLATFORM_WINDOWS
platform_linux.cpp:defines = PLATFORM_LINUX
```

**5. Per-File Warning Level**

Silence warnings in legacy code:

```ini
# Strict warnings by default
warning_level = Level4

# Reduce warnings for legacy code
legacy/old_code.cpp:warning_level = Level2
```

### Full Example

```ini
[project:Engine]
type = lib
sources = src/**/*.cpp
includes = src
std = 20

# All files use PCH
pch = Use
pch_header = pch.h

# pch.cpp creates the PCH
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h

# Third-party libraries don't use PCH
src/external/stb_image.cpp:pch = NotUsing
src/external/miniz.cpp:pch = NotUsing

# Disable optimization for debugger-friendly code
src/debug/debugger.cpp:optimization = Disabled

# High optimization for hot path
src/core/inner_loop.cpp:optimization[Release] = Full
```

### Important Notes

- File paths in per-file settings must match the wildcards or explicit paths used in the `sources` setting
- Use forward slashes (`/`) in paths even on Windows
- Per-file settings override project-level settings
- Configuration-specific per-file settings are supported: `file.cpp:setting[Config] = value`

---

## 6. File Inclusion

The `include` directive allows you to share common settings across multiple buildscripts.

### Syntax

```ini
include = path/to/settings.buildscript
include[Configuration|Platform] = path/to/settings.buildscript
```

### Basic Usage

**main.buildscript:**
```ini
[project:MyProject]
type = exe
sources = src/*.cpp

# Include common settings
include = common_settings.buildscript
```

**common_settings.buildscript:**
```ini
# Common compiler settings
std = 17
warning_level = Level3
multiprocessor = true
defines = _CRT_SECURE_NO_WARNINGS

# Configuration-specific settings
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
runtime_library[Debug] = MultiThreadedDebug
runtime_library[Release] = MultiThreaded
```

### Configuration-Specific Includes

You can include different files based on configuration:

```ini
[project:MyProject]
type = exe
sources = src/*.cpp

# Include different settings per configuration
include[Debug] = debug_settings.buildscript
include[Release] = release_settings.buildscript
```

### Platform-Specific Includes

```ini
[project:CrossPlatform]
type = exe
sources = src/**/*.cpp

# Common settings for all platforms
include = common_settings.buildscript

# Platform-specific settings
include[Win32] = windows_settings.buildscript
include[x64] = windows_x64_settings.buildscript
```

### Multiple Includes

You can have multiple include directives:

```ini
[project:MyProject]
type = exe
sources = src/*.cpp

# Include multiple setting files
include = common_settings.buildscript
include = compiler_flags.buildscript
include = third_party_paths.buildscript
```

### Practical Example: Team Shared Settings

**team_standard.buildscript:**
```ini
# Team-wide C++ standard
std = 17

# Team-wide warning level
warning_level = Level4

# Team-wide compiler settings
multiprocessor = true
exception_handling = Sync
rtti = true

# Standard configuration settings
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
runtime_library[Debug] = MultiThreadedDebugDLL
runtime_library[Release] = MultiThreadedDLL
debug_info[Debug] = ProgramDatabase
debug_info[Release] = ProgramDatabase
```

**project_specific.buildscript:**
```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include

# Include team standards
include = ../shared/team_standard.buildscript

# Project-specific settings
defines = MY_APP_VERSION=1.0
subsystem = Console
```

### Include Search Paths

- Paths are relative to the buildscript that contains the `include` directive
- Absolute paths are supported
- Use forward slashes (`/`) for cross-platform compatibility

### Best Practices

1. **Share common settings**: Put team-wide compiler flags in a shared file
2. **Configuration templates**: Create include files for Debug, Release, Profile configs
3. **Third-party paths**: Put external library paths in a separate include file
4. **Platform settings**: Separate Win32/x64 specific settings
5. **Version control**: Commit include files alongside buildscripts

### Important Notes

- Include files should contain only project-level settings (not `[solution]` or `[project:Name]` sections)
- Settings in the main buildscript override settings from included files
- Includes are processed in order, later includes can override earlier ones
- Circular includes are not supported

---

## 7. Project Types

sighmake supports three project types: executables, static libraries, and dynamic libraries.

### Executable (exe)

Produces a standalone executable (.exe on Windows).

**Basic syntax:**
```ini
[project:MyApp]
type = exe
sources = src/*.cpp
subsystem = Console
```

**Subsystem options:**
- `Console` - Console application (shows command prompt)
- `Windows` - GUI application (no console window)

**Full example:**
```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17
defines = MY_APP
subsystem = Console

# Output settings
target_name = myapp
target_ext = .exe
outdir[Debug] = bin/Debug
outdir[Release] = bin/Release
```

### Static Library (lib)

Produces a static library (.lib on Windows, .a on Linux) that is linked directly into executables.

**Basic syntax:**
```ini
[project:MathLib]
type = lib
sources = src/*.cpp
headers = include/*.h
```

**Full example:**
```ini
[project:MathLib]
type = lib
sources = src/math/*.cpp
headers = include/math/*.h
includes = include
std = 17
defines = MATHLIB_STATIC

# Output settings
target_name = math
target_ext = .lib
outdir = lib/Release
```

**Using a static library in an executable:**
```ini
[project:MyApp]
type = exe
sources = src/*.cpp
includes = ../MathLib/include
depends = MathLib
libs = math.lib
libdirs = ../MathLib/lib/Release
```

### Library Public Interface

Libraries can expose their include directories and library files to dependent projects using `public_includes` and `public_libs`.

**Syntax:**
```ini
[project:LibraryProject]
type = lib
public_includes = include, external/include
public_libs = lib/x64/prebuilt.lib
```

**Settings:**

| Setting | Description | Example |
|---------|-------------|---------|
| `public_includes` | Include directories exposed to dependent projects | `public_includes = include` |
| `public_libs` | Pre-built library files to link | `public_libs = lib/x64/SDL3.lib` |

**Example with pre-built library:**
```ini
[project:SDL3]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL3.lib
```

This is useful for wrapping third-party libraries that provide pre-built binaries. Projects that depend on SDL3 will automatically get the include directories and libraries.

### Dynamic Library (dll)

Produces a dynamic library (.dll on Windows, .so on Linux) that is loaded at runtime.

**Basic syntax:**
```ini
[project:MyDLL]
type = dll
sources = src/*.cpp
defines = MYDLL_EXPORTS
```

**Full example with export/import:**

**mathlib.buildscript:**
```ini
[project:MathLib]
type = dll
sources = mathlib/*.cpp
headers = mathlib/*.h
includes = mathlib
std = 17

# Export symbols from DLL
defines = MATHLIB_EXPORTS

outdir[Debug] = output/Debug
outdir[Release] = output/Release
```

**mathlib.h:**
```cpp
// Define export/import macros
#ifdef MATHLIB_EXPORTS
    #define MATHLIB_API __declspec(dllexport)
#else
    #define MATHLIB_API __declspec(dllimport)
#endif

// Exported function
MATHLIB_API int Add(int a, int b);
```

**mathlib.cpp:**
```cpp
#include "mathlib.h"

MATHLIB_API int Add(int a, int b) {
    return a + b;
}
```

**Using the DLL:**

```ini
[project:Calculator]
type = exe
sources = calculator/*.cpp
includes = mathlib
depends = MathLib

# Link to import library
libs = MathLib.lib
libdirs[Debug] = output/Debug
libdirs[Release] = output/Release

subsystem = Console
```

### Type Comparison

| Feature | exe | lib | dll |
|---------|-----|-----|-----|
| Produces | Executable | Static library | Dynamic library |
| Extension (Windows) | .exe | .lib | .dll |
| Extension (Linux) | (none) | .a | .so |
| Linked at | N/A | Compile time | Runtime |
| Export macros needed | No | No | Yes |
| Multiple instances | Each exe is separate | Compiled into each exe | Shared in memory |
| Update without recompile | No | No | Yes |

### Choosing a Project Type

**Use `exe` when:**
- Creating an application
- Creating a command-line tool
- Creating a test executable

**Use `lib` when:**
- Creating a reusable library
- No runtime dependencies needed
- Maximum performance (no DLL overhead)
- Simpler deployment (no separate DLL files)

**Use `dll` when:**
- Creating a plugin system
- Need to update library without recompiling applications
- Sharing code between multiple executables
- Creating a COM component

---

## 8. Dependencies and Linking

sighmake provides powerful dependency management for multi-project solutions.

### Project Dependencies

Use the `depends` setting to specify that a project depends on another project in the same solution.

**Syntax:**
```ini
depends = Project1, Project2, Project3
```

**Example:**
```ini
[solution]
name = MyApp
configurations = Debug, Release
platforms = x64

# Library project
[project:CoreLib]
type = lib
sources = corelib/*.cpp

# Application depends on CoreLib
[project:Application]
type = exe
sources = app/*.cpp
depends = CoreLib
libs = CoreLib.lib
```

**What `depends` does:**
- Sets build order (CoreLib builds before Application)
- Creates project reference in Visual Studio
- Ensures proper dependency tracking

### External Libraries

Use the `libs` setting to link external libraries.

**Syntax:**
```ini
libs = library1.lib, library2.lib, library3.lib
```

**Example:**
```ini
[project:MyGame]
type = exe
sources = src/*.cpp

# Link to system libraries
libs = user32.lib, gdi32.lib, opengl32.lib

# Link to external libraries
libs = SDL2.lib, SDL2main.lib
```

### Library Search Paths

Use `libdirs` to specify where to find library files.

**Syntax:**
```ini
libdirs = path/to/libs, another/path
```

**Example:**
```ini
[project:MyGame]
type = exe
sources = src/*.cpp
libs = SDL2.lib
libdirs = C:/Libraries/SDL2/lib/x64
```

### Complete Multi-Project Example

**multi_project.buildscript:**
```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = Win32, x64

# --- Core Library (Static) ---
[project:CoreLib]
type = lib
sources = core/*.cpp
headers = core/*.h
includes = core
std = 17

outdir[Debug|Win32] = lib/Win32/Debug
outdir[Release|Win32] = lib/Win32/Release
outdir[Debug|x64] = lib/x64/Debug
outdir[Release|x64] = lib/x64/Release

# --- Utilities DLL ---
[project:UtilsDLL]
type = dll
sources = utils/*.cpp
headers = utils/*.h
includes = utils, core
defines = UTILS_EXPORTS
std = 17

# UtilsDLL depends on CoreLib
depends = CoreLib
libs = CoreLib.lib
libdirs[Debug|Win32] = lib/Win32/Debug
libdirs[Release|Win32] = lib/Win32/Release
libdirs[Debug|x64] = lib/x64/Debug
libdirs[Release|x64] = lib/x64/Release

outdir[Debug|Win32] = bin/Win32/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release

# --- Main Application ---
[project:Application]
type = exe
sources = app/*.cpp
includes = core, utils
std = 17
subsystem = Console

# Application depends on both CoreLib and UtilsDLL
depends = CoreLib, UtilsDLL
libs = CoreLib.lib, UtilsDLL.lib
libdirs[Debug|Win32] = lib/Win32/Debug, bin/Win32/Debug
libdirs[Release|Win32] = lib/Win32/Release, bin/Win32/Release
libdirs[Debug|x64] = lib/x64/Debug, bin/x64/Debug
libdirs[Release|x64] = lib/x64/Release, bin/x64/Release

# Output to same directory as UtilsDLL for runtime loading
outdir[Debug|Win32] = bin/Win32/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release
```

### DLL Runtime Loading

When using DLLs, the executable must be able to find the DLL at runtime.

**Option 1: Same directory**
Place the .exe and .dll in the same output directory:
```ini
# DLL output
[project:MyDLL]
outdir = bin/Release

# EXE output (same directory)
[project:MyApp]
outdir = bin/Release
```

**Option 2: System PATH**
Place DLL in a directory on the system PATH.

**Option 3: Copy DLL**
Use a post-build script to copy the DLL to the exe directory.

### Import Libraries for DLLs

When you build a DLL on Windows, two files are created:
- `MyDLL.dll` - The actual dynamic library (runtime)
- `MyDLL.lib` - Import library (link time)

**DLL project:**
```ini
[project:MyDLL]
type = dll
sources = src/*.cpp
defines = MYDLL_EXPORTS
outdir = bin/Release
```

This creates:
- `bin/Release/MyDLL.dll`
- `bin/Release/MyDLL.lib` (import library)

**Application project:**
```ini
[project:MyApp]
type = exe
sources = app/*.cpp
depends = MyDLL
libs = MyDLL.lib
libdirs = bin/Release  # Where to find MyDLL.lib
outdir = bin/Release   # Same directory as MyDLL.dll
```

### Dependency Best Practices

1. **Use `depends` for internal projects**: Ensures correct build order
2. **Match output directories**: Put DLLs and EXEs in the same directory
3. **Specify `libdirs` carefully**: Point to where .lib files are located
4. **Use relative paths**: Makes projects portable
5. **Configuration-specific paths**: Use different paths for Debug/Release

### Alternative Syntax: target_link_libraries()

You can use the `target_link_libraries()` function syntax as an alternative to the `depends` setting.

**Syntax:**
```ini
target_link_libraries(ProjectName)
```

**Example:**
```ini
[solution]
name = MyApplication

[project:CoreLib]
type = lib
sources = core/*.cpp
headers = core/*.h
public_includes = core

[project:MyApp]
type = exe
sources = app/*.cpp
target_link_libraries(CoreLib)
```

**What `target_link_libraries()` does:**
- Automatically adds the project to `depends` (sets build order)
- Links the library's output (.lib file)
- Adds the library's `public_includes` to the include path
- Adds the library's `public_libs` to the linker

This is particularly useful when working with third-party libraries that define `public_includes` and `public_libs`, as it automatically handles all the dependency setup.

**Example with third-party library:**
```ini
[project:SDL3]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL3.lib

[project:MyGame]
type = exe
sources = src/**/*.cpp
target_link_libraries(SDL3)
# Automatically gets:
# - include paths from SDL3's public_includes
# - library files from SDL3's public_libs
```

### Dependency Visibility (CMake-style)

Control how dependencies propagate using visibility modifiers. This allows fine-grained control over transitive dependency propagation, similar to modern CMake.

**Visibility Modifiers:**

Sighmake follows CMake's standard dependency visibility semantics:

| Visibility | Applies to this target | Propagates to dependents |
|-----------|------------------------|-------------------------|
| **PRIVATE** | ✓ YES | ✗ NO |
| **INTERFACE** | ✗ NO | ✓ YES |
| **PUBLIC** | ✓ YES | ✓ YES |

- **PUBLIC**: Dependency affects both the target and all its dependents (default)
  - Adds `public_includes`, `public_libs`, `public_defines` to current target
  - Propagates these properties transitively to all consumers
  - Use for dependencies that are part of your public API

- **PRIVATE**: Dependency affects only the target, not its dependents
  - Adds `public_includes`, `public_libs`, `public_defines` to current target
  - Does NOT propagate to consumers (stops transitive dependency chain)
  - Use for internal implementation details

- **INTERFACE**: Dependency affects only dependents, not the target itself
  - Does NOT add to current target
  - Propagates `public_includes`, `public_libs`, `public_defines` to consumers
  - Use for header-only libraries or when target doesn't link but consumers do

**Syntax:**
```ini
# Multi-line syntax (CMake-style, whitespace-separated)
target_link_libraries(ProjectName
    PUBLIC dep1 dep2
    PRIVATE dep3
    INTERFACE dep4
)

# Or single-line (comma-separated or whitespace-separated)
target_link_libraries(ProjectName, PUBLIC dep1, dep2, PRIVATE dep3, INTERFACE dep4)
```

**Real-World Example (from SnakeGame project):**
```ini
# SDL3 - Header-only wrapper that provides SDL headers and .lib
[project:SDL3]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL3.lib

# entt - Header-only ECS library
[project:entt]
type = lib
headers = entt.hpp
public_includes = src

# Engine - Game engine library
[project:Engine]
type = lib
sources = src/**/*.cpp
headers = src/**/*.h
public_includes = src
target_link_libraries(
    INTERFACE SDL3  # SDL3 headers exposed to Engine users, but Engine doesn't link it
    PUBLIC entt     # entt headers/functionality exposed to Engine users
)
std = 20

# SnakeGame - Actual game executable
[project:SnakeGame]
type = exe
sources = src/**/*.cpp
headers = src/**/*.h
target_link_libraries(Engine)
std = 20

# SnakeGame automatically gets:
# - Engine.lib (links directly)
# - entt headers (PUBLIC through Engine)
# - SDL3 headers (INTERFACE through Engine)
# - SDL3.lib (via SDL3's public_libs through INTERFACE propagation)
```

**Why this works:**
- Engine uses SDL3 headers but doesn't need to link SDL3.lib directly (INTERFACE)
- Engine uses entt headers in its public API, so users need it too (PUBLIC)
- SnakeGame gets everything it needs without explicitly declaring SDL3 or entt

**How Transitive Propagation Works:**

1. **PUBLIC chain**: `SnakeGame → Engine → (PUBLIC) → entt`
   - Result: SnakeGame gets entt headers and can use ECS components
   - Use case: entt is part of Engine's public API (components/systems visible to game)

2. **INTERFACE propagation**: `SnakeGame → Engine → (INTERFACE) → SDL3`
   - Result: SnakeGame gets SDL3 headers and SDL3.lib (via public_libs)
   - Use case: SDL3 types appear in Engine's API, but Engine doesn't link SDL3 itself
   - **Key insight**: INTERFACE means "my users need this, but I don't link it"

3. **PRIVATE boundary**: `SnakeGame → Engine → (PRIVATE) → InternalProfiler`
   - Result: Engine gets InternalProfiler's includes/libs, but SnakeGame does NOT
   - Use case: Profiler is Engine's internal implementation detail
   - **Key insight**: PRIVATE means "I need this, but my users don't"

**Multi-Project Directory Structure Example:**

Project structure:
```
myproject/
├── game/
│   ├── game.buildscript
│   └── src/
├── engine/
│   ├── engine.buildscript
│   ├── src/
│   └── 3rdparty/
│       ├── 3rdparty.buildscript  # Aggregates all 3rd party libs
│       ├── entt/
│       │   └── entt.buildscript
│       └── SDL3/
│           └── sdl3.buildscript
```

**3rdparty/3rdparty.buildscript** (aggregator):
```ini
include = entt/entt.buildscript
include = SDL3/sdl3.buildscript
```

**3rdparty/SDL3/sdl3.buildscript**:
```ini
[project:SDL3]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL3.lib
```

**3rdparty/entt/entt.buildscript**:
```ini
[project:entt]
type = lib
headers = entt.hpp
public_includes = src
```

**engine/engine.buildscript**:
```ini
include = 3rdparty/3rdparty.buildscript

[project:Engine]
type = lib
sources = src/**/*.cpp
headers = src/**/*.h
public_includes = src
target_link_libraries(
    INTERFACE SDL3  # SDL types in Engine's public API
    PUBLIC entt     # ECS components/systems exposed to users
)
std = 20
```

**game/game.buildscript**:
```ini
[project:Game]
type = exe
sources = src/**/*.cpp
target_link_libraries(Engine)
std = 20
```

**Result:**
- Game links: Engine.lib
- Game gets includes: engine/src, 3rdparty/entt/src, 3rdparty/SDL3/include
- Game gets libs: SDL3.lib (via SDL3's public_libs)
- All transitive dependencies resolved automatically!

**Common Patterns:**

1. **SDL/Graphics Library Pattern** (INTERFACE):
```ini
[project:SDL3]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL3.lib  # External .lib provided

[project:Engine]
target_link_libraries(INTERFACE SDL3)  # Headers propagate, .lib propagates, but Engine doesn't link
```

2. **Header-Only ECS/Math Libraries** (PUBLIC):
```ini
[project:entt]
type = lib
headers = entt.hpp
public_includes = src

[project:Engine]
target_link_libraries(PUBLIC entt)  # Headers + functionality propagate to Engine users
```

3. **Internal Utilities** (PRIVATE):
```ini
[project:StringUtils]
type = lib
sources = src/*.cpp
public_includes = include

[project:Engine]
target_link_libraries(PRIVATE StringUtils)  # Used internally, not exposed
```

**Troubleshooting:**

| Problem | Likely Cause | Solution |
|---------|--------------|----------|
| "Unresolved external symbol" in game that uses Engine | Engine's dependency is PRIVATE but should be PUBLIC/INTERFACE | Change to PUBLIC or INTERFACE to propagate to dependents |
| Engine compiles but "cannot find SDL.h" | SDL3 dependency not adding includes to Engine | Ensure SDL3 has `public_includes` defined |
| Game compiles but "cannot find SDL.h" from Engine | Engine→SDL3 is PRIVATE (not propagating) | Change Engine→SDL3 from PRIVATE to INTERFACE or PUBLIC |
| Game links SDL twice (duplicate symbols) | Both Engine and Game link SDL as PUBLIC | Use INTERFACE on Engine's SDL dependency |
| PRIVATE dependency not adding includes (old bug) | Using outdated sighmake version | Update to latest version (PRIVATE now correctly adds includes locally) |

**Backward Compatibility:**

All dependencies without explicit visibility default to PUBLIC, maintaining existing behavior:

```ini
# Old syntax - still works (all dependencies are PUBLIC)
target_link_libraries(Engine, SDL, Utils)

# Equivalent to:
target_link_libraries(Engine PUBLIC SDL, Utils)
```

**When to Use Each Visibility:**

| Visibility | Use When | Example |
|------------|----------|---------|
| **PRIVATE** | Dependency used only in .cpp files (implementation) | Logger, profiler, internal utilities |
| **PUBLIC** | Dependency types appear in your public headers AND you link it | ECS library (entt), math library with .cpp files |
| **INTERFACE** | Dependency types appear in your public headers BUT you don't link it | Header-only libs, SDL wrapper that provides .lib separately |

**Decision Tree:**

1. **Does the dependency appear in your public headers?**
   - NO → Use **PRIVATE**
   - YES → Go to step 2

2. **Does your library need to link against it?**
   - YES → Use **PUBLIC**
   - NO → Use **INTERFACE** (header-only or externally linked)

**Real-World Examples:**

```ini
# Engine library with mixed dependencies
[project:Engine]
type = lib
target_link_libraries(
    # Header-only ECS - appears in Engine.h, but no .lib to link
    PUBLIC entt

    # SDL3 wrapper - SDL_Renderer* in Engine.h, but .lib linked separately
    INTERFACE SDL3

    # Logger - only used in Engine.cpp, not exposed
    PRIVATE spdlog

    # Profiler - only in debug builds, internal use
    PRIVATE tracy
)
```

**Best Practices:**

1. **Default to PRIVATE** - Start restrictive, expose only when needed
2. **PUBLIC sparingly** - Creates tight coupling between your users and dependencies
3. **INTERFACE for third-party wrappers** - Especially when you provide public_libs separately
4. **Test your visibility** - Build a project that depends on yours; if it fails to compile/link, you may need PUBLIC
```

### Finding External Packages: find_package()

Use `find_package()` to locate external SDKs and libraries on your system. This function searches for packages and sets variables that you can use in your build settings.

**Syntax:**
```ini
find_package(PackageName)
find_package(PackageName REQUIRED)
```

**Supported Packages:**

| Package | Windows | Linux |
|---------|---------|-------|
| `Vulkan` | Uses `VULKAN_SDK` environment variable | Uses pkg-config or standard paths |
| `OpenGL` | Always available (Windows SDK) | Uses pkg-config or standard paths |
| `SDL2` | Uses `SDL2_DIR` or `SDL2` environment variable | Uses pkg-config or standard paths |
| `SDL3` | Uses `SDL3_DIR` or `SDL3` environment variable | Uses pkg-config or standard paths |
| `DirectX9` or `DX9` | Uses `DXSDK_DIR` (DirectX SDK June 2010) | Not available |
| `DirectX10` or `DX10` | Uses `DXSDK_DIR` (DirectX SDK June 2010) | Not available |
| `DirectX11` | Always available (Windows SDK) | Not available |
| `DirectX12` | Always available (Windows 10+ SDK) | Not available |

**Variables Set:**

When a package is found, the following variables are set:

| Variable | Description |
|----------|-------------|
| `{Package}_FOUND` | `TRUE` if found, `FALSE` otherwise |
| `{Package}_INCLUDE_DIRS` | Include directories for the package |
| `{Package}_LIBRARIES` | Libraries to link against |
| `{Package}_LIBRARY_DIRS` | Library search directories (default x86 for DX9/DX10) |
| `{Package}_LIBRARY_DIRS_X86` | x86 library directories (DirectX9/DirectX10 only) |
| `{Package}_LIBRARY_DIRS_X64` | x64 library directories (DirectX9/DirectX10 only) |
| `{Package}_VERSION` | Package version (if detectable) |

**Basic Example:**
```ini
[project:VulkanApp]
type = exe

find_package(Vulkan REQUIRED)
find_package(SDL2)

sources = src/*.cpp
includes = include, ${Vulkan_INCLUDE_DIRS}, ${SDL2_INCLUDE_DIRS}
libs = ${Vulkan_LIBRARIES}, ${SDL2_LIBRARIES}
libdirs = ${Vulkan_LIBRARY_DIRS}, ${SDL2_LIBRARY_DIRS}
```

**REQUIRED Keyword:**

Adding `REQUIRED` makes the package mandatory. If not found, parsing will fail with an error:

```ini
# Will error if Vulkan SDK is not installed
find_package(Vulkan REQUIRED)

# Will warn but continue if SDL2 is not found
find_package(SDL2)
```

**Platform-Specific Setup:**

**Windows:**
Set environment variables pointing to SDK installations:
```batch
# Vulkan SDK (usually set by installer)
set VULKAN_SDK=C:\VulkanSDK\1.3.xxx

# SDL2 (manual setup)
set SDL2_DIR=C:\Libraries\SDL2-2.x.x

# SDL3 (manual setup)
set SDL3_DIR=C:\Libraries\SDL3

# DirectX SDK June 2010 (set by DXSDK_Jun10.exe installer)
# Required for DirectX 9 and DirectX 10
set DXSDK_DIR=C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)
```

**DirectX Notes:**
- **DirectX 9/10**: Require the legacy DirectX SDK (June 2010). Download and install `DXSDK_Jun10.exe` from Microsoft. The installer sets `DXSDK_DIR` automatically.
- **DirectX 11/12**: Included in the Windows SDK, no additional setup required.

**Linux:**
Install development packages:
```bash
# Vulkan
sudo apt install libvulkan-dev

# SDL2
sudo apt install libsdl2-dev

# SDL3
sudo apt install libsdl3-dev

# OpenGL
sudo apt install libgl1-mesa-dev
```

**Complete Example with Multiple Packages:**
```ini
[solution]
name = GameEngine
configurations = Debug, Release
platforms = x64

[project:Engine]
type = lib

# Find required packages
find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)

sources = src/**/*.cpp
headers = include/**/*.h

# Use package variables
includes = include, ${Vulkan_INCLUDE_DIRS}, ${SDL2_INCLUDE_DIRS}

# Platform-specific libraries
if(windows) {
    libs = ${Vulkan_LIBRARIES}, ${SDL2_LIBRARIES}
    libdirs = ${Vulkan_LIBRARY_DIRS}, ${SDL2_LIBRARY_DIRS}
}

if(linux) {
    libs = vulkan, SDL2
}

public_includes = include
public_libs = ${Vulkan_LIBRARIES}, ${SDL2_LIBRARIES}

[project:Game]
type = exe
sources = game/*.cpp
target_link_libraries(Engine)
subsystem = Windows
```

**DirectX Example (x86 target, e.g., Source Engine):**
```ini
[solution]
name = SourceMod
configurations = Debug, Release
platforms = Win32

[project:SourceMod]
type = dll

find_package(DirectX9 REQUIRED)

sources = src/*.cpp
includes = include, ${DirectX9_INCLUDE_DIRS}
libs = ${DirectX9_LIBRARIES}
libdirs = ${DirectX9_LIBRARY_DIRS_X86}
```

**DirectX Example (x64 target):**
```ini
[solution]
name = DX9Game
configurations = Debug, Release
platforms = x64

[project:DX9Game]
type = exe

find_package(DirectX9 REQUIRED)

sources = src/*.cpp
includes = include, ${DirectX9_INCLUDE_DIRS}
libs = ${DirectX9_LIBRARIES}
libdirs = ${DirectX9_LIBRARY_DIRS_X64}
subsystem = Windows
```

**Output Example:**

When parsing a buildscript with find_package, you'll see:
```
[find_package] Found Vulkan version 1.3.275
  Include dirs: C:\VulkanSDK\1.3.275.0\Include
  Libraries: vulkan-1.lib
  Library dirs: C:\VulkanSDK\1.3.275.0\Lib
[find_package] Found SDL2
  Include dirs: C:\Libraries\SDL2-2.30.0\include
  Libraries: SDL2.lib;SDL2main.lib
  Library dirs: C:\Libraries\SDL2-2.30.0\lib\x64
```

---

## 9. Precompiled Headers (PCH)

Precompiled headers dramatically reduce compile times by pre-compiling frequently used headers.

### How PCH Works

1. One source file **creates** the PCH (typically `pch.cpp`)
2. All other source files **use** the PCH
3. Third-party files can be excluded with **NotUsing**

### Basic PCH Setup

**Step 1: Create pch.h**
```cpp
// pch.h - Precompiled header file
#pragma once

// Standard library headers
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

// External library headers
#include <SDL2/SDL.h>

// Project headers (rarely change)
#include "core/types.h"
#include "core/macros.h"
```

**Step 2: Create pch.cpp**
```cpp
// pch.cpp - Creates the precompiled header
#include "pch.h"
```

**Step 3: Configure buildscript**
```ini
[project:MyProject]
type = exe
sources = src/**/*.cpp
headers = src/**/*.h
includes = src
std = 17

# Enable PCH for all files
pch = Use
pch_header = pch.h

# pch.cpp creates the PCH
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h
```

### PCH Settings Reference

| Setting | Description | Valid Values |
|---------|-------------|--------------|
| `pch` | PCH mode | `Use`, `Create`, `NotUsing` |
| `pch_header` | Header file to precompile | Filename (e.g., `pch.h`) |
| `pch_output` | Where to store PCH | Path (e.g., `obj/Debug/pch.pch`) |

**PCH Modes:**
- `Use` - Use an existing PCH (most files)
- `Create` - Create the PCH (one file only)
- `NotUsing` - Don't use PCH (third-party code)

### Per-Configuration PCH Output

Specify different PCH output paths per configuration:

```ini
[project:MyProject]
pch = Use
pch_header = src/pch.h

# Different PCH for each configuration
pch_output[Debug|Win32] = obj/Win32/Debug/pch.pch
pch_output[Release|Win32] = obj/Win32/Release/pch.pch
pch_output[Debug|x64] = obj/x64/Debug/pch.pch
pch_output[Release|x64] = obj/x64/Release/pch.pch

# Create PCH
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h
```

### Excluding Files from PCH

Third-party libraries often don't work with PCH:

```ini
[project:Engine]
sources = src/**/*.cpp
pch = Use
pch_header = pch.h

# Create PCH
src/pch.cpp:pch = Create

# Exclude third-party code
src/external/pugixml.cpp:pch = NotUsing
src/external/stb_image.cpp:pch = NotUsing
src/external/miniz.cpp:pch = NotUsing
```

### Complete PCH Example

**Directory structure:**
```
project/
├── src/
│   ├── pch.h
│   ├── pch.cpp
│   ├── main.cpp
│   ├── engine/
│   │   ├── renderer.cpp
│   │   └── physics.cpp
│   └── external/
│       └── stb_image.cpp
└── project.buildscript
```

**project.buildscript:**
```ini
[solution]
name = GameEngine
configurations = Debug, Release
platforms = x64

[project:Engine]
type = lib
sources = src/**/*.cpp
headers = src/**/*.h
includes = src
std = 20

# All files use PCH by default
pch = Use
pch_header = pch.h

# Per-configuration PCH output
pch_output[Debug|x64] = build/obj/x64/Debug/pch.pch
pch_output[Release|x64] = build/obj/x64/Release/pch.pch

# pch.cpp creates the PCH
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h

# Exclude third-party code from PCH
src/external/stb_image.cpp:pch = NotUsing

# Output directories
outdir[Debug|x64] = build/bin/x64/Debug
outdir[Release|x64] = build/bin/x64/Release
intdir[Debug|x64] = build/obj/x64/Debug
intdir[Release|x64] = build/obj/x64/Release
```

### PCH Best Practices

1. **Include stable headers**: Put rarely-changed headers in PCH
2. **Exclude volatile headers**: Don't put frequently-modified headers in PCH
3. **One PCH per project**: Each project should have its own PCH
4. **Include PCH first**: `#include "pch.h"` must be first in every .cpp file
5. **Exclude third-party code**: Use `NotUsing` for external libraries
6. **Per-config output paths**: Avoid PCH conflicts between configurations

### What to Put in PCH

**Good candidates:**
- Standard library headers (`<iostream>`, `<vector>`, etc.)
- External library headers (`<SDL2/SDL.h>`, etc.)
- Stable project headers (types, macros, constants)

**Bad candidates:**
- Frequently modified headers
- Template-heavy headers (already fast to compile)
- Headers with many dependencies
- Platform-specific headers (if targeting multiple platforms)

### Troubleshooting PCH

**Problem: "cannot open precompiled header file"**
- Ensure `pch.cpp` has `pch = Create`
- Check `pch_output` path exists
- Verify `pch_header` path is correct

**Problem: "PCH file was built with different compiler"**
- Clean and rebuild
- Ensure all configurations use separate PCH output paths

**Problem: Compilation is slower with PCH**
- Check what's in pch.h (maybe too much?)
- Ensure PCH is actually being used (check compiler output)

---

## 10. Wildcard Patterns

sighmake supports glob patterns for automatically including source files.

### Basic Wildcards

**Single directory:**
```ini
sources = src/*.cpp
headers = include/*.h
```

Matches all `.cpp` files in `src/` directory (non-recursive).

**Recursive wildcard:**
```ini
sources = src/**/*.cpp
headers = include/**/*.h
```

Matches all `.cpp` files in `src/` and all subdirectories.

### Wildcard Syntax

| Pattern | Description | Example |
|---------|-------------|---------|
| `*` | Match any characters except `/` | `*.cpp` matches `main.cpp` |
| `**` | Match any directories recursively | `**/*.cpp` matches all .cpp files |
| `?` | Match single character | `file?.cpp` matches `file1.cpp`, `file2.cpp` |

### Multiple Patterns

Use commas to specify multiple patterns:

```ini
sources = src/*.cpp, external/lib/*.cpp, main.cpp
headers = include/**/*.h, src/**/*.hpp
```

### Practical Examples

**1. Simple project structure:**
```
project/
├── main.cpp
├── utils.cpp
└── project.buildscript
```

```ini
sources = *.cpp
```

**2. Source/include split:**
```
project/
├── src/
│   ├── main.cpp
│   └── utils.cpp
├── include/
│   └── utils.h
└── project.buildscript
```

```ini
sources = src/*.cpp
headers = include/*.h
```

**3. Deep directory structure:**
```
project/
├── src/
│   ├── core/
│   │   ├── engine.cpp
│   │   └── renderer.cpp
│   ├── game/
│   │   ├── player.cpp
│   │   └── enemy.cpp
│   └── main.cpp
└── project.buildscript
```

```ini
sources = src/**/*.cpp
headers = src/**/*.h
```

**4. Multiple source directories:**
```
project/
├── engine/
│   └── *.cpp
├── game/
│   └── *.cpp
├── external/
│   └── *.cpp
└── project.buildscript
```

```ini
sources = engine/*.cpp, game/*.cpp, external/*.cpp
```

**5. Mixed explicit and wildcard:**
```ini
# Main source tree
sources = src/**/*.cpp

# Specific external files
sources = src/**/*.cpp, external/glad/glad.c, external/imgui/*.cpp
```

### Wildcard Best Practices

1. **Use `**` for organized projects**: If you have subdirectories, use `**/*.cpp`
2. **Separate by concern**: Use multiple patterns for different areas
3. **Be specific when needed**: Combine wildcards with explicit files
4. **Headers are optional**: Only needed for IDE organization
5. **Test your patterns**: Generate projects and check what files are included

### Wildcard Gotchas

**Problem: Too many files matched**
```ini
# Matches ALL .cpp files including unwanted ones
sources = **/*.cpp
```

**Solution: Be more specific**
```ini
# Only match in src/
sources = src/**/*.cpp
```

**Problem: Missing files**
```ini
# Only matches src/ directory, not subdirectories
sources = src/*.cpp
```

**Solution: Use recursive wildcard**
```ini
sources = src/**/*.cpp
```

### Paths and Wildcards

- Paths are relative to the buildscript location
- Use forward slashes (`/`) even on Windows
- Wildcards work with per-file settings:

```ini
sources = src/**/*.cpp
pch = Use

# Exclude specific matched files from PCH
src/external/pugixml.cpp:pch = NotUsing
```

### Advanced Patterns

**Organize by subdirectory:**
```ini
# Core engine files
sources = src/core/**/*.cpp

# Platform-specific files
sources[Win32] = src/core/**/*.cpp, src/platform/windows/*.cpp
sources[Linux] = src/core/**/*.cpp, src/platform/linux/*.cpp
```

**Different wildcards per configuration:**
```ini
# Debug: include debug utilities
sources[Debug] = src/**/*.cpp, debug/*.cpp

# Release: exclude debug utilities
sources[Release] = src/**/*.cpp
```

---

## 11. Generators

Generators determine what type of build files sighmake creates.

### vcxproj Generator (Visual Studio)

Generates Visual Studio project files.

**Usage:**
```batch
sighmake project.buildscript -g vcxproj
```

**Generated files:**
- `ProjectName_.vcxproj` - Project file
- `SolutionName_.sln` - Solution file
- `SolutionName_.slnx` - Visual Studio 2022+ XML solution file

**Default on:** Windows

**Supported toolsets:**
- msvc2026, msvc2022, msvc2019, msvc2017
- msvc2015, msvc2013, msvc2012, msvc2010

**Example:**
```batch
# Generate Visual Studio 2022 project
sighmake myproject.buildscript -g vcxproj -t msvc2022

# Open generated solution
start MyProject_.sln
```

**Building with MSBuild:**
```batch
# Build specific configuration
msbuild MyProject_.sln /p:Configuration=Release /p:Platform=x64

# Build all configurations
msbuild MyProject_.sln /t:Rebuild
```

**Features:**
- Full Visual Studio IDE integration
- IntelliSense support
- Debugging support
- Per-configuration and per-platform settings
- Project dependencies
- Precompiled header support

### makefile Generator

Generates GNU Makefiles.

**Usage:**
```bash
sighmake project.buildscript -g makefile
```

**Generated files:**
- `build/ProjectName.Debug` - Debug configuration Makefile
- `build/ProjectName.Release` - Release configuration Makefile
- One Makefile per project per configuration

**Default on:** Linux

**Example:**
```bash
# Generate Makefiles
./sighmake myproject.buildscript -g makefile

# Build Debug configuration
make -f build/MyProject.Debug

# Build Release configuration
make -f build/MyProject.Release

# Clean
make -f build/MyProject.Release clean
```

**Building with make:**
```bash
# Build with default jobs
make -f build/MyProject.Release

# Build with 8 parallel jobs
make -f build/MyProject.Release -j8

# Verbose output
make -f build/MyProject.Release VERBOSE=1
```

**Features:**
- GCC and Clang support
- Parallel builds with `-j`
- Dependency tracking
- Clean target
- Cross-platform compatible

### Listing Generators

```bash
sighmake --list
```

Output:
```
Available generators:
  vcxproj  - Visual Studio Project Generator
  makefile - GNU Makefile Generator
```

### Generator Selection

**Automatic (recommended):**
```batch
# On Windows, generates vcxproj
sighmake project.buildscript

# On Linux, generates makefile
./sighmake project.buildscript
```

**Explicit:**
```batch
# Force vcxproj on any platform
sighmake project.buildscript -g vcxproj

# Force makefile on any platform
sighmake project.buildscript -g makefile
```

### Cross-Generator Compatibility

Buildscripts are compatible across generators:

```bash
# Developer on Windows
sighmake project.buildscript -g vcxproj
# Opens in Visual Studio

# Developer on Linux (same buildscript)
./sighmake project.buildscript -g makefile
make -f build/MyProject.Release
```

### Platform-Specific Settings in Generators

**Visual Studio:**
- Supports Win32 and x64 platforms
- Configurations stored in project file
- Build all from IDE or MSBuild

**Makefile:**
- Separate Makefile per configuration
- Platform determined by compiler
- Build one configuration at a time

### Generator Comparison

| Feature | vcxproj | makefile |
|---------|---------|----------|
| Platform | Windows | Linux (cross-platform) |
| IDE | Visual Studio | Any (VSCode, Vim, etc.) |
| Build tool | MSBuild | GNU Make |
| Parallel builds | Automatic | `-j` flag |
| Debugging | Integrated | GDB/LLDB |
| Configurations | Multi-config | Separate files |
| Default on | Windows | Linux |

---

## 12. Toolsets

Toolsets specify which version of Visual Studio to target when generating vcxproj files.

### Available Toolsets

| Toolset | Visual Studio Version | Platform Toolset |
|---------|----------------------|------------------|
| `msvc2026` | Visual Studio 2026 | v143+ |
| `msvc2022` | Visual Studio 2022 | v143 |
| `msvc2019` | Visual Studio 2019 | v142 |
| `msvc2017` | Visual Studio 2017 | v141 |
| `msvc2015` | Visual Studio 2015 | v140 |
| `msvc2013` | Visual Studio 2013 | v120 |
| `msvc2012` | Visual Studio 2012 | v110 |
| `msvc2010` | Visual Studio 2010 | v100 |

**Default:** `msvc2022`

### Listing Toolsets

```batch
sighmake --list-toolsets
```

Output:
```
Available toolsets:
  msvc2026  Visual Studio 2026
  msvc2022  Visual Studio 2022 (default)
  msvc2019  Visual Studio 2019
  msvc2017  Visual Studio 2017
  msvc2015  Visual Studio 2015
  msvc2013  Visual Studio 2013
  msvc2012  Visual Studio 2012
  msvc2010  Visual Studio 2010
```

### Specifying Toolset

**Command-line option:**
```batch
sighmake project.buildscript -t msvc2019
```

**Environment variable:**
```batch
# Windows
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022
sighmake project.buildscript

# Linux/macOS
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
./sighmake project.buildscript
```

**Precedence:**
1. Command-line option (`-t`)
2. Environment variable (`SIGHMAKE_DEFAULT_TOOLSET`)
3. Default (`msvc2022`)

### Team Environments

Set a team-wide default using environment variables:

**Windows batch script (setup.bat):**
```batch
@echo off
set SIGHMAKE_DEFAULT_TOOLSET=msvc2022
echo Toolset set to %SIGHMAKE_DEFAULT_TOOLSET%
```

**Linux/macOS script (setup.sh):**
```bash
#!/bin/bash
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
echo "Toolset set to $SIGHMAKE_DEFAULT_TOOLSET"
```

**Add to shell profile (.bashrc, .zshrc):**
```bash
export SIGHMAKE_DEFAULT_TOOLSET=msvc2022
```

### Toolset Detection

sighmake can detect installed Visual Studio versions:

```cpp
// Automatic detection when opening in Visual Studio
// VS2022 will use v143, VS2019 will use v142, etc.
```

However, explicitly specifying ensures consistency across team members.

### C++ Standard Support by Toolset

| Toolset | C++14 | C++17 | C++20 | C++23 |
|---------|-------|-------|-------|-------|
| msvc2026 | ✓ | ✓ | ✓ | ✓ |
| msvc2022 | ✓ | ✓ | ✓ | Partial |
| msvc2019 | ✓ | ✓ | Partial | ✗ |
| msvc2017 | ✓ | Partial | ✗ | ✗ |
| msvc2015 | Partial | ✗ | ✗ | ✗ |
| msvc2013 | ✗ | ✗ | ✗ | ✗ |

**Example:**
```ini
[project:ModernCpp]
std = 20
```

Use `msvc2019` or newer for full C++20 support.

**C++ Standard Validation:**

Sighmake validates C++ standard values and ensures compatibility with VS 2022+:

| Value | Standard | MSVC Flag | Notes |
|-------|----------|-----------|-------|
| `14` | C++14 | `stdcpp14` | ✓ Supported (minimum for VS 2022+) |
| `17` | C++17 | `stdcpp17` | ✓ Supported |
| `20` | C++20 | `stdcpp20` | ✓ Supported |
| `23` | C++23 | `stdcpp23` | ✓ Supported (VS 2022 v17.8+) |
| `latest` | Latest | `stdcpplatest` | ✓ Supported |
| `11` | C++11 | N/A | ⚠️ Not supported (falls back to C++14 with warning) |
| `03`, `98` | C++03/98 | N/A | ⚠️ Not supported (falls back to C++14 with warning) |

**Automatic Fallback:**
```
Warning: C++11 is not supported by VS 2022+ (minimum is C++14). Falling back to stdcpp14.
```

- Invalid standards automatically fall back to the nearest supported version
- The build continues successfully with the fallback standard
- To suppress warnings, use a supported standard value (14, 17, 20, 23, or latest)

### C Language Projects

Sighmake supports pure C projects with C-specific standards and compilation settings. This enables using C libraries like GLAD (OpenGL loader) as dependencies.

#### Declaring a C Project

Use the `language` property to explicitly declare a project as C:

```ini
[project:GLAD]
type = lib
language = C           # Declare project as C
c_standard = 99        # C standard: 89, 99, 11, 17, 23
sources = src/glad.c
headers = include/**/*.h
public_includes = include
```

#### Auto-Detection

If `language` is not specified, sighmake auto-detects based on file extensions:
- **Only `.c` files** → Detected as C project
- **Any `.cpp`/`.cc`/`.cxx` files** → Detected as C++ project
- **Mixed** → Defaults to C++ (can compile both)

**Example (auto-detected as C):**
```ini
[project:MyLib]
type = lib
c_standard = 11
sources = src/*.c      # Only .c files → Auto-detected as C
headers = include/*.h
```

#### C Standards Supported

| Value | Standard | MSVC Flag | GCC/Clang Flag | Notes |
|-------|----------|-----------|----------------|-------|
| `89` | C89/C90 | `stdc89` | `-std=c89` | ✓ Supported |
| `99` | C99 | N/A | `-std=c99` | ⚠️ MSVC: Falls back to C11 with warning |
| `11` | C11 | `stdc11` | `-std=c11` | ✓ Supported |
| `17` | C17 | N/A | `-std=c17` | ⚠️ MSVC: Falls back to C11 with warning |
| `23` | C23 | N/A | `-std=c2x` | ⚠️ MSVC: Falls back to C11 with warning |

**MSVC Support:**
- ✓ Fully supported: C89 (`stdc89`), C11 (`stdc11`)
- ⚠️ Not supported: C99, C17, C23 (falls back to C11 with warning)
- Sighmake will automatically fall back to the nearest supported standard and emit a warning

#### Example: GLAD (OpenGL Loader)

GLAD is a pure C library commonly used for OpenGL:

```ini
# glad.buildscript
[project:GLAD]
type = lib
language = C
c_standard = 99
sources = glad/src/glad.c
headers = glad/include/**/*.h
public_includes = glad/include

# Using GLAD in your C++ project
[project:MyGame]
type = exe
language = C++
std = 20
sources = src/**/*.cpp
target_link_libraries(
    GLAD PRIVATE          # GLAD compiled as C, linked into C++ project
    SDL3 INTERFACE
)
```

#### Mixed C/C++ Projects

You can have both C and C++ files in the same project:

```ini
[project:MixedLib]
type = lib
# language auto-detected as C++ (safest for mixed)
sources =
    src/legacy.c      # Compiled as C
    src/modern.cpp    # Compiled as C++
headers = include/**/*.h
```

**How it works:**
- MSVC: `.c` files get `<CompileAs>CompileAsC</CompileAs>`, `.cpp` files get `CompileAsCpp`
- GCC/Clang: Uses `g++` compiler (can handle both C and C++)

**Per-file override (if needed):**
```ini
# Force specific file to compile as C
legacy.c:compile_as = CompileAsC
modern.cpp:compile_as = CompileAsCpp
```

#### Compiler Selection

Sighmake selects the appropriate compiler based on detected language:

**Windows (MSVC):**
- All projects use `cl.exe`
- C projects: `/TC` flag (compile as C)
- C++ projects: `/TP` flag (compile as C++)

**Linux/macOS (Makefile):**
- **C projects**: Uses `gcc` (or `$CC` environment variable)
- **C++ projects**: Uses `g++` (or `$CXX` environment variable)
- **Mixed projects**: Uses `g++` (can compile both)

**Example Makefile output for C project:**
```makefile
CC = gcc
CFLAGS = -std=c99 -O3 -Wall
```

**Example Makefile output for C++ project:**
```makefile
CXX = g++
CXXFLAGS = -std=c++20 -O3 -Wall
```

#### Real-World Example: SnakeGame with GLAD

Directory structure:
```
snakegame/
├── engine/
│   ├── 3rdparty/
│   │   ├── glad/
│   │   │   ├── include/
│   │   │   │   └── glad/glad.h
│   │   │   └── src/
│   │   │       └── glad.c
│   │   └── glad.buildscript      # C library
│   └── engine.buildscript         # C++ library using GLAD
└── game/
    └── game.buildscript           # C++ executable
```

**3rdparty/glad.buildscript:**
```ini
[project:GLAD]
type = lib
language = C
c_standard = 99
sources = glad/src/glad.c
headers = glad/include/**/*.h
public_includes = glad/include
```

**engine/engine.buildscript:**
```ini
include = 3rdparty/glad.buildscript

[project:Engine]
type = lib
language = C++
std = 20
sources = src/**/*.cpp
headers = src/**/*.h
public_includes = src
target_link_libraries(
    INTERFACE SDL3       # Headers only
    PUBLIC entt          # Header-only ECS
    PRIVATE GLAD         # C library (implementation detail)
)
```

**game/game.buildscript:**
```ini
include = ../engine/engine.buildscript

[project:SnakeGame]
type = exe
language = C++
std = 20
sources = src/**/*.cpp
target_link_libraries(
    PRIVATE Engine
)
```

Result:
- GLAD compiles as pure C with `-std=c99` (gcc) or default C (MSVC)
- Engine compiles as C++ with `-std=c++20`, links GLAD.lib
- SnakeGame compiles as C++, links Engine.lib (transitively includes GLAD)

#### When to Use `language = C`

**Use explicit `language = C` when:**
1. Building pure C libraries (GLAD, stb_image, etc.)
2. Enforcing C-only compilation for strict C99/C11 compatibility
3. Ensuring the project uses `gcc` instead of `g++` in Makefiles

**Auto-detection works when:**
1. File extensions clearly indicate language (.c only → C, .cpp present → C++)
2. Default compiler behavior is acceptable

#### Troubleshooting C Projects

**Problem: "C standard not supported"**
```
Warning: C99 (stdc99) is not fully supported by MSVC. Falling back to stdc11.
```

**What happens:**
- Sighmake automatically falls back to the nearest supported C standard (C11)
- The build continues successfully with the fallback standard
- You'll see a warning message, but the project will still compile

**Solution:**
- For C99 code on MSVC: The automatic fallback to C11 usually works fine
- If you need strict C99 behavior: Use GCC/Clang, or explicitly set `c_standard = 11`
- To suppress the warning: Change `c_standard = 99` to `c_standard = 11` in your buildscript

**Problem: ".c files compiling as C++"**

**Solution:**
```ini
# Add explicit language declaration
language = C
```

This forces all `.c` files to compile with C compiler and C standards.

**Problem: "Mixed C/C++ linking errors"**

**Solution:**
- Ensure C headers use `extern "C"` guards:
```c
#ifdef __cplusplus
extern "C" {
#endif

// C function declarations here

#ifdef __cplusplus
}
#endif
```

### Toolset Examples

**Target Visual Studio 2019:**
```batch
sighmake project.buildscript -t msvc2019
```

**Target Visual Studio 2017 for compatibility:**
```batch
sighmake project.buildscript -t msvc2017
```

**Use latest toolset:**
```batch
sighmake project.buildscript -t msvc2026
```

### Toolset Best Practices

1. **Set team default**: Use environment variable or batch script
2. **Document requirements**: Specify minimum VS version in README
3. **Test compatibility**: If targeting older VS, test on that version
4. **Use modern toolset**: Prefer newer toolsets for better standards support
5. **CI/CD consistency**: Set toolset explicitly in build scripts

### Troubleshooting Toolsets

**Problem: "Toolset not found"**
```
Error: Toolset 'msvc2019' not found
```

**Solutions:**
- Install the specified Visual Studio version
- Use `--list-toolsets` to see available options
- Change to an installed toolset

**Problem: "Unsupported C++ standard"**
```
Error: C++20 not supported by toolset 'msvc2015'
```

**Solutions:**
- Use a newer toolset (msvc2019+)
- Lower the C++ standard requirement (`std = 17`)

---

## 13. Converting from Visual Studio

sighmake can convert existing Visual Studio solutions to buildscripts.

### Basic Conversion

**Convert a solution:**
```batch
sighmake --convert MySolution.sln
```

**Generated files:**
- `MySolution.buildscript` - Solution-level settings
- `Project1.buildscript` - Settings for Project1
- `Project2.buildscript` - Settings for Project2
- ... (one per project)

### What Gets Converted

**Solution-level:**
- Solution name
- Configurations (Debug, Release, etc.)
- Platforms (Win32, x64, etc.)

**Project-level:**
- Project type (exe, lib, dll)
- Source files
- Header files
- Include directories
- Preprocessor defines
- C++ standard
- Compiler settings (optimization, runtime library, etc.)
- Linker settings (subsystem, etc.)
- Output directories
- Project dependencies

### Conversion Example

**Before: MySolution.sln**
```xml
<!-- Complex XML structure -->
```

**After: MySolution.buildscript**
```ini
[solution]
name = MySolution
configurations = Debug, Release
platforms = Win32, x64
```

**After: MyProject.buildscript**
```ini
[project:MyProject]
type = exe
sources = main.cpp, utils.cpp, engine.cpp
headers = utils.h, engine.h
includes = include, external/include
defines = WIN32, _WINDOWS, _CRT_SECURE_NO_WARNINGS
std = 17

outdir[Debug|Win32] = bin/Win32/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release

optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
runtime_library[Debug] = MultiThreadedDebugDLL
runtime_library[Release] = MultiThreadedDLL

subsystem = Console
```

### Post-Conversion Steps

1. **Review generated buildscripts**: Check accuracy of conversion
2. **Simplify with wildcards**: Replace explicit file lists with wildcards
3. **Extract common settings**: Use `include` for shared settings
4. **Add comments**: Document settings for team members
5. **Test**: Generate back to vcxproj and build

**Example simplification:**

**Generated (verbose):**
```ini
sources = main.cpp, utils.cpp, renderer.cpp, physics.cpp, audio.cpp
```

**Simplified (wildcards):**
```ini
sources = src/**/*.cpp
```

### Migration Workflow

**Step 1: Convert**
```batch
sighmake --convert MyProject.sln
```

**Step 2: Review and simplify**
Edit the generated `.buildscript` files.

**Step 3: Test**
```batch
# Generate vcxproj from buildscript
sighmake MyProject.buildscript

# Build and test
msbuild MyProject_.sln /p:Configuration=Release
```

**Step 4: Commit buildscripts**
```batch
git add *.buildscript
git commit -m "Migrate to sighmake buildscripts"
```

**Step 5: Update .gitignore**
```
# Ignore generated files
*.vcxproj
*.vcxproj.filters
*.vcxproj.user
*.sln
*.slnx
```

**Step 6: Update build scripts**
```batch
@echo off
echo Generating Visual Studio solution...
sighmake MyProject.buildscript
echo Done! Open MyProject_.sln in Visual Studio
```

### Full Migration Example

**Before: Legacy Visual Studio setup**
```
project/
├── MyProject.sln (in version control, causes merge conflicts)
├── MyProject.vcxproj (in version control)
└── src/
    └── *.cpp
```

**After: sighmake setup**
```
project/
├── .gitignore (ignore generated files)
├── generate.bat (regenerates solution)
├── MyProject.buildscript (in version control)
├── common_settings.buildscript (shared settings)
└── src/
    └── *.cpp
```

**generate.bat:**
```batch
@echo off
sighmake MyProject.buildscript
echo.
echo Solution generated: MyProject_.sln
pause
```

**.gitignore:**
```
# Generated Visual Studio files
*.vcxproj
*.vcxproj.filters
*.vcxproj.user
*.sln
*.slnx

# Build outputs
bin/
obj/
```

### Conversion Limitations

**Not fully converted:**
- Custom build steps
- Pre/post-build events
- NuGet packages
- External tool integrations
- Advanced MSBuild customizations

**Workaround:** Add these manually to generated vcxproj or use external scripts.

### Benefits of Migration

1. **Version control friendly**: No more XML merge conflicts
2. **Human readable**: Easy to review changes in pull requests
3. **Cross-platform**: Generate Makefiles for Linux
4. **Automation friendly**: Integrate into scripts and CI/CD
5. **Simpler maintenance**: Edit INI files instead of XML

---

## 14. Cross-Platform Development

sighmake makes it easy to maintain a single buildscript for Windows and Linux.

### Cross-Platform Buildscript

**Basic example:**
```ini
[solution]
name = CrossPlatformApp
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17

# Compiler settings (work on both MSVC and GCC/Clang)
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed

# Windows-specific
subsystem = Console
```

**Windows workflow:**
```batch
# Generate Visual Studio project
sighmake project.buildscript -g vcxproj

# Build with MSBuild
msbuild MyApp_.sln /p:Configuration=Release /p:Platform=x64
```

**Linux workflow:**
```bash
# Generate Makefile
./sighmake project.buildscript -g makefile

# Build with make
make -f build/MyApp.Release -j8
```

### Platform-Specific Files

Use configuration-specific sources for platform-specific code:

```ini
[project:Engine]
type = lib
includes = include

# Common cross-platform files
sources = src/core/**/*.cpp

# Windows-specific files
sources[Win32] = src/core/**/*.cpp, src/platform/windows/**/*.cpp
sources[x64] = src/core/**/*.cpp, src/platform/windows/**/*.cpp
```

**Directory structure:**
```
project/
├── src/
│   ├── core/               (cross-platform)
│   │   ├── engine.cpp
│   │   └── renderer.cpp
│   └── platform/
│       ├── windows/        (Windows-specific)
│       │   └── file_io.cpp
│       └── linux/          (Linux-specific)
│           └── file_io.cpp
└── project.buildscript
```

### Platform-Specific Source Files (Inline Conditions)

You can conditionally include or exclude source files based on the target platform using inline conditions.

**Syntax:**
```ini
sources = {
    src/**/*.cpp
    src/platform/win_impl.cpp [windows]
    src/platform/linux_impl.cpp [linux]
}
```

**Supported conditions:**
- `[windows]` or `[win32]` - Include only on Windows
- `[linux]` - Include only on Linux
- `[osx]`, `[macos]`, or `[darwin]` - Include only on macOS
- `[!windows]` - Include on everything except Windows
- `[!linux]` - Include on everything except Linux
- `[!osx]` - Include on everything except macOS

**Override behavior:**
When a file matches both a wildcard pattern AND has an explicit entry with a condition, the explicit condition takes precedence. This allows you to:

1. Include most files via wildcard
2. Restrict specific files to certain platforms

**Example - Windows-only D3D11 files:**
```ini
[project:Renderer]
sources = {
    src/**/*.cpp
    src/Graphics/D3D11RenderAPI.cpp [windows]
    src/Graphics/D3D11Mesh.cpp [windows]
}
```

In this example:
- `src/**/*.cpp` matches all .cpp files including the D3D11 files
- BUT `D3D11RenderAPI.cpp` and `D3D11Mesh.cpp` have explicit `[windows]` conditions
- On Windows: All files are included (D3D11 files match the condition)
- On Linux: D3D11 files are excluded (condition not met), other files are included

**Example - Cross-platform with platform-specific implementations:**
```ini
[project:AudioSystem]
sources = {
    src/**/*.cpp
    src/audio/wasapi_audio.cpp [windows]
    src/audio/alsa_audio.cpp [linux]
    src/audio/coreaudio_audio.cpp [osx]
}
```

**Multi-line brace syntax:**
For better readability, use the brace block syntax to list sources across multiple lines:
```ini
sources = {
    src/core/*.cpp
    src/utils/*.cpp
    src/platform/common.cpp
    src/platform/win32_impl.cpp [windows]
    src/platform/posix_impl.cpp [!windows]
}
```

This is equivalent to the comma-separated single-line format:
```ini
sources = src/core/*.cpp, src/utils/*.cpp, src/platform/common.cpp, src/platform/win32_impl.cpp [windows], src/platform/posix_impl.cpp [!windows]
```

**Behavior per platform:**

| File | Condition | Windows | Linux | macOS |
|------|-----------|---------|-------|-------|
| `common.cpp` | (none) | Included | Included | Included |
| `win32_impl.cpp` | `[windows]` | Included | Excluded | Excluded |
| `posix_impl.cpp` | `[!windows]` | Excluded | Included | Included |
| `linux_only.cpp` | `[linux]` | Excluded | Included | Excluded |
| `mac_only.cpp` | `[osx]` | Excluded | Excluded | Included |

**Note:** Inline conditions also work with `headers` and `resources`:
```ini
headers = {
    include/**/*.h
    include/win32_api.h [windows]
}

resources = {
    res/*.rc
    res/windows_app.rc [windows]
}
```

### Platform-Specific Defines

**Using bracket notation:**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp

# Common defines
defines = MY_APP

# Platform-specific defines
defines[Win32] = WIN32, _WINDOWS
defines[x64] = WIN64, _WIN64
```

**Using conditional blocks (alternative):**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp

# Common defines
defines = MY_APP

# Windows-specific defines
if(Windows)
{
    defines = WIN32, _WINDOWS
}
```

**In code:**
```cpp
#ifdef WIN32
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif
```

### Platform-Specific Libraries

```ini
[project:MyGame]
type = exe
sources = src/**/*.cpp

# Windows libraries
libs[Win32] = user32.lib, gdi32.lib, opengl32.lib
libs[x64] = user32.lib, gdi32.lib, opengl32.lib

# Linux libraries (if sighmake supported on Linux with libs setting)
# Would be handled by makefile generator
```

### Cross-Platform Settings

These settings work across platforms:

| Setting | MSVC (Windows) | GCC/Clang (Linux) |
|---------|----------------|-------------------|
| `std` | `/std:c++17` | `-std=c++17` |
| `optimization = Disabled` | `/Od` | `-O0` |
| `optimization = MaxSpeed` | `/O2` | `-O3` |
| `defines` | `/D DEFINE` | `-DDEFINE` |
| `includes` | `/I path` | `-Ipath` |

sighmake translates settings to platform-appropriate compiler flags.

### Complete Cross-Platform Example

**project.buildscript:**
```ini
[solution]
name = CrossPlatformEngine
configurations = Debug, Release
platforms = Win32, x64

[project:Engine]
type = lib
std = 17

# Include directories (cross-platform)
includes = include, external/include

# Common source files
sources = src/core/**/*.cpp, src/renderer/**/*.cpp

# Platform-specific sources
sources[Win32] = src/core/**/*.cpp, src/renderer/**/*.cpp, src/platform/windows/*.cpp
sources[x64] = src/core/**/*.cpp, src/renderer/**/*.cpp, src/platform/windows/*.cpp

# Common defines
defines = ENGINE_VERSION=1.0

# Platform-specific defines
defines[Win32] = WIN32, _WINDOWS
defines[x64] = WIN64, _WIN64

# Compiler settings
warning_level = Level3
multiprocessor = true
exception_handling = Sync

# Configuration-specific settings
optimization[Debug|Win32] = Disabled
optimization[Debug|x64] = Disabled
optimization[Release|Win32] = MaxSpeed
optimization[Release|x64] = MaxSpeed

runtime_library[Debug|Win32] = MultiThreadedDebug
runtime_library[Debug|x64] = MultiThreadedDebug
runtime_library[Release|Win32] = MultiThreaded
runtime_library[Release|x64] = MultiThreaded

# Output directories
outdir[Debug|Win32] = bin/Win32/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release
```

**Windows developer:**
```batch
sighmake project.buildscript
# Opens in Visual Studio
```

**Linux developer (same buildscript):**
```bash
./sighmake project.buildscript -g makefile
make -f build/Engine.Release -j8
```

### Cross-Platform Best Practices

1. **Separate platform code**: Use `src/platform/windows/` and `src/platform/linux/`
2. **Abstract platform APIs**: Create common interfaces for platform-specific code
3. **Use platform defines**: `WIN32`, `__linux__`, `__APPLE__`
4. **Test on all platforms**: Build and test on Windows and Linux regularly
5. **Shared buildscript**: Keep one buildscript, use configuration-specific settings
6. **Forward slashes in paths**: Use `/` not `\` for cross-platform compatibility
7. **Portable libraries**: Prefer cross-platform libraries (SDL, GLFW, etc.)

### Common Cross-Platform Patterns

**1. Platform abstraction:**
```cpp
// platform.h
#ifdef _WIN32
    #include "platform/windows/platform_windows.h"
#elif defined(__linux__)
    #include "platform/linux/platform_linux.h"
#endif
```

**2. Conditional compilation:**
```cpp
void InitializeWindow() {
#ifdef _WIN32
    // Windows-specific code
    HWND hwnd = CreateWindow(...);
#elif defined(__linux__)
    // Linux-specific code
    Display* display = XOpenDisplay(NULL);
#endif
}
```

**3. Platform-specific entry points:**
```cpp
// main.cpp (cross-platform)
int Main(int argc, char** argv) {
    // Cross-platform application code
}

// main_windows.cpp (Windows-specific)
#ifdef _WIN32
int WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    return Main(__argc, __argv);
}
#endif

// main_linux.cpp (Linux-specific)
#ifndef _WIN32
int main(int argc, char** argv) {
    return Main(argc, argv);
}
#endif
```

---

## 15. CMake Integration

sighmake can parse basic CMakeLists.txt files and generate project files.

### Basic Usage

```batch
# Generate Visual Studio project from CMake
sighmake CMakeLists.txt -g vcxproj

# Generate Makefile from CMake
sighmake CMakeLists.txt -g makefile
```

### Supported CMake Features

sighmake supports basic CMake commands:

**Project definition:**
```cmake
project(MyProject)
```

**Executable:**
```cmake
add_executable(MyApp
    src/main.cpp
    src/utils.cpp
)
```

**Library:**
```cmake
add_library(MyLib STATIC
    src/lib.cpp
    include/lib.h
)
```

**Include directories:**
```cmake
target_include_directories(MyApp PRIVATE
    include
    external/include
)
```

**Link libraries:**
```cmake
target_link_libraries(MyApp PRIVATE
    MyLib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/lib/external.lib
)
```

**C++ standard:**
```cmake
set_property(TARGET MyApp PROPERTY CXX_STANDARD 17)
```

### Example CMake Project

**CMakeLists.txt:**
```cmake
cmake_minimum_required(VERSION 3.10)
project(AudioEngine)

# Static library
add_library(AudioEngine STATIC
    src/audio_engine.cpp
    src/audio_source.cpp
    include/audio_engine.h
    include/audio_source.h
)

target_include_directories(AudioEngine PUBLIC
    include
)

set_property(TARGET AudioEngine PROPERTY CXX_STANDARD 17)

# Executable using the library
add_executable(AudioPlayer
    app/main.cpp
)

target_link_libraries(AudioPlayer PRIVATE
    AudioEngine
)
```

**Generate Visual Studio project:**
```batch
sighmake CMakeLists.txt -g vcxproj -t msvc2022
```

**Generated:** `AudioEngine_.sln`, `AudioEngine_.vcxproj`, `AudioPlayer_.vcxproj`

### Limitations

CMake support is **experimental** and limited to basic features.

**Supported:**
- `project()`
- `add_executable()`
- `add_library()` (STATIC, SHARED)
- `target_include_directories()`
- `target_link_libraries()`
- `set_property(TARGET ... CXX_STANDARD)`
- Basic variables

**Not supported:**
- Complex CMake scripts
- Generator expressions
- Custom commands
- External project dependencies
- Advanced CMake features
- Most CMake modules

### When to Use CMake Integration

**Good use cases:**
- Simple CMake projects
- Migrating from CMake to sighmake
- Quick Visual Studio project generation
- Learning CMake structure

**Not recommended:**
- Complex CMake projects
- Projects with custom CMake modules
- Projects requiring full CMake feature set

### CMake to Buildscript Workflow

1. **Generate from CMake:**
```batch
sighmake CMakeLists.txt -g vcxproj
```

2. **Convert back to buildscript:**
```batch
sighmake --convert AudioEngine.sln
```

3. **Clean up buildscript:**
Edit generated `.buildscript` files, add wildcards, etc.

4. **Use buildscript going forward:**
```batch
sighmake audioengine.buildscript
```

### Hybrid Workflow

**Option 1: CMake primary, sighmake for VS**
- Maintain CMakeLists.txt
- Use CMake on Linux: `cmake . && make`
- Use sighmake on Windows: `sighmake CMakeLists.txt`

**Option 2: Buildscript primary**
- Maintain `.buildscript` files
- Generate vcxproj on Windows
- Generate Makefiles on Linux

Recommended: **Use buildscripts** for simpler, more consistent workflow.

---

## 16. Multi-Project Solutions

sighmake excels at managing multi-project solutions with dependencies.

### Basic Multi-Project Structure

```ini
[solution]
name = MyApplication
configurations = Debug, Release
platforms = x64

# Library project
[project:CoreLib]
type = lib
sources = corelib/*.cpp
headers = corelib/*.h

# Application project
[project:Application]
type = exe
sources = app/*.cpp
depends = CoreLib
libs = CoreLib.lib
```

### Complete Multi-Project Example

**Directory structure:**
```
project/
├── corelib/
│   ├── math.cpp
│   ├── math.h
│   ├── utils.cpp
│   └── utils.h
├── engine/
│   ├── engine.cpp
│   ├── engine.h
│   ├── renderer.cpp
│   └── renderer.h
├── application/
│   └── main.cpp
└── multi_project.buildscript
```

**multi_project.buildscript:**
```ini
[solution]
name = GameApplication
configurations = Debug, Release
platforms = Win32, x64

# --- Core Library (lowest level) ---
[project:CoreLib]
type = lib
sources = corelib/*.cpp
headers = corelib/*.h
includes = corelib
std = 17

outdir[Debug|Win32] = lib/Win32/Debug
outdir[Release|Win32] = lib/Win32/Release
outdir[Debug|x64] = lib/x64/Debug
outdir[Release|x64] = lib/x64/Release

intdir[Debug|Win32] = obj/CoreLib/Win32/Debug
intdir[Release|Win32] = obj/CoreLib/Win32/Release
intdir[Debug|x64] = obj/CoreLib/x64/Debug
intdir[Release|x64] = obj/CoreLib/x64/Release

# --- Engine Library (depends on CoreLib) ---
[project:Engine]
type = lib
sources = engine/*.cpp
headers = engine/*.h
includes = engine, corelib
std = 17

# Engine depends on CoreLib
depends = CoreLib
libs = CoreLib.lib
libdirs[Debug|Win32] = lib/Win32/Debug
libdirs[Release|Win32] = lib/Win32/Release
libdirs[Debug|x64] = lib/x64/Debug
libdirs[Release|x64] = lib/x64/Release

outdir[Debug|Win32] = lib/Win32/Debug
outdir[Release|Win32] = lib/Win32/Release
outdir[Debug|x64] = lib/x64/Debug
outdir[Release|x64] = lib/x64/Release

intdir[Debug|Win32] = obj/Engine/Win32/Debug
intdir[Release|Win32] = obj/Engine/Win32/Release
intdir[Debug|x64] = obj/Engine/x64/Debug
intdir[Release|x64] = obj/Engine/x64/Release

# --- Application (depends on Engine and CoreLib) ---
[project:Application]
type = exe
sources = application/*.cpp
includes = engine, corelib
std = 17
subsystem = Console

# Application depends on both Engine and CoreLib
depends = Engine, CoreLib
libs = Engine.lib, CoreLib.lib
libdirs[Debug|Win32] = lib/Win32/Debug
libdirs[Release|Win32] = lib/Win32/Release
libdirs[Debug|x64] = lib/x64/Debug
libdirs[Release|x64] = lib/x64/Release

outdir[Debug|Win32] = bin/Win32/Debug
outdir[Release|Win32] = bin/Win32/Release
outdir[Debug|x64] = bin/x64/Debug
outdir[Release|x64] = bin/x64/Release

intdir[Debug|Win32] = obj/Application/Win32/Debug
intdir[Release|Win32] = obj/Application/Win32/Release
intdir[Debug|x64] = obj/Application/x64/Debug
intdir[Release|x64] = obj/Application/x64/Release
```

### DLL + EXE Example

**Directory structure:**
```
project/
├── mathlib/
│   ├── mathlib.cpp
│   └── mathlib.h
├── calculator/
│   └── main.cpp
└── multi_project.buildscript
```

**mathlib/mathlib.h:**
```cpp
#pragma once

#ifdef MATHLIB_EXPORTS
    #define MATHLIB_API __declspec(dllexport)
#else
    #define MATHLIB_API __declspec(dllimport)
#endif

MATHLIB_API int Add(int a, int b);
MATHLIB_API int Multiply(int a, int b);
```

**mathlib/mathlib.cpp:**
```cpp
#include "mathlib.h"

MATHLIB_API int Add(int a, int b) {
    return a + b;
}

MATHLIB_API int Multiply(int a, int b) {
    return a * b;
}
```

**calculator/main.cpp:**
```cpp
#include <iostream>
#include "mathlib.h"

int main() {
    std::cout << "5 + 3 = " << Add(5, 3) << std::endl;
    std::cout << "5 * 3 = " << Multiply(5, 3) << std::endl;
    return 0;
}
```

**multi_project.buildscript:**
```ini
[solution]
name = MathApplication
configurations = Debug, Release
platforms = Win32, x64

# MathLib DLL
[project:MathLib]
type = dll
sources = mathlib/mathlib.cpp
headers = mathlib/mathlib.h
includes = mathlib
defines = MATHLIB_EXPORTS
std = 17

outdir[Debug|Win32] = output/Win32/Debug
outdir[Release|Win32] = output/Win32/Release
outdir[Debug|x64] = output/x64/Debug
outdir[Release|x64] = output/x64/Release

# Calculator EXE (uses MathLib)
[project:Calculator]
type = exe
sources = calculator/main.cpp
includes = mathlib
std = 17
subsystem = Console

# Depend on MathLib
depends = MathLib
libs = MathLib.lib

# Same output directory as MathLib for DLL loading
outdir[Debug|Win32] = output/Win32/Debug
outdir[Release|Win32] = output/Win32/Release
outdir[Debug|x64] = output/x64/Debug
outdir[Release|x64] = output/x64/Release

# Library directories (where to find MathLib.lib)
libdirs[Debug|Win32] = output/Win32/Debug
libdirs[Release|Win32] = output/Win32/Release
libdirs[Debug|x64] = output/x64/Debug
libdirs[Release|x64] = output/x64/Release
```

**Generate and build:**
```batch
sighmake multi_project.buildscript
msbuild MathApplication_.sln /p:Configuration=Release /p:Platform=x64
output\x64\Release\Calculator.exe
```

### Build Order

sighmake determines build order from `depends`:

```ini
[project:A]
type = lib

[project:B]
type = lib
depends = A    # B builds after A

[project:C]
type = exe
depends = B, A # C builds after B and A
```

**Build order:** A → B → C

### Shared Output Directories

For DLLs, place the EXE in the same directory:

```ini
[project:MyDLL]
type = dll
outdir = bin/Release

[project:MyApp]
type = exe
depends = MyDLL
outdir = bin/Release  # Same directory!
```

### Separate Intermediate Directories

Always use separate intermediate directories per project:

```ini
[project:CoreLib]
intdir[Debug] = obj/CoreLib/Debug
intdir[Release] = obj/CoreLib/Release

[project:Engine]
intdir[Debug] = obj/Engine/Debug
intdir[Release] = obj/Engine/Release
```

### Multi-Project Best Practices

1. **Organize by dependency level**: Core libraries first, applications last
2. **Use `depends` for internal projects**: Ensures correct build order
3. **Separate intermediate directories**: Avoid conflicts
4. **Shared output for DLLs**: Put DLL and EXE in same directory
5. **Consistent naming**: Use clear project names
6. **Document dependencies**: Add comments explaining relationships

---

## 17. Advanced Patterns

### Custom Configurations

Beyond Debug and Release:

```ini
[solution]
name = MyGame
configurations = Debug, Release, Profile, Shipping
platforms = x64

[project:Game]
type = exe
sources = src/**/*.cpp

# Debug: No optimization, all debug info
optimization[Debug] = Disabled
debug_info[Debug] = EditAndContinue
defines[Debug] = DEBUG, ENABLE_LOGGING, ENABLE_ASSERTS

# Release: Optimized with debug info
optimization[Release] = MaxSpeed
debug_info[Release] = ProgramDatabase
defines[Release] = NDEBUG, ENABLE_LOGGING

# Profile: Optimized for profiling
optimization[Profile] = MaxSpeed
debug_info[Profile] = ProgramDatabase
defines[Profile] = NDEBUG, ENABLE_PROFILER, ENABLE_LOGGING
link_incremental[Profile] = false

# Shipping: Maximum optimization, no debug features
optimization[Shipping] = Full
debug_info[Shipping] = None
defines[Shipping] = NDEBUG, SHIPPING_BUILD
link_incremental[Shipping] = false
```

### Configuration Matrices

Per-configuration AND per-platform:

```ini
[project:Engine]
type = lib
sources = src/**/*.cpp

# Debug Win32: Conservative optimization for Edit and Continue
optimization[Debug|Win32] = Disabled
debug_info[Debug|Win32] = EditAndContinue
runtime_library[Debug|Win32] = MultiThreadedDebug

# Debug x64: No Edit and Continue on x64
optimization[Debug|x64] = Disabled
debug_info[Debug|x64] = ProgramDatabase
runtime_library[Debug|x64] = MultiThreadedDebug

# Release Win32: Optimize for size
optimization[Release|Win32] = MinSize
debug_info[Release|Win32] = ProgramDatabase
runtime_library[Release|Win32] = MultiThreaded

# Release x64: Optimize for speed
optimization[Release|x64] = MaxSpeed
debug_info[Release|x64] = ProgramDatabase
runtime_library[Release|x64] = MultiThreaded
```

### Per-File Hot Path Optimization

Optimize critical code paths:

```ini
[project:GameEngine]
sources = src/**/*.cpp
std = 20

# Default: balanced optimization
optimization[Release] = MaxSpeed

# Hot path: maximum optimization
src/core/inner_loop.cpp:optimization[Release] = Full
src/physics/collision_detection.cpp:optimization[Release] = Full
src/renderer/draw_calls.cpp:optimization[Release] = Full

# Debug-heavy code: disable optimization for debugging
src/debug/debugger.cpp:optimization = Disabled
src/profiler/profiler.cpp:optimization = Disabled
```

### Third-Party Library Integration

**Pattern 1: Prebuilt libraries**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp
includes = include, C:/Libraries/SDL2/include
libs = SDL2.lib, SDL2main.lib
libdirs = C:/Libraries/SDL2/lib/x64
```

**Pattern 2: Header-only libraries**
```ini
[project:MyApp]
type = exe
sources = src/**/*.cpp
includes = include, external/json/include, external/spdlog/include
```

**Pattern 3: Source integration**
```ini
[project:MyApp]
type = exe
std = 20

# Your code
sources = src/**/*.cpp

# Third-party source code
sources = src/**/*.cpp, external/imgui/*.cpp, external/glad/glad.c

# Exclude third-party from PCH
external/imgui/imgui.cpp:pch = NotUsing
external/imgui/imgui_draw.cpp:pch = NotUsing
external/imgui/imgui_widgets.cpp:pch = NotUsing
external/glad/glad.c:pch = NotUsing

# Lower warning level for third-party code
external/imgui/imgui.cpp:warning_level = Level2
```

### Conditional Includes Based on Configuration

```ini
[project:Engine]
type = lib
sources = src/**/*.cpp

# Common settings for all configurations
include = common_settings.buildscript

# Debug-specific settings
include[Debug] = debug_settings.buildscript

# Release-specific settings
include[Release] = release_settings.buildscript

# Profile-specific settings
include[Profile] = profile_settings.buildscript
```

**debug_settings.buildscript:**
```ini
defines = DEBUG, ENABLE_LOGGING, ENABLE_ASSERTS, MEMORY_TRACKING
optimization = Disabled
runtime_library = MultiThreadedDebug
```

**release_settings.buildscript:**
```ini
defines = NDEBUG
optimization = MaxSpeed
runtime_library = MultiThreaded
```

**profile_settings.buildscript:**
```ini
defines = NDEBUG, ENABLE_PROFILER, ENABLE_TIMING
optimization = MaxSpeed
runtime_library = MultiThreaded
generate_debug_info = true
```

### Platform-Specific Code Organization

```
project/
├── src/
│   ├── core/                  (cross-platform)
│   ├── platform/
│   │   ├── platform.h         (interface)
│   │   ├── windows/
│   │   │   └── platform_impl.cpp
│   │   └── linux/
│   │       └── platform_impl.cpp
│   └── main.cpp
└── project.buildscript
```

```ini
[project:CrossPlatform]
type = exe
std = 17

# Core files (always included)
sources = src/core/**/*.cpp, src/main.cpp

# Platform-specific implementation
sources[Win32] = src/core/**/*.cpp, src/main.cpp, src/platform/windows/*.cpp
sources[x64] = src/core/**/*.cpp, src/main.cpp, src/platform/windows/*.cpp

# Would need separate buildscript for Linux or use generator detection
```

### Modular Projects

**Directory structure:**
```
project/
├── core.buildscript
├── renderer.buildscript
├── physics.buildscript
├── audio.buildscript
├── game.buildscript
└── shared_settings.buildscript
```

**shared_settings.buildscript:**
```ini
std = 20
warning_level = Level4
multiprocessor = true
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
```

**core.buildscript:**
```ini
[solution]
name = Core
configurations = Debug, Release
platforms = x64

[project:Core]
type = lib
sources = src/core/**/*.cpp
includes = src/core
include = shared_settings.buildscript
```

**game.buildscript (master solution):**
```ini
[solution]
name = GameEngine
configurations = Debug, Release
platforms = x64

[project:Core]
type = lib
sources = src/core/**/*.cpp
includes = src/core
include = shared_settings.buildscript

[project:Renderer]
type = lib
sources = src/renderer/**/*.cpp
includes = src/renderer, src/core
depends = Core
libs = Core.lib
include = shared_settings.buildscript

[project:Game]
type = exe
sources = src/game/**/*.cpp
includes = src/core, src/renderer
depends = Core, Renderer
libs = Core.lib, Renderer.lib
subsystem = Console
include = shared_settings.buildscript
```

---

## 18. Version Control Integration

### What to Commit

**Commit these:**
- ✓ `*.buildscript` files
- ✓ Include files (e.g., `common_settings.buildscript`)
- ✓ Generation scripts (`generate.bat`, `generate.sh`)
- ✓ `.gitignore`
- ✓ `README.md` with build instructions

**Don't commit these:**
- ✗ Generated `.vcxproj` files
- ✗ Generated `.sln` files
- ✗ Generated `.slnx` files
- ✗ Generated Makefiles (in `build/` directory)
- ✗ Build outputs (`bin/`, `obj/`, etc.)

### .gitignore Template

**.gitignore:**
```gitignore
# Generated Visual Studio files
*.vcxproj
*.vcxproj.filters
*.vcxproj.user
*.sln
*.slnx

# Visual Studio cache/options
.vs/
*.suo
*.user
*.userosscache
*.sln.docstates

# Build results
bin/
obj/
build/
lib/
[Dd]ebug/
[Rr]elease/
x64/
x86/

# Compiled files
*.exe
*.dll
*.lib
*.a
*.so
*.o
*.obj
*.pch

# Build system outputs
*.log
*.tlog
*.ilk
*.pdb
*.iobj
*.ipdb

# OS files
.DS_Store
Thumbs.db

# IDE files (if not using Visual Studio)
.vscode/
.idea/
*.swp
*.swo
*~

# Keep .buildscript files (explicitly allow)
!*.buildscript
```

### Generation Scripts

**generate.bat (Windows):**
```batch
@echo off
echo.
echo ========================================
echo   Generating Visual Studio Solution
echo ========================================
echo.

sighmake MyProject.buildscript -t msvc2022

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Solution generation failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo   Generation Complete!
echo ========================================
echo.
echo Open MyProject_.sln in Visual Studio
echo.
pause
```

**generate.sh (Linux):**
```bash
#!/bin/bash

echo ""
echo "========================================"
echo "   Generating Makefiles"
echo "========================================"
echo ""

./sighmake MyProject.buildscript -g makefile

if [ $? -ne 0 ]; then
    echo ""
    echo "ERROR: Makefile generation failed!"
    exit 1
fi

echo ""
echo "========================================"
echo "   Generation Complete!"
echo "========================================"
echo ""
echo "Build with: make -f build/MyProject.Release"
echo ""
```

Make executable:
```bash
chmod +x generate.sh
```

### Repository Structure

```
project/
├── .git/
├── .gitignore                        (version controlled)
├── README.md                          (version controlled)
├── generate.bat                       (version controlled)
├── generate.sh                        (version controlled)
├── project.buildscript                (version controlled)
├── common_settings.buildscript        (version controlled)
├── src/                               (version controlled)
│   └── *.cpp, *.h
├── external/                          (version controlled)
│   └── third-party libs
├── MyProject_.sln                     (ignored)
├── MyProject_.vcxproj                 (ignored)
└── bin/                               (ignored)
    └── build outputs
```

### Team Workflow

**New team member:**
```batch
# Clone repository
git clone https://github.com/team/project.git
cd project

# Generate project files
generate.bat

# Open and build
start MyProject_.sln
```

**Daily workflow:**
```batch
# Pull latest changes
git pull

# Regenerate if buildscripts changed
generate.bat

# Build in Visual Studio
```

**Making changes:**
```batch
# Edit buildscript
notepad MyProject.buildscript

# Regenerate
generate.bat

# Test build
msbuild MyProject_.sln /p:Configuration=Release

# Commit buildscript only
git add MyProject.buildscript
git commit -m "Add new source files to build"
git push
```

### Pull Request Best Practices

**Reviewable changes in buildscripts:**
```diff
[project:Engine]
type = lib
sources = src/**/*.cpp
+includes = include, external/json/include
-defines = DEBUG
+defines = DEBUG, ENABLE_LOGGING
std = 17
```

vs. **Unreadable changes in vcxproj:**
```diff
-    <ClCompile Include="src\old_file.cpp" />
+    <ClCompile Include="src\new_file.cpp" />
+    <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
+      <ClCompile>
+        <PreprocessorDefinitions>DEBUG;ENABLE_LOGGING;%(PreprocessorDefinitions)</PreprocessorDefinitions>
... 50 more lines of XML changes ...
```

### Handling Merge Conflicts

**With buildscripts (easy):**
```ini
<<<<<<< HEAD
sources = src/core/*.cpp
includes = include
=======
sources = src/core/*.cpp, src/utils/*.cpp
includes = include, external
>>>>>>> feature-branch
```

**Resolution:**
```ini
sources = src/core/*.cpp, src/utils/*.cpp
includes = include, external
```

**With vcxproj (nightmare):**
```xml
<<<<<<< HEAD
  <ItemGroup>
    <ClCompile Include="src\file1.cpp" />
    <ClCompile Include="src\file2.cpp" />
  </ItemGroup>
=======
  <ItemGroup>
    <ClCompile Include="src\file1.cpp" />
    <ClCompile Include="src\file3.cpp" />
  </ItemGroup>
  <ItemDefinitionGroup ...>
    ... 100 lines of XML ...
  </ItemDefinitionGroup>
>>>>>>> feature-branch
```

### Pre-Commit Hook (Optional)

Automatically regenerate project files before committing:

**.git/hooks/pre-commit:**
```bash
#!/bin/bash

# Check if .buildscript files changed
if git diff --cached --name-only | grep -q "\.buildscript$"; then
    echo "Buildscript changed, regenerating project files..."

    ./sighmake project.buildscript

    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to regenerate project files"
        exit 1
    fi

    echo "Project files regenerated successfully"
fi

exit 0
```

Make executable:
```bash
chmod +x .git/hooks/pre-commit
```

---

## 19. CI/CD Integration

sighmake integrates easily into continuous integration and deployment pipelines.

### GitHub Actions (Windows)

**.github/workflows/build-windows.yml:**
```yaml
name: Build Windows

on: [push, pull_request]

jobs:
  build:
    runs-on: windows-latest

    steps:
    - name: Checkout code
      uses: actions/checkout@v3

    - name: Setup MSBuild
      uses: microsoft/setup-msbuild@v1

    - name: Generate Visual Studio solution
      run: sighmake project.buildscript -t msvc2022

    - name: Build Debug
      run: msbuild Project_.sln /p:Configuration=Debug /p:Platform=x64

    - name: Build Release
      run: msbuild Project_.sln /p:Configuration=Release /p:Platform=x64

    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: windows-build
        path: bin/Release/
```

### GitHub Actions (Linux)

**.github/workflows/build-linux.yml:**
```yaml
name: Build Linux

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
    - name: Checkout code
      uses: actions/checkout@v3

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y build-essential

    - name: Generate Makefiles
      run: ./sighmake project.buildscript -g makefile

    - name: Build Debug
      run: make -f build/Project.Debug -j$(nproc)

    - name: Build Release
      run: make -f build/Project.Release -j$(nproc)

    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: linux-build
        path: bin/Release/
```

### GitHub Actions (Multi-Platform)

**.github/workflows/build.yml:**
```yaml
name: Build

on: [push, pull_request]

jobs:
  build:
    strategy:
      matrix:
        os: [windows-latest, ubuntu-latest]
        configuration: [Debug, Release]

    runs-on: ${{ matrix.os }}

    steps:
    - name: Checkout code
      uses: actions/checkout@v3

    - name: Setup MSBuild (Windows)
      if: runner.os == 'Windows'
      uses: microsoft/setup-msbuild@v1

    - name: Install GCC (Linux)
      if: runner.os == 'Linux'
      run: sudo apt-get update && sudo apt-get install -y build-essential

    - name: Generate project files
      shell: bash
      run: |
        if [ "$RUNNER_OS" == "Windows" ]; then
          sighmake project.buildscript -t msvc2022
        else
          ./sighmake project.buildscript -g makefile
        fi

    - name: Build (Windows)
      if: runner.os == 'Windows'
      run: msbuild Project_.sln /p:Configuration=${{ matrix.configuration }} /p:Platform=x64

    - name: Build (Linux)
      if: runner.os == 'Linux'
      run: make -f build/Project.${{ matrix.configuration }} -j$(nproc)
```

### GitLab CI

**.gitlab-ci.yml:**
```yaml
stages:
  - build

build-windows:
  stage: build
  tags:
    - windows
  script:
    - sighmake project.buildscript -t msvc2022
    - msbuild Project_.sln /p:Configuration=Release /p:Platform=x64
  artifacts:
    paths:
      - bin/Release/

build-linux:
  stage: build
  tags:
    - linux
  script:
    - ./sighmake project.buildscript -g makefile
    - make -f build/Project.Release -j$(nproc)
  artifacts:
    paths:
      - bin/Release/
```

### Azure Pipelines

**azure-pipelines.yml:**
```yaml
trigger:
  - main

pool:
  vmImage: 'windows-latest'

steps:
- task: MSBuild@1
  displayName: 'Setup MSBuild'

- script: |
    sighmake project.buildscript -t msvc2022
  displayName: 'Generate Visual Studio solution'

- task: MSBuild@1
  inputs:
    solution: 'Project_.sln'
    configuration: 'Release'
    platform: 'x64'
  displayName: 'Build project'

- task: PublishBuildArtifacts@1
  inputs:
    PathtoPublish: 'bin/Release'
    ArtifactName: 'drop'
  displayName: 'Publish artifacts'
```

### Jenkins

**Jenkinsfile:**
```groovy
pipeline {
    agent any

    stages {
        stage('Generate') {
            steps {
                script {
                    if (isUnix()) {
                        sh './sighmake project.buildscript -g makefile'
                    } else {
                        bat 'sighmake project.buildscript -t msvc2022'
                    }
                }
            }
        }

        stage('Build') {
            steps {
                script {
                    if (isUnix()) {
                        sh 'make -f build/Project.Release -j$(nproc)'
                    } else {
                        bat 'msbuild Project_.sln /p:Configuration=Release /p:Platform=x64'
                    }
                }
            }
        }

        stage('Archive') {
            steps {
                archiveArtifacts artifacts: 'bin/Release/**/*', fingerprint: true
            }
        }
    }
}
```

### Docker Build

**Dockerfile (Linux):**
```dockerfile
FROM ubuntu:22.04

# Install build tools
RUN apt-get update && apt-get install -y \
    build-essential \
    git

# Copy project
WORKDIR /app
COPY . .

# Generate and build
RUN chmod +x sighmake && \
    ./sighmake project.buildscript -g makefile && \
    make -f build/Project.Release -j$(nproc)

# Runtime stage
FROM ubuntu:22.04
COPY --from=0 /app/bin/Release/Project /usr/local/bin/
CMD ["Project"]
```

### Build Script Integration

**build.bat (Windows CI):**
```batch
@echo off
setlocal

echo Generating Visual Studio solution...
sighmake project.buildscript -t msvc2022
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Building Debug configuration...
msbuild Project_.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Building Release configuration...
msbuild Project_.sln /p:Configuration=Release /p:Platform=x64 /v:minimal
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo Build successful!
exit /b 0
```

**build.sh (Linux CI):**
```bash
#!/bin/bash
set -e

echo "Generating Makefiles..."
./sighmake project.buildscript -g makefile

echo "Building Debug configuration..."
make -f build/Project.Debug -j$(nproc)

echo "Building Release configuration..."
make -f build/Project.Release -j$(nproc)

echo ""
echo "Build successful!"
```

### CI/CD Best Practices

1. **Cache sighmake executable**: Avoid downloading on every build
2. **Parallel builds**: Use `-j` with make, `/m` with MSBuild
3. **Artifact upload**: Save build outputs for deployment
4. **Matrix builds**: Test multiple configurations and platforms
5. **Fail fast**: Exit on first error
6. **Verbose on failure**: Show full output when build fails

---

## 20. Best Practices

### Buildscript Organization

**1. Use descriptive project names:**
```ini
# Good
[project:CoreEngine]
[project:NetworkingLibrary]
[project:GameApplication]

# Bad
[project:Lib1]
[project:Proj]
[project:App]
```

**2. Group related settings:**
```ini
[project:Engine]
# File organization
sources = src/**/*.cpp
headers = include/**/*.h
includes = include, external

# Compilation settings
std = 20
defines = ENGINE_VERSION=1.0
warning_level = Level4

# Compiler options
multiprocessor = true
exception_handling = Sync
rtti = true

# Configuration-specific settings
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
```

**3. Add comments:**
```ini
[project:GameEngine]
type = lib

# Core engine files
sources = src/core/**/*.cpp

# Platform-specific files (Windows only for now)
sources = src/core/**/*.cpp, src/platform/windows/*.cpp

# Third-party libraries
sources = src/core/**/*.cpp, external/imgui/*.cpp

# Exclude third-party from precompiled header
external/imgui/imgui.cpp:pch = NotUsing
```

### Naming Conventions

**Solutions:**
- Use PascalCase: `MyApplication`, `GameEngine`

**Projects:**
- Use PascalCase: `CoreLib`, `AudioEngine`, `GameClient`
- Suffix type: `MathLib`, `UtilsDLL`, `TestRunner`

**Buildscript files:**
- Match project name: `myapplication.buildscript`
- Or use descriptive name: `multi_project.buildscript`

**Include files:**
- Use snake_case: `common_settings.buildscript`
- Be descriptive: `debug_settings.buildscript`, `release_x64_settings.buildscript`

### Directory Structure Recommendations

**Small project:**
```
project/
├── src/
│   └── *.cpp
├── include/
│   └── *.h
├── project.buildscript
└── generate.bat
```

**Medium project:**
```
project/
├── src/
│   ├── core/
│   ├── renderer/
│   └── main.cpp
├── include/
│   └── *.h
├── external/
│   └── third-party libs
├── project.buildscript
├── common_settings.buildscript
└── generate.bat
```

**Large project (multi-project):**
```
project/
├── corelib/
│   ├── src/
│   └── include/
├── engine/
│   ├── src/
│   └── include/
├── game/
│   └── src/
├── external/
│   └── third-party libs
├── shared/
│   ├── common_settings.buildscript
│   └── team_standard.buildscript
├── multi_project.buildscript
└── generate.bat
```

### Performance Tips

**1. Use wildcards effectively:**
```ini
# Good: One wildcard pattern
sources = src/**/*.cpp

# Bad: Hundreds of explicit files
sources = src/a.cpp, src/b.cpp, src/c.cpp, ...
```

**2. Enable multiprocessor compilation:**
```ini
multiprocessor = true
```

**3. Use precompiled headers:**
```ini
pch = Use
pch_header = pch.h
src/pch.cpp:pch = Create
```

**4. Incremental linking for Debug:**
```ini
link_incremental[Debug] = true
link_incremental[Release] = false
```

**5. Optimize intermediate directory organization:**
```ini
# Separate intermediate directories prevent conflicts
intdir[Debug|Win32] = obj/Win32/Debug
intdir[Release|Win32] = obj/Win32/Release
intdir[Debug|x64] = obj/x64/Debug
intdir[Release|x64] = obj/x64/Release
```

### Maintainability Guidelines

**1. Extract common settings:**

Instead of:
```ini
[project:ProjectA]
std = 17
warning_level = Level3
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed

[project:ProjectB]
std = 17
warning_level = Level3
optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
```

Do this:
```ini
[project:ProjectA]
include = common_settings.buildscript

[project:ProjectB]
include = common_settings.buildscript
```

**2. Use configuration-specific includes:**
```ini
[project:MyProject]
include = common_settings.buildscript
include[Debug] = debug_settings.buildscript
include[Release] = release_settings.buildscript
```

**3. Document non-obvious settings:**
```ini
# Use Edit and Continue only on Win32 Debug (doesn't work on x64)
debug_info[Debug|Win32] = EditAndContinue
debug_info[Debug|x64] = ProgramDatabase

# Disable optimization for better debugging experience
src/debug/debugger.cpp:optimization = Disabled

# Third-party library doesn't support PCH
external/stb_image.cpp:pch = NotUsing
```

**4. Keep buildscripts DRY (Don't Repeat Yourself):**
- Use wildcards instead of explicit file lists
- Use `include` for shared settings
- Use configuration-specific settings instead of duplicating

**5. Version control buildscripts:**
- Commit `.buildscript` files
- Ignore generated `.vcxproj` and `.sln` files
- Provide generation scripts for team members

### Security Considerations

**1. Don't commit absolute paths:**
```ini
# Bad: Hardcoded absolute path
includes = C:/Users/JohnDoe/Projects/external

# Good: Relative path
includes = external
```

**2. Don't commit sensitive defines:**
```ini
# Bad: API key in buildscript (will be in git)
defines = API_KEY=secret123

# Good: Load from environment or separate config file
# (Handled in code at runtime)
```

**3. Sanitize paths in CI:**
Ensure CI scripts don't expose sensitive paths in logs.

---

## 21. Common Patterns & Recipes

### Basic Console Application

```ini
[solution]
name = HelloWorld
configurations = Debug, Release
platforms = x64

[project:HelloWorld]
type = exe
sources = main.cpp
std = 17
subsystem = Console

outdir[Debug] = bin/Debug
outdir[Release] = bin/Release
```

### GUI Application (Windows)

```ini
[solution]
name = MyGUIApp
configurations = Debug, Release
platforms = x64

[project:MyGUIApp]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17
subsystem = Windows  # No console window
libs = user32.lib, gdi32.lib

outdir[Debug] = bin/Debug
outdir[Release] = bin/Release
```

### Static Library

```ini
[solution]
name = MathLibrary
configurations = Debug, Release
platforms = x64

[project:MathLib]
type = lib
sources = src/*.cpp
headers = include/*.h
includes = include
std = 17

outdir = lib/Release
```

**Using the library:**
```ini
[project:MyApp]
type = exe
sources = app/*.cpp
includes = ../MathLib/include
libs = MathLib.lib
libdirs = ../MathLib/lib/Release
```

### DLL with Exports

**mathlib.buildscript:**
```ini
[solution]
name = MathDLL
configurations = Debug, Release
platforms = x64

[project:MathLib]
type = dll
sources = src/mathlib.cpp
headers = include/mathlib.h
includes = include
defines = MATHLIB_EXPORTS
std = 17

outdir[Debug] = bin/Debug
outdir[Release] = bin/Release
```

**include/mathlib.h:**
```cpp
#pragma once

#ifdef MATHLIB_EXPORTS
    #define MATHLIB_API __declspec(dllexport)
#else
    #define MATHLIB_API __declspec(dllimport)
#endif

MATHLIB_API int Add(int a, int b);
```

**src/mathlib.cpp:**
```cpp
#include "mathlib.h"

MATHLIB_API int Add(int a, int b) {
    return a + b;
}
```

### Unit Test Project

```ini
[solution]
name = ProjectWithTests
configurations = Debug, Release
platforms = x64

# Main library
[project:MyLib]
type = lib
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
std = 17
outdir = lib/Release

# Unit tests
[project:Tests]
type = exe
sources = tests/**/*.cpp
includes = include, external/catch2
depends = MyLib
libs = MyLib.lib
libdirs = lib/Release
subsystem = Console
std = 17
outdir = bin/Tests
```

### Header-Only Library Organization

```ini
[solution]
name = HeaderOnlyLib
configurations = Debug, Release
platforms = x64

[project:Example]
type = exe
sources = example/main.cpp

# Include header-only library
includes = include, external/json/include, external/spdlog/include

std = 20
subsystem = Console
```

**Directory structure:**
```
project/
├── include/
│   └── mylib/
│       ├── algorithm.hpp
│       └── utils.hpp
├── external/
│   ├── json/include/
│   └── spdlog/include/
└── example/
    └── main.cpp
```

### Game Engine Project

```ini
[solution]
name = GameEngine
configurations = Debug, Release, Profile
platforms = x64

[project:Engine]
type = lib
sources = src/engine/**/*.cpp
headers = src/engine/**/*.h
includes = src/engine, external/SDL2/include
std = 20

# PCH setup
pch = Use
pch_header = pch.h
src/engine/pch.cpp:pch = Create

# Third-party exclusions
external/glad/glad.c:pch = NotUsing

# Configuration settings
defines[Debug] = DEBUG, ENABLE_LOGGING, ENABLE_ASSERTS
defines[Release] = NDEBUG
defines[Profile] = NDEBUG, ENABLE_PROFILER

optimization[Debug] = Disabled
optimization[Release] = MaxSpeed
optimization[Profile] = MaxSpeed

outdir[Debug] = lib/Debug
outdir[Release] = lib/Release
outdir[Profile] = lib/Profile

[project:Game]
type = exe
sources = src/game/**/*.cpp
includes = src/engine, src/game
depends = Engine
libs = Engine.lib, SDL2.lib, SDL2main.lib
libdirs[Debug] = lib/Debug, external/SDL2/lib/x64
libdirs[Release] = lib/Release, external/SDL2/lib/x64
libdirs[Profile] = lib/Profile, external/SDL2/lib/x64
subsystem = Console
std = 20

outdir[Debug] = bin/Debug
outdir[Release] = bin/Release
outdir[Profile] = bin/Profile
```

### Maximum Performance Build

When migrating from a hand-tuned Visual Studio project, you may want to replicate aggressive MSVC optimization flags. Here's how each one maps to sighmake buildscript settings:

| MSVC Flag | Purpose | Buildscript Equivalent |
|-----------|---------|----------------------|
| `/fp:fast` | Fast floating point (relaxed IEEE conformance) | `floating_point = Fast` |
| `/GL` + `/LTCG` | Whole program optimization / link-time code generation | `whole_program_optimization = true` |
| `/Oi` | Enable intrinsic functions | `intrinsic_functions = true` (Release default) |
| `/Ob2` | Inline any suitable function | `inline_expansion = AnySuitable` |
| `/Ob3` | Aggressive inlining (VS 2019 16.x+) | `cflags = /Ob3` (no direct buildscript equivalent) |

**Full example — maximum performance Release configuration:**
```ini
[solution]
name = MyApp
configurations = Debug, Release
platforms = x64

[project:MyApp]
type = exe
sources = src/**/*.cpp
headers = src/**/*.h
includes = src
std = 17

# Release already enables: MaxSpeed (/O2), intrinsic_functions (/Oi),
# function_level_linking (/Gy), enable_comdat_folding (/OPT:ICF),
# and optimize_references (/OPT:REF) by default.

# Additional performance settings for Release:
floating_point[Release] = Fast
whole_program_optimization[Release] = true
inline_expansion[Release] = AnySuitable
favor_size_or_speed[Release] = Speed

# For /Ob3 (aggressive inlining, VS 2019 16.x+), use cflags:
cflags[Release] = /Ob3
```

> **Note:** `intrinsic_functions` and `function_level_linking` are already enabled in the auto-populated Release configuration, so you don't need to set them explicitly unless you've defined a custom `[config:Release]` section.

### CMake Migration

**Step 1:** Existing CMakeLists.txt
**Step 2:** Generate from CMake
```batch
sighmake CMakeLists.txt -g vcxproj
```
**Step 3:** Convert to buildscript
```batch
sighmake --convert Project.sln
```
**Step 4:** Clean up and use buildscript going forward

### Cross-Platform Library

```ini
[solution]
name = CrossPlatformLib
configurations = Debug, Release
platforms = x64

[project:CPLib]
type = lib
std = 17

# Cross-platform core
sources = src/core/**/*.cpp

# Platform-specific (configure per platform)
sources[x64] = src/core/**/*.cpp, src/platform/windows/**/*.cpp

includes = include, src
defines[x64] = WIN64, _WIN64

outdir[Debug] = lib/Debug
outdir[Release] = lib/Release
```

For Linux, use same buildscript with makefile generator:
```bash
./sighmake crossplatform.buildscript -g makefile
```

### Application with Third-Party Library

This example demonstrates using `target_link_libraries()`, `public_includes`, `public_libs`, and conditional blocks - a modern approach to managing dependencies.

**Project structure:**
```
MyProject/
├── MyProject.buildscript
├── src/
│   └── main.cpp
└── 3rdparty/
    ├── 3rdparty.buildscript
    └── SDL3-3.4.0/
        ├── sdl3.buildscript
        ├── include/
        │   └── SDL3/
        └── lib/x64/
            └── SDL3.lib
```

**MyProject.buildscript:**
```ini
[solution]
name = MyProject

# Include third-party dependencies
include = 3rdparty/3rdparty.buildscript

[project:MyApp]
type = exe
sources = src/**/*.cpp
headers = src/**/*.h
includes = src
std = 20

# Use conditional block for platform-specific settings
if(Windows)
{
    subsystem = Windows
}

# Link to SDL3 - automatically gets includes and libs
target_link_libraries(SDL3)
```

**3rdparty/3rdparty.buildscript:**
```ini
# Include all third-party libraries
include = SDL3-3.4.0/sdl3.buildscript
```

**3rdparty/SDL3-3.4.0/sdl3.buildscript:**
```ini
[project:SDL3]
type = lib
headers = include/**/*.h

# Expose include directory to dependent projects
public_includes = include

# Expose pre-built library to dependent projects
public_libs = lib/x64/SDL3.lib
```

**Benefits of this approach:**
- **Simple dependency management**: Just use `target_link_libraries(SDL3)` instead of manually specifying includes and libs
- **Automatic include paths**: SDL3's `public_includes` are automatically added
- **Automatic library linking**: SDL3's `public_libs` are automatically linked
- **Clean separation**: Third-party libraries are isolated in their own buildscripts
- **Easy to add more libraries**: Just create a new buildscript and include it
- **Conditional compilation**: Use `if(Windows)` blocks for platform-specific settings

---

## 22. Troubleshooting

### Common Errors and Solutions

#### 1. "Precompiled header file not found"

**Error:**
```
fatal error C1083: Cannot open precompiled header file: 'pch.pch': No such file or directory
```

**Causes:**
- PCH output path doesn't exist
- PCH not being created
- Wrong `pch_header` path

**Solutions:**

**Check PCH configuration:**
```ini
[project:MyProject]
pch = Use
pch_header = pch.h
pch_output[Debug] = obj/Debug/pch.pch

# Ensure one file creates PCH
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h
```

**Verify directory exists:**
Create output directory manually or let build system create it.

**Clean and rebuild:**
```batch
# Delete obj/ directory
rmdir /s /q obj

# Regenerate and rebuild
sighmake project.buildscript
msbuild Project_.sln /t:Rebuild
```

#### 2. "Wildcard pattern matches no files"

**Error:**
```
Warning: Pattern 'src/*.cpp' matched no files
```

**Causes:**
- Wrong path relative to buildscript
- Files don't exist
- Wrong wildcard pattern

**Solutions:**

**Check paths are relative to buildscript location:**
```ini
# If buildscript is in project root:
sources = src/*.cpp  # Correct

# If buildscript is in build/ subdirectory:
sources = ../src/*.cpp  # Correct
```

**Use recursive wildcard if files are in subdirectories:**
```ini
# Non-recursive (only immediate children)
sources = src/*.cpp

# Recursive (all subdirectories)
sources = src/**/*.cpp
```

**Verify files exist:**
```batch
dir src\*.cpp
```

#### 3. "Unresolved external symbol"

**Error:**
```
error LNK2019: unresolved external symbol "int __cdecl Add(int,int)"
```

**Causes:**
- Missing library dependency
- Wrong library path
- DLL import library not found
- Missing source file

**Solutions:**

**Check library dependencies:**
```ini
[project:MyApp]
depends = MathLib
libs = MathLib.lib
libdirs[Debug] = lib/Debug
libdirs[Release] = lib/Release
```

**Verify library exists:**
```batch
dir lib\Debug\MathLib.lib
```

**For DLLs, ensure import library is in libdirs:**
```ini
# DLL creates both .dll and .lib
[project:MyDLL]
outdir = bin/Release  # .dll goes here

# App needs import library from same location
[project:MyApp]
libs = MyDLL.lib
libdirs = bin/Release  # Import .lib is here
```

**Check source file is included:**
```ini
sources = src/**/*.cpp  # Make sure file is matched
```

#### 4. "Cannot find DLL at runtime"

**Error:**
```
The code execution cannot proceed because MyLib.dll was not found.
```

**Causes:**
- DLL not in same directory as EXE
- DLL not in PATH
- Wrong output directory

**Solutions:**

**Place DLL and EXE in same directory:**
```ini
[project:MyDLL]
outdir = bin/Release

[project:MyApp]
outdir = bin/Release  # Same directory!
```

**Or copy DLL to EXE directory:**
Use post-build script or manual copy.

**Or add DLL directory to PATH:**
```batch
set PATH=%PATH%;C:\Project\bin\Release
MyApp.exe
```

#### 5. "Configuration not found"

**Error:**
```
Error: Configuration 'Release' not defined in solution
```

**Causes:**
- Configuration misspelled
- Configuration not in solution

**Solutions:**

**Check solution configurations:**
```ini
[solution]
configurations = Debug, Release  # Make sure it's here

[project:MyProject]
optimization[Release] = MaxSpeed  # Must match solution config
```

**Configuration names are case-sensitive:**
```ini
# Bad: Mismatched case
configurations = Debug, Release
optimization[release] = MaxSpeed  # Wrong: should be 'Release'

# Good: Consistent case
configurations = Debug, Release
optimization[Release] = MaxSpeed
```

#### 6. "Path too long"

**Error:**
```
error MSB3491: Could not write lines to file. The specified path, file name, or both are too long.
```

**Causes:**
- Very deep directory structure
- Long intermediate/output paths

**Solutions:**

**Use shorter output paths:**
```ini
# Bad: Very long path
outdir[Debug|x64] = build/intermediate/Win32/x64/Debug/output

# Good: Short path
outdir[Debug|x64] = bin/x64/Debug
```

**Move project closer to root:**
```
# Bad: C:\Users\LongUserName\Documents\Projects\Company\Division\Team\Project\...
# Good: C:\Projects\MyProject\...
```

#### 7. "Project dependency loop detected"

**Error:**
```
Error: Circular dependency detected: A -> B -> C -> A
```

**Causes:**
- Project A depends on B, B depends on C, C depends on A

**Solutions:**

**Remove circular dependency:**
```ini
# Bad: A -> B -> A (circular)
[project:A]
depends = B

[project:B]
depends = A

# Good: A -> B (linear)
[project:A]
depends = B

[project:B]
# No dependency on A
```

**Refactor to break cycle:**
Create a common base library that both depend on.

#### 8. "Include path not found"

**Error:**
```
fatal error C1083: Cannot open include file: 'myheader.h': No such file or directory
```

**Causes:**
- Include directory not specified
- Wrong include path
- Header file doesn't exist

**Solutions:**

**Add include directory:**
```ini
includes = include, external/library/include
```

**Use relative paths from buildscript location:**
```ini
# If buildscript is in project root:
includes = include

# If header is in ../include:
includes = ../include
```

**Check header exists:**
```batch
dir include\myheader.h
```

---

## 23. Reference Tables

### Complete Settings Reference

#### Solution Settings

| Setting | Description | Example |
|---------|-------------|---------|
| `name` | Solution name | `name = MyApplication` |
| `configurations` | Build configurations | `configurations = Debug, Release` |
| `platforms` | Target platforms | `platforms = Win32, x64` |

#### Project Basic Settings

| Setting | Description | Valid Values | Default |
|---------|-------------|--------------|---------|
| `type` | Project type | `exe`, `lib`, `dll` | Required |
| `sources` | Source files | File paths, supports wildcards | Required |
| `headers` | Header files | File paths, supports wildcards | None |
| `includes` | Include directories | Comma-separated paths | None |
| `defines` | Preprocessor defines | Comma-separated defines | None |
| `std` | C++ standard | `14`, `17`, `20`, `23` | Compiler default |
| `target_name` | Output file name | String | Project name |
| `target_ext` | Output file extension | `.exe`, `.lib`, `.dll` | Based on type |
| `outdir` | Output directory | Path | Platform default |
| `intdir` | Intermediate directory | Path | Platform default |

#### Compiler Settings

| Setting | Description | Valid Values | Default |
|---------|-------------|--------------|---------|
| `warning_level` | Warning level | `Level0`, `Level1`, `Level2`, `Level3`, `Level4` | `Level3` |
| `multiprocessor` | Multi-processor compilation | `true`, `false` | `true` |
| `utf8` | UTF-8 source/execution encoding | `true`, `false` | `false` |
| `exception_handling` | Exception handling | `Sync`, `Async`, `false` | `Sync` |
| `rtti` | Runtime type information | `true`, `false` | `true` |
| `optimization` | Optimization level | `Disabled`, `MinSize`, `MaxSpeed`, `Full` | Config dependent |
| `runtime_library` | Runtime library | See Runtime Library table | Config dependent |
| `debug_info` | Debug info format | `None`, `ProgramDatabase`, `EditAndContinue` | Config dependent |
| `floating_point` | Floating point model | `Precise`, `Fast`, `Strict` | `Precise` |
| `inline_expansion` | Inline function expansion | `Default`, `Disabled`, `OnlyExplicitInline`, `AnySuitable` | Compiler default |
| `intrinsic_functions` | Enable intrinsic functions | `true`, `false` | Release: `true` |
| `whole_program_optimization` | Whole program optimization (`/GL` + `/LTCG`) | `true`, `false` | `false` |
| `favor_size_or_speed` | Favor size or speed | `Neither`, `Speed`, `Size` | `Neither` |
| `cflags` | Additional compiler flags | Raw flags string | None |
| `ldflags` | Additional linker flags | Raw flags string | None |

#### Runtime Library Values

| Value | Description | Use Case |
|-------|-------------|----------|
| `MultiThreaded` | Static, release | Release static linking |
| `MultiThreadedDebug` | Static, debug | Debug static linking |
| `MultiThreadedDLL` | Dynamic, release | Release dynamic linking |
| `MultiThreadedDebugDLL` | Dynamic, debug | Debug dynamic linking |

#### Optimization Values

| Value | MSVC Flag | GCC Flag | Description |
|-------|-----------|----------|-------------|
| `Disabled` | `/Od` | `-O0` | No optimization |
| `MinSize` | `/O1` | `-Os` | Optimize for size |
| `MaxSpeed` | `/O2` | `-O3` | Optimize for speed |
| `Full` | `/Ox` | `-O3 -march=native` | Maximum optimization |

#### Linker Settings

| Setting | Description | Valid Values | Default |
|---------|-------------|--------------|---------|
| `subsystem` | Subsystem type | `Console`, `Windows` | `Console` |
| `generate_debug_info` | Generate debug info | `true`, `false` | Config dependent |
| `link_incremental` | Incremental linking | `true`, `false` | Config dependent |
| `depends` | Project dependencies | Comma-separated project names | None |
| `libs` | Library dependencies | Comma-separated library files | None |
| `libdirs` | Library search paths | Comma-separated paths | None |

#### PCH Settings

| Setting | Description | Valid Values |
|---------|-------------|--------------|
| `pch` | PCH mode | `Use`, `Create`, `NotUsing` |
| `pch_header` | PCH header file | Filename (e.g., `pch.h`) |
| `pch_output` | PCH output path | Path (e.g., `obj/Debug/pch.pch`) |

#### Debug Info Values

| Value | Description | Use Case |
|-------|-------------|----------|
| `None` | No debug info | Shipping builds |
| `ProgramDatabase` | Full debug info | Most configurations |
| `EditAndContinue` | Edit and continue support | Win32 Debug only |

### Configuration Matrix Examples

#### Debug vs Release

| Setting | Debug | Release |
|---------|-------|---------|
| `optimization` | `Disabled` | `MaxSpeed` |
| `runtime_library` | `MultiThreadedDebug` | `MultiThreaded` |
| `debug_info` | `EditAndContinue`/`ProgramDatabase` | `ProgramDatabase` |
| `link_incremental` | `true` | `false` |
| `defines` | `DEBUG`, `_DEBUG` | `NDEBUG` |

#### Win32 vs x64

| Aspect | Win32 | x64 |
|--------|-------|-----|
| Platform identifier | `Win32` | `x64` |
| Pointer size | 32-bit | 64-bit |
| Typical defines | `WIN32`, `_WIN32` | `WIN64`, `_WIN64` |
| Edit and Continue | Supported in Debug | Not supported |
| Output directory example | `bin/Win32/Release` | `bin/x64/Release` |

### Wildcard Patterns

| Pattern | Description | Example Match |
|---------|-------------|---------------|
| `*.cpp` | All .cpp in directory | `main.cpp`, `utils.cpp` |
| `**/*.cpp` | All .cpp recursively | `src/core/engine.cpp`, `src/game/player.cpp` |
| `*.{cpp,h}` | Multiple extensions | `main.cpp`, `utils.h` |
| `src/*/*.cpp` | One level deep | `src/core/engine.cpp` |
| `?ile.cpp` | Single char wildcard | `file.cpp`, `bile.cpp` |

### Toolset Version Compatibility

| Toolset | VS Version | C++14 | C++17 | C++20 | C++23 |
|---------|-----------|-------|-------|-------|-------|
| msvc2026 | VS 2026 | ✓ | ✓ | ✓ | ✓ |
| msvc2022 | VS 2022 | ✓ | ✓ | ✓ | Partial |
| msvc2019 | VS 2019 | ✓ | ✓ | Partial | ✗ |
| msvc2017 | VS 2017 | ✓ | Partial | ✗ | ✗ |
| msvc2015 | VS 2015 | Partial | ✗ | ✗ | ✗ |
| msvc2013 | VS 2013 | ✗ | ✗ | ✗ | ✗ |

### Generator Comparison

| Feature | vcxproj | makefile |
|---------|---------|----------|
| **Platform** | Windows | Linux / Cross-platform |
| **Default OS** | Windows | Linux |
| **IDE** | Visual Studio | Any (VSCode, Vim, etc.) |
| **Build Tool** | MSBuild | GNU Make |
| **File Extension** | `.vcxproj`, `.sln` | Makefile |
| **Multi-config** | Yes | No (separate files) |
| **Parallel builds** | Automatic (`/m`) | Manual (`-j`) |
| **Debugging** | Integrated | External (GDB/LLDB) |
| **Incremental builds** | Yes | Yes |
| **Project dependencies** | Automatic | Managed |

### Common Default Values

| Setting | Debug Default | Release Default |
|---------|--------------|-----------------|
| `optimization` | `Disabled` | `MaxSpeed` |
| `runtime_library` | `*Debug` variant | Non-debug variant |
| `debug_info` | `EditAndContinue` (Win32) or `ProgramDatabase` (x64) | `ProgramDatabase` |
| `link_incremental` | `true` | `false` |
| `generate_debug_info` | `true` | `true` |

---

## Conclusion

This guide covered all aspects of using sighmake, from basic quick start to advanced multi-project setups, cross-platform development, CI/CD integration, and troubleshooting.

For more examples, see the `examples/` directory in the sighmake repository.

For issues or feature requests, visit: [sighmake GitHub repository](https://github.com/yourusername/sighmake)

Happy building!
