# sighmake Usage Reference

sighmake reads `.buildscript` files and a supported subset of CMake, then
generates Visual Studio projects, GNU Makefiles, CMake projects, or normalized
buildscripts. This guide documents the behavior implemented by the current
parser and generators.

For installation, source builds, tests, and release packaging, see
[docs/BUILD.md](docs/BUILD.md). For a short project overview, see
[README.md](README.md).

## Contents

- [Quick start](#quick-start)
- [Command line](#command-line)
- [Buildscript language](#buildscript-language)
- [Configurations and platforms](#configurations-and-platforms)
- [Projects and files](#projects-and-files)
- [Dependencies and packages](#dependencies-and-packages)
- [Generators and direct builds](#generators-and-direct-builds)
- [Android](#android)
- [Conversion](#conversion)
- [Setting reference](#setting-reference)
- [Troubleshooting](#troubleshooting)

## Quick start

Create `hello.buildscript` next to your source tree:

```ini
[solution]
name = Hello
configurations = Debug, Release

# Choose the platforms that this file actually targets.
platforms = Win32, x64

[project:Hello]
type = exe
sources = src/*.cpp
headers = include/*.h
includes = include
std = 20
subsystem = Console
```

On Linux, use `platforms = Linux`. On macOS, use `platforms = macOS`. A single
file may contain both Windows and Unix platforms; each generator filters out
platforms it cannot build.

Generate files with the host default generator:

```text
sighmake hello.buildscript
```

- Windows defaults to `vcxproj` and writes Visual Studio files under `build/`.
- Linux and macOS default to `makefile` and write Makefiles under `build/`.

Build from the directory containing the generated `.sighmake_cache`:

```text
sighmake --build . --config Release --parallel 8
```

Generation must run before `--build`. If `--config` is omitted, sighmake uses
`Debug` when that configuration exists, otherwise the first available
configuration.

> The parser defaults to `Debug, Release` and `Win32, x64` when the solution
> omits those lists. That platform default is useful for Visual Studio, but the
> Makefile generator will have no build targets until a non-Windows platform is
> declared.

## Command line

### Syntax

```text
sighmake <input-file> [generation-options]
sighmake --build <directory> [build-options]
sighmake --convert <file.sln|.slnx|.vcxproj|.vcproj> [options]
sighmake convert vpc <file.vpc>
sighmake update [--check-only] [--force]
```

`<input-file>` may be a `.buildscript`, `CMakeLists.txt`, or `.cmake` file.

### Generation options

| Option | Meaning |
| --- | --- |
| `-g`, `--generator <type>` | Select `vcxproj`, `makefile`, `cmake`, or `buildscript`. |
| `-B`, `--build-dir <dir>` | Set the Visual Studio output subdirectory. The default is `build`. Other generators ignore this option with a warning. |
| `-D <NAME>=<VALUE>` | Define a value used by `${NAME}` substitutions while parsing a buildscript. Repeat for multiple variables. |
| `-t`, `--toolset <name>` | Set the default MSVC toolset for projects that do not specify one. |
| `--export-deps` | Write `<Solution>_dependencies.html` in the output root. |
| `-l`, `--list` | List registered generators. |
| `--list-toolsets` | List recognized `msvcYYYY` toolset aliases. |
| `-h`, `--help` | Show command help. |
| `-V`, `--version` | Show the version and release repository. |

Examples:

```text
sighmake app.buildscript -g cmake
sighmake app.buildscript -g vcxproj -B vs-build
sighmake app.buildscript -t msvc2022
sighmake app.buildscript -D SDK_ROOT=C:/SDK
sighmake app.buildscript --export-deps
```

`-D` is two arguments: the option and `NAME=VALUE`. Undefined `${NAME}`
references are replaced with an empty string. Values discovered later by
`find_package()` replace a command-line variable with the same name.

### Build options

| Option | Meaning |
| --- | --- |
| `-b`, `--build <dir>` | Build from the cache in `<dir>`. This form must be the first command argument. |
| `--config <name>` | Select a configuration such as `Debug` or `Release`. |
| `--platform <name>` | Select a generated Visual Studio platform such as `x64` or `Win32`; `x86` is an alias for `Win32`. |
| `--target <name>` | Pass a target to the active backend. |
| `--project <name-or-file>` | Build one generated project instead of the complete solution/graph. |
| `--no-project-references`, `--no-deps` | With an MSBuild project build, set `BuildProjectReferences=false`. |
| `--clean` | Clean without building. |
| `--clean-first` | Clean, then build. |
| `-j`, `--parallel <N>` | Set the parallel job count. |

Examples:

```text
sighmake --build .
sighmake --build . --config Release -j 8
sighmake --build . --config Debug --platform Win32 --project Editor
sighmake --build . --clean
```

For Visual Studio builds, `--platform` validates the requested name against the
generated platform matrix and passes it to MSBuild. Platform names are matched
case-insensitively, and `x86` selects Visual Studio's canonical `Win32`
platform. If the option is omitted, sighmake prefers `x64` when present and
otherwise uses the first cached platform.

The Makefile and CMake generators do not retain the same build-time platform
matrix, so their `--build` routes reject `--platform` with an explanation.
Select the platform while generating a Makefile or configuring CMake instead.

### Updates

```text
sighmake update --check-only
sighmake update
sighmake update --force
```

`--check-only` performs no installation. `--force` installs the latest release
even when its version matches the current executable.

### Environment variables

| Variable | Purpose |
| --- | --- |
| `SIGHMAKE_DEFAULT_TOOLSET` | Default MSVC toolset when neither `-t` nor the project sets one. |
| `SIGHMAKE_UPDATE_MANIFEST_URL` | Override the updater manifest URL. |
| `SIGHMAKE_DEBUG=1` | Enable verbose diagnostic output. |

Package discovery and Android use additional environment variables described in
[Finding packages](#finding-packages) and [Android](#android).

## Buildscript language

### Sections and assignments

A buildscript normally contains one solution and one or more projects:

```ini
[solution]
name = Example
configurations = Debug, Release
platforms = x64, Linux

[project:Core]
type = lib
sources = core/*.cpp

[project:App]
type = exe
sources = app/*.cpp
```

Setting names and section names are case-sensitive. Use the lowercase spellings
shown in this guide. Values such as `MaxSpeed`, `MultiThreadedDLL`, and
`CompileAsCpp` also use the spellings expected by their backend.

### Comments

Lines whose first non-whitespace character is `#` or `;` are comments:

```ini
# This is a comment.
; This is also a comment.
sources = src/*.cpp
```

Do not put comments after an ordinary value or function argument. Inline
comments are not generally removed and can become part of a path, flag, or
dependency name.

### Lists and multiline values

Most list settings are comma-separated:

```ini
sources = src/main.cpp, src/app.cpp
defines = APP_BUILD, FEATURE_X=1
```

Brace lists are a readable equivalent. One entry is placed on each line; commas
are not required:

```ini
sources = {
    src/main.cpp
    src/app.cpp
    src/platform.cpp [windows]
}
```

Triple quotes preserve multiline command values:

```ini
postbuild = """
echo first step
echo second step
"""
```

### Paths and wildcards

Relative input and include paths are resolved from the buildscript that contains
them. `*` matches files in one directory and `**` enables recursive traversal:

```ini
sources = src/*.cpp
headers = include/**/*.hpp
```

Wildcard expansion happens while sighmake parses the file. A wildcard that
currently matches nothing adds no files. Quote an individual source path when it
contains spaces:

```ini
sources = "third party/source file.cpp"
```

MSBuild expressions containing `$(` or `%(` are preserved rather than resolved
as filesystem paths. They are primarily useful with the `vcxproj` generator and
may have no meaning in Make or CMake output.

### Includes

Use `include` for a shared settings fragment:

```ini
[project:App]
include = config/common.buildscript
sources = app/*.cpp
```

The include path is relative to the file containing the directive. Includes are
parsed immediately in the current context, then the parent context and base path
are restored. Circular includes and missing files produce warnings. Settings-only
fragments are the safest use because project declarations inside an include can
make ownership and ordering difficult to follow.

### Variables

Buildscript substitution uses `${NAME}`:

```ini
includes = ${SDK_ROOT}/include
libdirs = ${SDK_ROOT}/lib
```

Variables come from `-D` and `find_package()`. Buildscripts do not currently
provide a general variable-assignment statement.

## Configurations and platforms

### Solution matrix

Declare configuration names and target platforms in `[solution]`:

```ini
[solution]
name = Engine
configurations = Debug, Release, Profile
platforms = Win32, x64, Linux, Android
```

`x86` normalizes to `Win32`, and any casing of `android` normalizes to
`Android`. Other platform names retain their spelling.

The generators use these platform groups:

| Generator | Platforms used |
| --- | --- |
| `vcxproj` | `Win32`, `x64`, `x86`, `ARM`, and `ARM64`; Unix and Android platforms are skipped. |
| `makefile` | Non-Windows platforms such as `Linux`, `macOS`, and `Android`; Windows platforms are skipped. |
| `cmake` | Generates one CMake target graph and does not preserve the full platform dimension. |

### Exact selectors

Use a full `Configuration|Platform` selector for one matrix cell:

```ini
optimization[Debug|x64] = Disabled
optimization[Release|x64] = MaxSpeed
defines[Debug|Linux] = DEBUG_LINUX
```

A single token in brackets is a **platform selector**, applied to every current
configuration for that platform:

```ini
defines[x64] = PLATFORM_X64
defines[Linux] = PLATFORM_LINUX
```

`setting[Debug]` is therefore not configuration shorthand; it targets a platform
literally named `Debug`. To configure all platforms for one configuration,
repeat the full selector for each platform or use explicit full configuration
sections.

`[*]` expands over the current solution matrix for project/configuration
settings:

```ini
treat_warning_as_error[*] = true
```

Declare the solution matrix before bracketed settings so expansion sees the
intended configurations and platforms.

### Configuration sections

Full configuration sections group settings for one matrix cell:

```ini
[project:App]
type = exe
sources = src/*.cpp

[config:Debug|x64]
optimization = Disabled
runtime_library = MultiThreadedDebugDLL

[config:Release|x64]
optimization = MaxSpeed
runtime_library = MultiThreadedDLL
```

Always include the platform in a configuration section. The current parser does
not expand `[config:Debug]` over all platforms.

When configuration sections are present, the names and platforms appearing in
those sections become the discovered matrix. If several names and platforms are
present, sighmake forms their cross-product and fills unset cells with defaults.

### Configuration templates

A configuration can copy a previously defined configuration and override selected
settings:

```ini
[config:Release|x64]
optimization = MaxSpeed
runtime_library = MultiThreadedDLL
defines = NDEBUG

[config:Profile|x64] : Template:Release
defines = NDEBUG, ENABLE_PROFILER
generate_debug_info = true
```

For readability, define the base before the derived section. Use full
`Config|Platform` section names for predictable inheritance.

### Defaults

When omitted, the parser supplies these baseline values:

| Setting | Default |
| --- | --- |
| Configurations | `Debug`, `Release` |
| Platforms | `Win32`, `x64` |
| Character set | `MultiByte` |
| Windows SDK | `10.0` |
| Debug optimization | `Disabled` |
| Other optimization | `MaxSpeed` |
| Debug runtime | `MultiThreadedDebugDLL` |
| Other runtime | `MultiThreadedDLL` |
| Debug info | `EditAndContinue` for `Debug|Win32`; `ProgramDatabase` otherwise |

Non-Debug configurations also default to function-level linking, intrinsic
functions, reference optimization, and COMDAT folding when those settings are
unset. Explicit settings take precedence.

### Conditional blocks

Conditions are evaluated on the machine running sighmake:

```ini
if(windows) {
    defines = HOST_WINDOWS
}

if(linux) {
    defines = HOST_LINUX
}
```

Supported host conditions are `windows`, `linux`, `osx`/`macos`/`darwin`, and
`unix`/`posix`. Their `!` negations are also supported.

`Win32` and `x64` are Windows-host platform filters. `Android` is a target
platform filter that executes on every host:

```ini
if(x64) {
    defines = PLATFORM_X64
}

if(Android) {
    defines = PLATFORM_ANDROID
}
```

`!Win32` selects `x64` on Windows and `!x64` selects `Win32`. `!Android` is not
implemented. Conditions may be nested, and the opening brace may appear on the
same line or the next line.

File entries may also have generation-time conditions:

```ini
sources = {
    src/common.cpp
    src/windows.cpp [windows]
    src/linux.cpp [linux]
    src/metal.mm [macOS]
}
```

These conditions select files using the host running sighmake; they are not a
general per-target source filter. Use separate generation runs or explicit
per-file exclusion settings when one run must describe several target OSes.

## Projects and files

### Project types

| Type | Meaning |
| --- | --- |
| `exe` | Executable/application. |
| `lib` | Static library. |
| `dll` | Shared/dynamic library. |
| `interface` | Header-only or usage-requirement target; no binary is built. |
| `sys` | Kernel-mode driver project. |
| `sys_lib` | Kernel-mode static library with exceptions, RTTI, and buffer security checks disabled by default. |

Canonical type names above are preferred. Accepted compatibility aliases include
`application`, `static`, `staticlib`, `shared`, `dynamiclib`, `header-only`,
`driver`, and `kernel_lib`.

`sys` and `sys_lib` primarily target Visual Studio. Other generators can emit a
file with a corresponding type, but they do not provide a Windows driver
toolchain.

### Source inputs

| Setting | Files |
| --- | --- |
| `sources` | C (`.c`), C++ (`.cpp`, `.cc`, `.cxx`), Objective-C/Objective-C++ (`.m`, `.mm`), or other source entries. |
| `headers` | `.h`, `.hpp`, `.hh`, and `.hxx`. |
| `resources` | Windows resource (`.rc`) files. |
| `masm` | MASM (`.asm`, `.masm`) files. |
| `nasm` | NASM (`.asm`, `.nasm`) files. |
| `mc` | Windows Message Compiler (`.mc`) files. |
| `idl` | MIDL (`.idl`) files. |

`masm[x64]` and `nasm[x64]` add files for the selected platform and exclude them
from the others in the current matrix.

Backend support differs: Visual Studio handles all Windows-specific input types;
Make compiles C, C++, Objective-C/Objective-C++, and NASM; CMake emits C/C++,
Objective-C/Objective-C++, resource, MASM, and NASM rules. Verify generated
output when using specialized tools.

### C and C++ language selection

Projects auto-detect their language from source extensions. Any C++ source makes
the project C++; a project containing only `.c` sources is C.

```ini
language = C
c_standard = 11
```

Supported C standards are `89`/`90`, `99`, `11`, `17`, and `23`. MSVC falls
back to C11 for standards it cannot express.

```ini
language = C++
std = 20
```

Supported C++ standards are `14`, `17`, `20`, `23`, and `latest`. The GNU and
CMake backends currently map `latest` to C++23.

### Per-file settings

The compact form is `path:setting = value`:

```ini
pch.cpp:pch = Create
third_party/legacy.cpp:pch = NotUsing
src/hot.cpp:optimization[Release|x64] = Full
src/debug.cpp:excluded[Release|x64] = true
```

Supported per-file settings are `includes`, `defines`, `flags`/`cflags`, `pch`,
`pch_header`, `pch_output`, `exclude`/`excluded`, `object_file`, `compile_as`,
`optimization`, and custom-build command fields.

Unqualified per-file settings apply to all configurations. For a qualified
per-file setting use a full `Config|Platform` selector or `[*]`; single-token
per-file selectors are not expanded.

A file section is equivalent and is useful for several properties:

```ini
[file:third_party/legacy.cpp]
pch = NotUsing
compile_as = CompileAsCpp
defines = LEGACY_BUILD
```

Apply the same properties to several files with `file_properties`:

```ini
file_properties(src/a.cpp, src/b.cpp) {
    pch = NotUsing
    optimization[Release|x64] = MaxSpeed
}
```

The alternative parenthesized form applies settings to one file:

```ini
set_file_properties(src/legacy.c,
    compile_as = CompileAsC
    pch = NotUsing
)
```

### Precompiled headers

Set the project/configuration PCH policy, then mark its creator file:

```ini
pch = Use
pch_header = pch.h
pch_output = $(IntDir)pch.pch

pch.cpp:pch = Create
third_party/source.cpp:pch = NotUsing
```

Modes are `Use`, `Create`, and `NotUsing`. `pch_output` is mainly an MSBuild
setting; Make creates a `.gch` in its intermediate directory and CMake uses
`target_precompile_headers`.

`uses_pch()` is a compact per-file helper:

```ini
uses_pch("Use", "pch.h", ["src/a.cpp", "src/b.cpp"])
```

An optional third string before the file array specifies the PCH output path.

### Build events

```ini
prebuild = echo Preparing build
prelink = echo Linking target
postbuild = echo Build complete

prebuild_message = Preparing
postbuild_use_in_build = true
```

The corresponding `*_message` and `*_use_in_build` settings are available for
pre-build, pre-link, and post-build events. Commands are backend-specific shell
text; a command written for `cmd.exe` will not automatically become a POSIX shell
command.

### Runtime dependency receipts

Executable and shared-library targets generated for Visual Studio or Makefiles
can declare files that must travel with their primary artifact:

```ini
runtime_dependencies = SDL3|lib/x64/SDL3.dll|SDL3.dll|true
runtime_dependencies = OptionalCodec|bin/codec.dll|plugins/codec.dll|false
```

Each record is `Name|Source|StagePath|Required`. `Source` is resolved relative
to the buildscript that declares it. Runtime dependencies propagate through the
complete physical dependency closure, including private static-library edges,
because link visibility does not change a final process's runtime requirements.

After a successful Visual Studio or Makefile build, sighmake writes
`<Target>.targetreceipt.json` beside the primary artifact. Format version 1
identifies the target/platform/architecture/configuration, records the primary
artifact size and SHA-256 hash, and emits a canonical sorted list of present
runtime dependencies with their absolute source paths, safe relative stage
paths, sizes, and SHA-256 hashes. Missing required inputs fail the build;
missing optional inputs are retained in `SkippedRuntimeDependencies`.

Generated Makefiles require Python 3 for receipt creation (`PYTHON=...` can
override the default `python3` command). They revalidate the receipt on every
normal build, including when the linked artifact is already up to date, and
remove it during `make clean`. Desktop architecture is detected from `uname -m`
and normalized to sighmake's architecture names; cross-builds can set
`SIGHMAKE_RECEIPT_ARCHITECTURE` explicitly. Android receipts use `ANDROID_ABI`.

Target receipts are an artifact handoff contract. Consumers should reject an
unknown format, tuple mismatch, stale primary-artifact hash, unsafe stage path,
duplicate or unsorted record, or dependency hash drift before publication.

### Custom build rules

Use `custom_build()` for a file produced by another tool:

```ini
custom_build(schema/input.schema,
    command = schema_compiler schema/input.schema -o generated/schema.cpp
    outputs = generated/schema.cpp
    inputs = schema/common.schema
    description = Generating schema source
)
```

The single-line form is also accepted when each argument is comma-separated.
Visual Studio and CMake emit custom build rules. The Makefile generator currently
does not emit `custom_build()` rules.

### Solution folders and Visual Studio filters

Outside a project, `folder()` groups projects into solution folders:

```ini
folder("Libraries") {
    [project:Core]
    type = lib
    sources = core/*.cpp
}
```

Folders may be nested. Inside a project, the same syntax assigns files to Visual
Studio filters:

```ini
[project:App]
type = exe

folder("Source/Core") {
    sources = src/core/*.cpp
    headers = include/core/*.h
}
```

Folder metadata affects Visual Studio organization; it does not move files on
disk.

## Dependencies and packages

### Project dependencies

The simple form creates a public project dependency:

```ini
[project:App]
depends = Core, Graphics
```

For explicit visibility, use `target_link_libraries()` inside the consuming
project. Unlike CMake syntax, the current project name is not an argument:

```ini
[project:Engine]
type = lib
sources = engine/*.cpp

target_link_libraries(
    PUBLIC Core
    PRIVATE Compression
    INTERFACE Headers
)
```

Visibility follows the keyword until another keyword appears. With no keyword,
dependencies default to `PUBLIC` for compatibility.

| Visibility | Current target | Downstream consumers |
| --- | --- | --- |
| `PUBLIC` | Uses dependency public properties | Properties continue transitively |
| `PRIVATE` | Uses dependency public properties | Transitive propagation stops |
| `INTERFACE` | Does not use dependency locally | Properties are passed to consumers |

A library publishes usage requirements with:

```ini
public_includes = include
public_defines = CORE_API=1
public_libs = zlib
public_libdirs = third_party/lib
```

These properties can also use exact or platform bracket selectors. They
propagate through project references according to visibility.

### Whole-archive linking

`WHOLE_ARCHIVE` applies to the next dependency only:

```ini
target_link_libraries(
    PRIVATE WHOLE_ARCHIVE PluginRegistry
    PRIVATE Utility
)
```

The generators map this to `/WHOLEARCHIVE`, GNU/Apple linker flags, or CMake's
`LINK_LIBRARY:WHOLE_ARCHIVE` expression. The CMake expression requires CMake
3.24 or newer even though the generated root currently declares a 3.20 minimum.
Whole-archive is a direct-link property and does not propagate transitively.

### External libraries

Use `libs` or `link_libs` for linker dependencies and `libdirs` for search
paths:

```ini
libs[x64] = user32.lib, shell32.lib
libdirs = third_party/lib
ldflags[Release|x64] = /INCREMENTAL:NO
```

Bare library names are backend-translated where possible. Raw `cflags` and
`ldflags` are passed through; keep them inside host/config-specific settings when
one buildscript feeds different toolchains.

### Finding packages

The buildscript parser has built-in finders for:

- `Vulkan`
- `OpenGL`
- `SDL2`
- `SDL3`
- `DirectX9`/`DX9`
- `DirectX10`/`DX10`
- `DirectX11`
- `DirectX12`

```ini
find_package(Vulkan REQUIRED)

[project:Renderer]
type = lib
sources = renderer/*.cpp
target_link_libraries(PRIVATE Vulkan)
```

`REQUIRED` turns a missing package into an error. Without it, parsing continues
after a warning. `NO_PROPAGATE` sets result variables but does not create the
synthetic dependency used by `target_link_libraries`:

```ini
find_package(SDL3 REQUIRED NO_PROPAGATE)
```

Successful discovery always defines `${Package_FOUND}`,
`${Package_INCLUDE_DIRS}`, and `${Package_LIBRARIES}`. When available it also
defines `${Package_LIBRARY_DIRS}`, `${Package_LIBRARY_DIRS_X64}`, and
`${Package_VERSION}`. Values containing multiple entries are semicolon-separated,
so automatic dependency propagation is preferable to copying those variables
into comma-separated buildscript lists.

On Windows the finders use installed system SDKs and variables such as
`VULKAN_SDK`, `SDL2_DIR`/`SDL2`, `SDL3_DIR`/`SDL3`, and `DXSDK_DIR`. On Linux and
macOS they first use `pkg-config` where applicable, then check standard paths.
DirectX finders are Windows-only. Discovery runs on the host executing sighmake,
not on a remote or Android target.

## Generators and direct builds

### Generator comparison

| Generator | Output | Direct build backend | Important boundary |
| --- | --- | --- | --- |
| `vcxproj` | `.vcxproj` files and one `.sln` or `.slnx` under `build/` by default | MSBuild | Requires a detected Visual Studio installation and skips Unix/Android platforms. |
| `makefile` | Master and per-project Makefiles under `build/` | GNU Make | Skips Windows platforms. |
| `cmake` | Root `CMakeLists.txt` plus one subdirectory per project | `cmake --build` | Collapses the platform dimension by using one configuration record per configuration name. |
| `buildscript` | `.buildscript` output | None | Intended primarily for conversion/normalization. |

Run `sighmake --list` to see the generators compiled into the current binary.

### Visual Studio

```text
sighmake app.buildscript -g vcxproj
sighmake app.buildscript -g vcxproj -B generated/vs
```

The generator detects Visual Studio before writing projects. It emits `.slnx`
for installations the code classifies as Visual Studio 2026 or newer and `.sln`
for older installations; it does not emit both formats in one run. Generated
filenames may include the repository-configured underscore separator.

An explicit project toolset wins. Otherwise `-t` or
`SIGHMAKE_DEFAULT_TOOLSET` wins, followed by the detected installation's
toolset. `--list-toolsets` prints aliases such as `msvc2022`; raw identifiers
such as `v143` are also accepted. Unknown identifiers are retained for forward
compatibility and produce a warning.

### Make

```text
sighmake app.buildscript -g makefile
make -C build
make -C build Release
make -C build App
make -C build clean
```

The master Makefile also provides `install` and `uninstall` for host executables
and shared libraries. `PREFIX` defaults to `/usr/local`; `DESTDIR` is honored.
Static libraries and Android binaries are not installed by these targets.

When both desktop and Android platforms exist, desktop configurations keep names
such as `Release` while Android configurations use `Release.Android`.
Linux and macOS configurations with the same name also share one desktop
Makefile name, so generate them in separate runs when their settings differ.

### CMake

```text
sighmake app.buildscript -g cmake
sighmake --build . --config Release -j 8
```

On the first direct build, sighmake configures the generated project with
`cmake -S <cache-dir> -B <cache-dir>/build`, then invokes `cmake --build`.

The CMake generator translates the portable subset of compiler settings,
dependencies, PCH, per-file settings, build events, NASM, and custom build rules.
MSBuild-specific values may be omitted or passed only inside guarded expressions;
inspect generated CMake when relying on an MSVC-specific setting.

### Build cache

Every directly buildable generator writes `.sighmake_cache` in its output root.
Pass that root—not the nested `build/` directory—to `sighmake --build`:

```text
# Correct when .sighmake_cache is in the current directory
sighmake --build .
```

Regenerate after moving the output tree, changing generators, or editing the
solution matrix. The cache records the generator, projects, configurations,
platforms, and backend paths.

## Android

Add `Android` to the solution platforms and use the Makefile generator for
self-contained NDK Makefiles:

```ini
[solution]
name = NativeApp
configurations = Debug, Release
platforms = Android

[project:NativeApp]
type = dll
sources = src/*.cpp

if(Android) {
    defines = ANDROID_BUILD
}
```

Set `ANDROID_NDK_HOME` or `ANDROID_NDK_ROOT` when building. The generated files
require NDK r19 or newer.

```text
make -C build Debug.Android
make -C build Release.Android ANDROID_ABI=x86_64 ANDROID_API=26
sighmake --build . --target Release.Android
```

Defaults and accepted overrides:

| Variable | Default | Accepted values |
| --- | --- | --- |
| `ANDROID_ABI` | `arm64-v8a` | `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86` |
| `ANDROID_API` | `24` | An NDK API level available in the installed toolchain |

Objects and outputs are scoped under
`build/<Config>/android/$(ANDROID_ABI)/`. Android shared libraries receive a
`lib` prefix and `.so` suffix. Release executables/shared libraries use the
NDK's `llvm-strip`. Host `make install` intentionally skips Android outputs.

The CMake generator also annotates Android output, but configure it explicitly
with the NDK toolchain file:

```text
cmake -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24
```

## Conversion

### Visual Studio to buildscript

```text
sighmake --convert Game.sln
sighmake --convert Game.slnx
sighmake --convert Game.vcxproj
sighmake --convert Legacy.vcproj
```

The conversion reader supports modern `.sln`/`.slnx` solutions with `.vcxproj`
projects and legacy `.sln` solutions containing `.vcproj` projects. Standalone
projects are wrapped as one-project solutions. Generated buildscripts are
written beside the input.

Conversion preserves the supported identity, configuration, compiler/linker,
file, filter, dependency, property-sheet, and build-event data. Visual Studio
projects can contain arbitrary targets and extensions that do not have a
buildscript equivalent, so review the generated files and regenerate/build them
before removing the originals.

Add `--export-deps` to write an HTML dependency report during conversion.

### VPC to buildscript

```text
sighmake convert vpc game.vpc
```

The VPC parser supports a practical subset including macros, conditions,
projects, folders, files, configurations, compiler/linker properties, libraries,
and pre/post-build events. Unsupported VPC keywords are skipped. Review the
result rather than treating conversion as a lossless round trip.

### CMake input

```text
sighmake CMakeLists.txt -g buildscript
sighmake CMakeLists.txt -g vcxproj
sighmake CMakeLists.txt -g makefile
```

The built-in CMake parser is not a replacement for CMake evaluation. It supports
a tested subset including `project`, `add_executable`, `add_library`, `set`,
`option`, `target_sources`, target include/definition/option/link commands,
directory include/link commands, basic properties, `find_package`, and selected
`if`, `foreach`, `while`, function, macro, and list behavior. Generator
expressions and unhandled commands cannot be translated generally. Validate all
converted targets and flags.

## Setting reference

This section is an index, not a promise that every backend consumes every
setting. The `vcxproj` generator has the broadest coverage. Make and CMake use
portable settings plus selected linker, PCH, NASM, event, and custom-rule
features.

Compatibility aliases exist for many imported Visual Studio spellings. Prefer
the primary names shown here in hand-written buildscripts.

### Solution settings

| Setting | Value |
| --- | --- |
| `name` | Solution name. Defaults to the first project name. |
| `configurations` | Comma-separated configuration names. |
| `platforms` | Comma-separated platform names. |
| `defines` | Definitions applied to every project/configuration. Exact/platform selectors are accepted. |
| `include` | Settings fragment path. |

### Project identity and output

| Setting | Purpose |
| --- | --- |
| `type` | `exe`, `lib`, `dll`, `interface`, `sys`, or `sys_lib`. |
| `project_name` | Visual Studio display/project name override. |
| `uuid` | Explicit project GUID/UUID. Stable IDs are generated when omitted. |
| `root_namespace` | Visual Studio root namespace. |
| `toolset` | MSVC alias or platform toolset identifier. |
| `windows_sdk` | Windows target platform version. |
| `charset` | Common values are `MultiByte` and `Unicode`. |
| `target_name`, `target_ext` | Output basename and extension. |
| `outdir`, `intdir` | Output and intermediate directories. |
| `ignore_warn_duplicated_filename` | Suppress the duplicate-filename warning when true. |

### Common compiler settings

| Setting | Typical values or purpose |
| --- | --- |
| `includes` | Comma-separated include directories. |
| `defines` | Comma-separated preprocessor definitions. |
| `forced_includes` | Headers forcibly included by the compiler. |
| `std` | `14`, `17`, `20`, `23`, or `latest`. |
| `language` | `C` or `C++`; normally auto-detected. |
| `c_standard` | `89`/`90`, `99`, `11`, `17`, or `23`. |
| `optimization` | `Disabled`, `MinSpace`, `MaxSpeed`, or `Full`. |
| `warning_level` | `Level0` through `Level4`. |
| `runtime_library` | `MultiThreaded`, `MultiThreadedDebug`, `MultiThreadedDLL`, or `MultiThreadedDebugDLL`. |
| `debug_info` | `EditAndContinue`, `ProgramDatabase`, or `OldStyle`. |
| `exceptions` | `true`/`Sync`, `Async`, or `false`. |
| `rtti` | Boolean runtime type information. |
| `multiprocessor` | Boolean parallel compilation setting. |
| `utf8` | Use UTF-8 source/execution character sets. |
| `cflags` | Raw additional compiler options. |
| `objcflags` | Additional flags for `.m`/`.mm` files. |
| `compile_as` | `Default`, `CompileAsC`, or `CompileAsCpp`. |
| `pch`, `pch_header`, `pch_output` | Precompiled-header policy. |

Other recognized compiler keys include `disable_warnings`, `error_reporting`,
`assembler_listing`, `object_file_name`, `program_database_file`,
`browse_information`, `browse_information_file`, `basic_runtime_checks`, `simd`,
`floating_point`, `inline_function_expansion`, `favor_size_or_speed`,
`string_pooling`, `minimal_rebuild`, `buffer_security_check`,
`force_conformance_in_for_loop_scope`, `function_level_linking`,
`intrinsic_functions`, `generate_xml_documentation_files`,
`treat_wchar_t_as_builtin`, `assembler_output`, `expand_attributed_source`,
`openmp`, and `treat_warning_as_error`.

`optimization`, `runtime_library`, `debug_info`, `function_level_linking`, and
`intrinsic_functions` are configuration settings; use an exact selector or full
configuration section when setting them explicitly.

### Linker and librarian settings

| Setting | Purpose |
| --- | --- |
| `libs`, `link_libs` | Link dependencies. |
| `libdirs` | Library search directories. |
| `ldflags` | Raw linker options. |
| `subsystem` | Common values are `Console`, `Windows`, and `Native`. |
| `ignore_libs` | Specific default libraries to ignore. |
| `ignore_all_default_libraries` | Disable all default libraries. |
| `module_def` | Module definition/version-script path. |
| `generate_debug_info` | Emit linker debug information. |
| `link_incremental` | Incremental linking. |
| `whole_program_optimization` | Whole-program optimization/LTCG. |
| `optimize_references` | Remove unreferenced functions/data. |
| `enable_comdat_folding` | Fold identical COMDAT entries. |

Additional recognized linker keys include `show_progress`, `output_file`,
`suppress_startup_banner`, `program_database_file`, `base_address`,
`target_machine`, `link_error_reporting`, `safe_seh`, `entry_point`,
`link_version`, `generate_map_file`, `map_file_name`, `fixed_base_address`,
`randomized_base_address`, and `large_address_aware`.

`link_incremental`, `whole_program_optimization`, `generate_debug_info`,
`optimize_references`, and `enable_comdat_folding` are configuration settings;
use an exact selector or full configuration section.

Static-library settings use `lib_output_file`, `lib_suppress_startup_banner`,
`lib_use_unicode_response_files`, `libflags`, and
`lib_additional_dependencies`.

### Public dependency settings

`public_includes`, `public_libs`, `public_libdirs`, and `public_defines` publish
usage requirements. They support full configuration/platform and platform-only
selectors.

### Specialized tools

| Tool | Settings |
| --- | --- |
| Resource compiler | `resource_culture`, `resource_defines`, `resource_includes` |
| NASM | `nasm_path`, `nasm_format`, `nasm_flags`, `nasm_includes`, `nasm_defines` |
| Message compiler | `mc_header_dir`, `mc_rc_dir`, `mc_flags` |
| MIDL | `midl_output_dir`, `midl_header`, `midl_type_library`, `midl_dlldata`, `midl_iid`, `midl_proxy`, `midl_flags`, `midl_defines`, `midl_default_char_type`, `midl_target_environment` |
| Manifest | `generate_manifest`, `manifest_suppress_startup_banner`, `manifest_additional_files` |
| Browse/XML tools | `xdcmake_suppress_startup_banner`, `bscmake_suppress_startup_banner`, `bscmake_output_file` |

Additional configuration output keys are `executable_path`,
`ignore_import_library`, and `import_library`.

### Boolean values

Use lowercase `true` and `false`. Most parser branches also accept `yes` and
`1` as true, but boolean handling is intentionally simple and case-sensitive.

## Troubleshooting

### Makefile says there are no projects to build

The solution contains only Windows platforms, often because it relied on the
default `Win32, x64` matrix. Add `Linux`, `macOS`, or `Android` as appropriate,
then regenerate.

### `sighmake --build` cannot find a cache

Run generation first and pass the directory containing `.sighmake_cache`, not
the nested `build/` directory:

```text
sighmake app.buildscript
sighmake --build .
```

### A configuration-specific value is ignored

Use `setting[Config|Platform]` or `[config:Config|Platform]`. A one-token bracket
such as `[Debug]` is treated as a platform selector. Per-file qualifiers also
need a full selector or `[*]`.

### A path, flag, or dependency contains comment text

Move the comment to its own line. Ordinary inline comments are not stripped.

### A wildcard finds no files

Paths are relative to the buildscript containing the setting. Use `dir/*.cpp`
for one directory and `dir/**/*.cpp` for recursive traversal. Wildcards expand
at generation time, so regenerate after adding files.

### A package is not found

Run with `SIGHMAKE_DEBUG=1`, check the environment variable used by the package
finder, and verify the expected SDK directory or `pkg-config` package exists.
Remember that discovery occurs on the generation host.

### Visual Studio generation fails before writing files

The `vcxproj` generator requires a detected Visual Studio installation. Run from
a machine with the C++ workload installed, inspect `sighmake --list-toolsets`,
and remove a requested toolset that is newer than the installed Visual Studio.

### Generated files do not reflect a changed buildscript

Run generation again. sighmake does not watch files, and `--build` consumes the
previously generated backend plus its cache.

### A setting works in Visual Studio but not Make or CMake

Many advanced keys represent MSBuild properties. Prefer the portable settings
in the main tables, guard raw flags by host/configuration, and inspect the
generated Makefile or CMakeLists for the exact feature you need.
