# sighmake

A build system generator that converts simple buildscript files into Visual Studio project files (.vcxproj and .sln) and Makefiles.

## Features

- Simple, readable buildscript format
- Generates Visual Studio project files (.vcxproj) and solutions (.sln)
- Generates Makefiles for Linux/MinGW
- Supports multiple configurations (Debug, Release, etc.)
- Per-file compiler settings
- Wildcard support for source files
- Precompiled header support
- Custom build events

## Building

### Prerequisites
- C++17 compatible compiler (MSVC on Windows, GCC/Clang on Linux)

### Build Instructions

#### Windows
Run `GenerateSolution.bat` to generate the Visual Studio solution, then build with VS. Or use the provided `sighmake.exe`.

#### Linux
Run the bootstrap script to compile and generate Makefiles:
```bash
chmod +x generate_linux.sh
./generate_linux.sh
make -f build/sighmake.Release
```

## Usage

```
sighmake <buildscript> [options]

Options:
  -g, --generator <type>     Generator type (vcxproj, makefile)
  -t, --toolset <name>       Default toolset (msvc2022, msvc2019, etc)
  -c, --convert              Convert Visual Studio solution to buildscripts
  -l, --list                 List available generators
  -h, --help                 Show help message
```

### Examples

**Generate Visual Studio project:**
```batch
sighmake project.buildscript -g vcxproj
```

**Generate Makefiles (default on Linux):**
```bash
./sighmake project.buildscript -g makefile
```
Makefiles are generated in the `build/` directory with the format `ProjectName.Configuration`.

## Buildscript Format
... (rest of the content)

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