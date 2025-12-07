# sighmake

A build system generator that converts simple buildscript files into Visual Studio project files (.vcxproj and .sln).

## Features

- Simple, readable buildscript format
- Generates Visual Studio project files (.vcxproj)
- Generates Visual Studio solution files (.sln)
- Supports multiple configurations (Debug, Release, etc.)
- Per-file compiler settings
- Wildcard support for source files
- Precompiled header support
- Custom build events

## Building

### Prerequisites
- C++17 compatible compiler (MSVC)

### Build Instructions

#### Windows (Command Prompt)

```batch
sighmake.exe sighmake.buildscript -b build
```

open solution in build/sighmake.sln

## Usage

```
sighmake <buildscript> [options]

Options:
  -b, --build <dir>    Output directory (default: current directory)
  -h, --help            Show help message
```

### Example

```batch
sighmake build.txt -b ./output
```

## Buildscript Format

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

```ini
[project:ProjectName]
type = exe                      # exe, lib, dll
sources = src/*.cpp             # Source files (supports wildcards)
headers = include/*.h           # Header files
includes = include, external    # Include directories
defines = MYDEFINE              # Preprocessor definitions
std = 17                        # C++ standard (14, 17, 20)
libs = user32.lib, gdi32.lib   # Library dependencies
```

### Configuration-Specific Settings

```ini
[project:MyProject]
optimization[Debug|Win32] = Disabled
optimization[Release|Win32] = MaxSpeed
```

### Per-File Settings

```ini
main.cpp:pch = Create
main.cpp:pch_header = pch.h

utils.cpp:defines = EXTRA_DEFINE
utils.cpp:defines[Debug|Win32] = DEBUG_UTILS
```

## License

This project uses PugiXML, which is licensed under the MIT License.