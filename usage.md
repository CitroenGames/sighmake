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
- [SIMD and vectorization](#simd-and-vectorization)
- [Generators and direct builds](#generators-and-direct-builds)
- [Android](#android)
- [Conversion](#conversion)
- [Setting reference](#setting-reference)
- [C# projects](#c-projects)
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
sighmake --version
```

`--build`, `--convert`, `convert`, `update` (alias `--update`), and
`--version`/`-V` are recognized only as the first argument. Placing them after
an input file produces an "Unexpected argument" error.

`<input-file>` may be a `.buildscript`, `CMakeLists.txt`, or `.cmake` file. A directory is rejected. An argument that starts with `-` and is not a known option is an "Unknown option" error; put `--` before an input file whose name starts with `-`.

### Generation options

| Option | Meaning |
| --- | --- |
| `-g`, `--generator <type>` | Select `vcxproj`, `makefile`, `cmake`, or `buildscript`. |
| `-B`, `--build-dir <dir>` | Set the Visual Studio output subdirectory. The default is `build`. Other generators ignore this option with a warning. |
| `-D <NAME>=<VALUE>` | Define a value used by `${NAME}` substitutions while parsing a buildscript. Repeat for multiple variables. |
| `-t`, `--toolset <name>` | Set the default MSVC toolset for projects that do not specify one. |
| `--target-os <os>` | Evaluate `if(...)` blocks and `[tag]` conditions for this OS instead of the machine running sighmake: `windows`, `linux`, `macos` (`osx`, `darwin`), `ios`, or `android`. See [Cross-generating for another OS](#cross-generating-for-another-os). |
| `--cxx <compiler>`, `--cc <compiler>` | Makefile generator: the C++ / C compiler driver written as the default of `CXX` / `CC` (default `g++` / `gcc`, `clang++` / `clang` on macOS). Overrides the `cxx` / `cc` solution keys. `CXX=...` / `CC=...` in the environment or on the make command line still override the generated value. |
| `--export-deps` | Write `<Solution>_dependencies.html` in the output root. |
| `--keep-stale` | Keep generated files of earlier runs that this run no longer writes, instead of removing them. See [Stale outputs](#stale-outputs). |
| `--prune-unlisted` | Also remove files that look like sighmake outputs but are in no generation manifest (outputs of a sighmake version without manifests). See [Stale outputs](#stale-outputs). |
| `-l`, `--list` | List registered generators. |
| `--list-toolsets` | List recognized `msvcYYYY` toolset aliases. |
| `-h`, `--help` | Show command help. |
| `-V`, `--version` | Show the version and release repository. First argument only. |

Examples:

```text
sighmake app.buildscript -g cmake
sighmake app.buildscript -g vcxproj -B vs-build
sighmake app.buildscript -t msvc2022
sighmake app.buildscript -D SDK_ROOT=C:/SDK
sighmake app.buildscript --export-deps
```

Any input file that is not named `CMakeLists.txt` or `*.cmake` is parsed as a
buildscript regardless of its extension. Output is always written relative to
the current working directory, not beside the input file; there is no separate
output-directory option.

`-D` is two arguments: the option and `NAME=VALUE`. Undefined `${NAME}`
references are replaced with an empty string; `${NAME:-default}` supplies an
explicit fallback (see [Variables](#variables)). Values discovered later by
`find_package()` replace a command-line variable with the same name.

### Build options

| Option | Meaning |
| --- | --- |
| `-b`, `--build <dir>` | Build from the cache in `<dir>`. This form must be the first command argument. |
| `--config <name>` | Select a configuration such as `Debug` or `Release`. |
| `--platform <name>` | Select a generated Visual Studio platform such as `x64` or `Win32`; `x86` is an alias for `Win32`. With a Makefile cache, `Android` selects the Android build (the `<Config>.Android` targets). |
| `--abi <abi>` | Makefile builds for Android: set `ANDROID_ABI` (`arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`). |
| `--api <level>` | Makefile builds for Android: set `ANDROID_API`. |
| `--aab` | Makefile builds for Android: also build the `.aab` app bundle of `apk` projects (`ANDROID_AAB=1`), see [Android packages](#android-packages-apk). |
| `--target <name>` | Pass a target to the active backend. With MSBuild it combines with `--project` and `--clean-first` (`/t:Clean;<name>`). For Make and CMake it cannot be combined with `--project` (error); with Make the target names the configuration itself, so `--config` has no effect (a warning is printed). |
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

The CMake generator does not retain the build-time platform matrix, so its
`--build` route rejects `--platform` with an explanation; select the platform
while configuring CMake instead. A Makefile cache has one build per OS family:
`--platform Android` builds the Android targets (`Release.Android`,
`Game.Release.Android` with `--project`) and any desktop platform name builds
the host targets. Without `--platform`, a cache that only contains Android
builds Android and a mixed cache builds the host targets. See
[Building Android from sighmake](#building-android-from-sighmake).

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
| `SIGHMAKE_TARGET_OS` | Target OS for conditions when `--target-os` is not given (same values). Unknown values are ignored with a warning. |
| `SIGHMAKE_MAKE` | The `make` program `sighmake --build` runs for Makefile builds. Default: `make` from `PATH`, else the NDK's make for Android builds. |
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

A settings-only fragment (keys, `[config:...]` sections and conditional blocks,
no `[project:...]` or `[solution]` section) is applied every time it is
included, so several projects can share one: two library variants that must
compile with identical flags each include the same fragment and add only what
differs. A file that declares projects is included once; a repeated include of
it is skipped with a warning.

```ini
[project:Widgets]
include = widgets_settings.buildscript
defines = WIDGETS_TEST_HOOKS

[project:WidgetsNoHooks]
include = widgets_settings.buildscript
```

### Variables

Buildscript substitution uses `${NAME}`:

```ini
includes = ${SDK_ROOT}/include
libdirs = ${SDK_ROOT}/lib
```

Variables come from `-D` and `find_package()`. Buildscripts do not currently
provide a general variable-assignment statement.

An undefined variable expands to an empty string. `${NAME:-default}` gives the
substitution an explicit fallback that is used when `NAME` is undefined or empty,
so data owned by a game (an orientation, a store label, an extra permission) can
fill a generated manifest while the buildscript still works without it:

```ini
android_label = ${APP_TITLE:-My Game}
android_activity_attrs = android:screenOrientation="${ORIENTATION:-landscape}"
android_permissions = INTERNET, ${EXTRA_PERMISSION:-VIBRATE}
```

```text
sighmake -g makefile --target-os android game.buildscript -D ORIENTATION=portrait -D APP_TITLE="Chess Night"
```

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

Either side of a full selector may be `*`: `[*|Android]` is every current
configuration on Android, `[Release|*]` is Release on every current platform and
`[*]` is the whole matrix.

```ini
outdir[*|Android] = bin/android/$(ANDROID_ABI)
treat_warning_as_error[Release|*] = true
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

### Accumulating and replacing settings

Whether a repeated setting adds to or replaces the previous value depends on
how it is written, not only on the key:

| Form | `cflags`, `ldflags`, `objcflags`, `forced_includes` | `includes`, `defines`, `libs`, `libdirs` |
| --- | --- | --- |
| Unqualified project setting | Appends | Appends |
| Exact or platform selector, `[*]` | Replaces the selected cells | Appends |
| `[config:...]` section | Replaces that cell | Appends |

So `cflags = -Wall` followed by `cflags[Release|x64] = -O3` leaves Release x64
with only `-O3`. Put every flag the cell needs on the selector line, or use
separate keys (for example `optimization`) for the configuration-specific part.

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

Always include the platform in a configuration section; `[config:Debug]` is not
expanded over platforms. Use a wildcard section (below) for that.

#### Wildcard configuration sections

`*` stands for "every configuration" or "every platform" in the solution matrix
declared so far, so one block replaces several near-identical ones:

```ini
[solution]
configurations = Debug, Release
platforms = x64, Android

[project:Game]
type = exe

[config:Debug|x64]
outdir = bin/x64/Debug

[config:Release|x64]
outdir = bin/x64/Release

# One block for Debug|Android and Release|Android
[config:*|Android]
outdir = bin/android/$(ANDROID_ABI)/$(Configuration)
defines = ON_DEVICE
```

Forms: `[config:*|Android]`, `[config:Release|*]`, `[config:*|*]` and `[config:*]`
(the last two are the whole matrix). Every setting in the block is applied to each
matching cell exactly as if the section were written once per cell, so the usual
accumulate/replace rules apply. A literal platform that is not part of the matrix
yet is added to it, and the platforms and configurations declared in `[solution]`
stay in the matrix. Declare the matrix before using wildcards; configurations
introduced later are not covered.

Configuration sections add to the matrix declared in `[solution]`; they never
remove from it. A configuration or platform named only in a section joins the
matrix, and a declared one without a section of its own stays and gets the
defaults (`Debug` keeps `-O0 -g` in Makefiles when only `[config:Release|Linux]`
is written). When `[solution]` declares no `configurations` (or no `platforms`),
the names (or platforms) appearing in sections form that dimension instead of the
built-in defaults. sighmake forms the cross-product and fills unset cells with
defaults.

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

### Default `NDEBUG` / `_DEBUG`

Every generator (Visual Studio, Makefile, CMake) applies the same industry-standard default
to configurations named `Release` and `Debug`: `Release` compiles with `NDEBUG` (asserts off)
and `Debug` with `_DEBUG`. The default is skipped when the project already lists `_DEBUG` or
`NDEBUG` for that configuration (in any spelling of the `defines` setting that reaches it), so
a project can keep asserts live in Release by defining `_DEBUG`, or define `NDEBUG` itself.
Other configuration names (`Profile`, `Retail`, ...) get no default. Because the define is part
of the compiler flags, the Makefile per-target flag stamps change and stale objects rebuild
when upgrading sighmake. Custom spellings such as `-UNDEBUG` in `cflags` are not detected;
define `_DEBUG` instead.

For readability, define the base before the derived section. Use full
`Config|Platform` section names for predictable inheritance.

Name a full `Config|Platform` cell to inherit from another platform. This is the
"same as Release|x64 with a different outdir" case:

```ini
[config:Release|x64]
optimization = MaxSpeed
warning_level = Level4
outdir = bin/x64/Release

[config:Release|Android] : Template:Release|x64
outdir = bin/android/$(ANDROID_ABI)/Release
```

In a wildcard section `*` in the template stands for the derived cell's
configuration, so `[config:*|Android] : Template:*|x64` makes every Android cell
copy the x64 cell of the same configuration. As with every template, a setting
is copied only when the derived cell has not set it (list settings are copied
only when the derived list is empty).

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

Supported OS conditions are `windows`, `linux`, `osx`/`macos`/`darwin`, `ios`, and
`unix`/`posix`. Their `!` negations are also supported. The OS is the machine
running sighmake unless `--target-os` or `SIGHMAKE_TARGET_OS` selects another
one (see [Cross-generating for another OS](#cross-generating-for-another-os)).
`[windows]`-style tags on file lists use the same evaluation.

#### Host conditions

`windows`, `linux`, `posix`, ... describe the **target** OS (see `--target-os`).
Prebuild/postbuild commands, however, run on the machine that builds. Use the
`host_` conditions to choose commands for that machine; they ignore
`--target-os`:

| Condition | True when the machine running sighmake/the build is |
| --- | --- |
| `host_windows` | Windows |
| `host_linux` | Linux |
| `host_macos` (`host_osx`, `host_darwin`) | macOS |
| `host_posix` (`host_unix`) | Linux or macOS |

Their `!` negations are supported. `env(NAME)` is true when the environment
variable `NAME` is set at generation time to something other than empty, `0`,
`false`, `no` or `off` (`!env(NAME)` negates). It switches a pre-build step, a
dependency or any block on a build-time switch:

```ini
if(!env(SKIP_DEPCHECK)) {
    prebuild = tools/depcheck --all
}
if(host_posix) {
    if(env(CI)) {
        defines = RUNNING_ON_CI
    }
}
```

Conditions do not combine with `&&`; nest the
blocks instead:

```ini
if(android) {
    if(host_windows) {
        prebuild = python tools/gen_assets.py
    }
    if(host_posix) {
        prebuild = python3 tools/gen_assets.py
    }
}
```

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

`!Win32` selects `x64` on Windows and `!x64` selects `Win32`. `!android` is true
unless the target OS is `android`; it applies to every configuration, including
Android ones, when generating for the host OS. Conditions may be nested, and the opening brace may appear on the
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

These conditions select files using the host running sighmake. The Android tags
are different, they scope a file to a platform instead of evaluating the host:
`[android]` keeps a file for the Android configurations only and `[!android]` for
every platform but Android. A file scoped away from a platform does not appear in
that platform's build (and not in the Visual Studio project when no Windows
platform keeps it):

```ini
sources = {
    src/common.cpp
    src/android_main.cpp [android]
    src/desktop_main.cpp [!android]
}
```

For other per-target splits use separate generation runs or explicit per-file
exclusion settings.

#### Platform-scoped declarations

A platform block (`if(Android) { ... }`, `if(x64) { ... }`) scopes more than
configuration settings:

| Inside `if(Android) { ... }` | Effect |
| --- | --- |
| `target_link_libraries(...)`, `dependencies`, `depends`, `project_references` | The edge exists for Android configurations only. The other platforms neither link the project, nor receive its `public_*` settings, nor order the build after it. The Visual Studio, CMake and `--export-deps` outputs omit the edge (the Makefile build keeps it), so a desktop project file is byte-identical to one whose Android-only blocks were never written. |
| `sources`, `src`, `files` | The files are compiled for Android only (same as an `[android]` tag per file). |
| `type = dll` | Sets the project type for that platform's cells only; other platforms keep the type declared outside the block. |
| `libs`, `libdirs`, `defines`, `includes`, `outdir`, ... | Configuration settings of the Android cells (as before). |

```ini
[project:Game]
type = exe
sources = src/*.cpp
target_link_libraries(
    Engine PUBLIC
)
if(Android) {
    # libgame.so loaded by the NativeActivity
    type = dll
    sources = src/android/*.cpp
    # SDL3: an Android-only external_cmake project
    target_link_libraries(
        SDL3 PUBLIC
    )
}
```

Declaring the same dependency in several blocks widens its scope to the union of
the platforms; declaring it outside any block makes it unconditional.

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
| `external_cmake` | A third-party CMake project that is configured and built with `cmake` (Makefile generator only), see [External CMake projects](#external-cmake-projects). |

Canonical type names above are preferred. Accepted compatibility aliases include
`application`, `static`, `staticlib`, `shared`, `dynamiclib`, `header-only`,
`driver`, and `kernel_lib`, as well as the raw MSBuild spellings `Application`,
`StaticLibrary`, `DynamicLibrary`, `Utility`, and `Driver`.

`sys` and `sys_lib` primarily target Visual Studio. Other generators can emit a
file with a corresponding type, but they do not provide a Windows driver
toolchain.

### External CMake projects

A project of type `external_cmake` builds a third-party CMake source tree instead of
compiling sources listed in the buildscript, and exposes what it produces to the
projects that depend on it. It is supported by the Makefile generator (including
Android); the Visual Studio generator leaves such projects out and the CMake
generator emits an `INTERFACE` placeholder.

```ini
[project:SDL3]
type = external_cmake

# The source: a pinned git checkout ...
cmake_git = https://github.com/libsdl-org/SDL.git
cmake_git_tag = release-3.2.8
# ... or a tree already on disk (use one of the two):
# cmake_source = third_party/SDL

cmake_options = -DSDL_SHARED=ON, -DSDL_STATIC=OFF, -DSDL_TEST_LIBRARY=OFF
cmake_targets = SDL3-shared
cmake_outputs = libSDL3.so

public_includes = build/_external/SDL3/source/include
outdir[*|Android] = bin/android/$(ANDROID_ABI)

[project:Game]
type = shared
sources = src/*.cpp
target_link_libraries(
    SDL3 PUBLIC
)
```

| Setting | Meaning |
| --- | --- |
| `cmake_source` | Local CMake source tree, relative to the buildscript. |
| `cmake_git`, `cmake_git_tag` | Repository to clone and the tag, branch or commit to check out. A hexadecimal commit id (7-40 digits) is fetched with `git fetch --depth 1`; anything else is cloned with `--branch`. A repository without a tag is not pinned and produces a warning. |
| `cmake_checkout_dir` | Where the clone goes. Default: `build/_external/<Project>/source` next to the buildscript (shared by all configurations and ABIs). |
| `cmake_source_subdir` | Directory inside the source tree that holds `CMakeLists.txt`. |
| `cmake_options` | Comma-separated extra `cmake` arguments, usually `-DNAME=VALUE`. Make variables such as `$(ANDROID_ABI)` expand. |
| `cmake_targets` | Comma-separated `--target` names; default: the default target. |
| `cmake_outputs` | Comma-separated files the build produces, relative to the cmake build directory. Each is copied (only when it changed) into the project's `outdir`. |
| `cmake_generator` | The `cmake -G` generator. Default: Ninja when `ninja` is installed, else `Unix Makefiles` (with the running make as `CMAKE_MAKE_PROGRAM`). `make CMAKE_GENERATOR=...` overrides it. |
| `cmake_build_type` | `CMAKE_BUILD_TYPE` for every configuration. Default: the configuration name (`Debug`, `Release`, ...). |
| `cmake_build` | `false` skips the `cmake --build` step: the tree is fetched and configured only (header-only trees, or configure-time generated headers). Default `true`. |

Header-only trees are supported: leave out `cmake_outputs` (nothing is copied or
checked), expose the headers with `public_includes`/`public_defines` and list the project in
`target_link_libraries` of the projects that use it.

The make job limit reaches cmake: `cmake --build ... --parallel N` uses the `N` of
`sighmake --build ... -j N`, of a plain `make -jN`, or of `make SIGHMAKE_JOBS=N`
(a bare `--parallel` without any limit).

Every configuration (and every Android ABI) configures into its own build directory,
`<intdir>` of the project (default `build/<Config>/android/$(ANDROID_ABI)/obj/<Project>`),
so ABIs and configurations never share a CMake cache. The makefile re-runs the
(idempotent) cmake configure and build on every `make`, so changes to
`cmake_options` and to the third-party sources are always picked up, and missing
outputs fail the build with a message naming the file.

A pinned checkout is tracked with a stamp file (`<checkout dir>.pin` holding
`<url>@<ref>`); changing `cmake_git` or `cmake_git_tag` in the buildscript deletes
the old checkout and fetches the new one. Delete the checkout directory to force a
fresh clone. Use `make clean` to remove build directories and copied outputs (the
checkout is kept).

Projects that list an `external_cmake` project in `target_link_libraries` build after it
(`dependencies` only orders the build), and every produced `.a`, `.so`, `.dylib` or `.lib` in `cmake_outputs`
becomes a link input of the dependant (`public_includes` and `public_defines` propagate as
for any project). Other produced files are only copied. For Android the build uses the NDK
toolchain file with `ANDROID_ABI`, `ANDROID_PLATFORM=android-$(ANDROID_API)` and, for the
64-bit ABIs, `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (16 KB alignment, see
[Shared libraries](#shared-libraries-and-executables)). `CMAKE=` and `GIT=` select other
programs. `cmake` must be installed (the copy bundled with the Android SDK lives in
`<sdk>/cmake/<version>/bin`).

### Android packages (APK)

A project of type `apk` (alias `android_apk`) assembles an installable Android package from the shared libraries
of the projects it depends on, using the Android SDK build-tools (`aapt2`, `d8`, `zipalign`, `apksigner`,
found through `ANDROID_HOME`/`ANDROID_SDK_ROOT`, with `ANDROID_BUILD_TOOLS` and `ANDROID_JAR` as overrides).
It exists only for the Android platform and is dropped from every other cell of the matrix. Makefile
generator only. Nothing is installed or run on a device.

```ini
[project:GameApk]
type = apk
android_package = com.example.game
android_label = My Game
# NativeActivity loads libgame.so
android_native_lib = game
# stored uncompressed under assets/
android_assets = content
android_permissions = INTERNET
target_name = Game
outdir = bin/android
# type = dll projects (and external_cmake .so outputs) are packaged
target_link_libraries(
    GameLib PUBLIC
)
```

| Setting | Meaning |
| --- | --- |
| `android_package` | applicationId (required unless `android_manifest` is set). |
| `android_label`, `android_icon`, `android_version_code`, `android_version_name` | Application label (default: project name), `@mipmap/...` icon, version. |
| `android_activity` | Launcher activity (default `android.app.NativeActivity`, needs no Java). |
| `android_native_lib` | `lib<name>.so` loaded by NativeActivity (default `main`). |
| `android_min_sdk`, `android_target_sdk` | Default: `ANDROID_API`, and the newest installed platform. |
| `android_manifest` | Use your own AndroidManifest.xml instead of the generated one. `${NAME}` and `${NAME:-default}` references inside it are expanded from `-D` / `find_package` variables into a copy next to the makefiles (a manifest without `${` is used in place), so an engine does not have to render its template itself. |
| `android_extract_native_libs` | `android:extractNativeLibs` of the generated manifest. Default `false` (libraries are mapped straight out of the APK); `true` makes the installer extract them to the app's `nativeLibraryDir`, needed by code that `dlopen()`s the library by absolute path. |
| `android_res`, `android_assets` | Resource directory; asset directories merged into `assets/`. |
| `android_java`, `android_jars` | Java source directories (compiled with `javac`, dexed with `d8`) and extra jars. Prebuilt Java in an SDK layout works without sources: list jars or wildcards (`android_jars = sdk/java/*.jar`); a `.jar` or `*.jar` entry of `android_java` counts as a jar. Jars are compiled against and dexed into the package. |
| `android_native_libs` | Extra prebuilt `.so` files. The path may contain `$(ANDROID_ABI)` and `$(Configuration)` / `$(Platform)`, so Debug and Release share one spelling (`prebuilt/$(Configuration)/$(ANDROID_ABI)/libfoo.so`). Tag an entry `[optional]` to skip it, with a notice, when the file does not exist. |
| `android_aab` | `true` also builds `<target_name>.aab` (Android App Bundle) next to the `.apk`, see below. Default `false`; `--aab` / `ANDROID_AAB=1` enable it for one build. |
| `android_permissions`, `android_features` | `uses-permission` names (`android.permission.` optional); `uses-feature` entries: `name`, `name:false`, `glEsVersion=0x00030000`, and `name key=value ...` for features with more attributes, for example `android.hardware.vulkan.version version=0x00401000` or `android.hardware.vulkan.level level=1 required=false`. |
| `android_cpp_shared` | Bundle `libc++_shared.so`: `auto` (when a library references it), `true`, `false`. |
| `android_activity_attrs`, `android_application_attrs` | Raw extra attributes. |

Native libraries are stored uncompressed and zipaligned (16 KB with build-tools 35+; the manifest sets
`extractNativeLibs=false`). Signing: Release uses the keystore in `ANDROID_KEYSTORE` with
`ANDROID_KEYSTORE_PASSWORD` (optional `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`); other configurations
use `~/.android/debug.keystore`, created with `keytool` when missing. `ANDROID_SIGN=debug|release`
overrides this. The result is verified with `apksigner verify` and `zipalign -c`.

Several ABIs: `sighmake --build ... --platform Android --abi arm64-v8a,x86_64` builds each ABI and packages
all of them into one APK (make variables `ANDROID_APK_ABIS`, `SIGHMAKE_SKIP_PACKAGING`). The package path
is well defined: with one ABI it is `<outdir>/<target_name>.apk` (`$(ANDROID_ABI)` in `outdir` expands to that
ABI), with several ABIs it is a single combined package at `<outdir>/<target_name>.apk` where
`$(ANDROID_ABI)` in `outdir` is replaced by `universal` (`bin/android/$(ANDROID_ABI)` becomes
`bin/android/universal`). An `outdir` without `$(ANDROID_ABI)` is used unchanged. The same rule applies to the `.aab`.

**Android App Bundle (`.aab`).** With `android_aab = true` (or `sighmake --build ... --aab`, or
`make ANDROID_AAB=1`, or `make aab`) the package project also writes `<target_name>.aab`. It runs
`aapt2 link --proto-format` on the manifest and resources, lays the base module out as a bundle module
(`manifest/AndroidManifest.xml`, `resources.pb`, `res/`, `dex/`, `lib/<abi>/`, `assets/`), builds it with
`bundletool build-bundle` (native libraries stored uncompressed) and signs it with `jarsigner`
(SHA256withRSA): Release uses `ANDROID_KEYSTORE`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and the
optional `ANDROID_KEY_PASSWORD`; other configurations use the debug keystore (same `ANDROID_SIGN` override as
the APK). `jarsigner` comes from `JAVA_HOME`. bundletool is not part of the Android SDK: set `BUNDLETOOL` to
`bundletool-all-<version>.jar` (or to a bundletool launcher), or put `bundletool`, `bundletool.jar` or
`bundletool-all.jar` on `PATH`. When none is found the bundle step stops with a message saying so; the
APK is built first and is not affected. Download it from <https://github.com/google/bundletool/releases>.

### Platform filter

By default every project is built for every platform in the solution matrix.
Declare `platforms` in a project to restrict it:

```ini
[project:D3D12RHI]
type = lib
platforms = Windows

[project:MetalRHI]
type = lib
platforms = macOS, iOS

[project:GameplayTools]
type = lib
platforms = !Android
```

Entries are comma-separated and case-insensitive. They are platform names
(`x64`, `Linux`, `Android`, ...) or groups: `Windows` (`Win32`, `x64`, `ARM`,
`ARM64`), `x86` (`Win32`), `macOS` (`osx`, `darwin`), and `posix` (Linux, macOS,
Android, iOS). A leading `!` excludes. Without positive entries every platform
except the excluded ones is kept.

After parsing, the project's configurations for all other platforms are removed,
so the Makefile generator writes no makefile or `make` target for them, and the
Visual Studio generator leaves them out of the solution configurations; a project
left with no Visual Studio platform is not generated at all. `target_link_libraries`
edges to a project that does not exist on a platform are ignored for that
platform. The CMake generator does not use the filter.

### Source inputs

| Setting | Files |
| --- | --- |
| `sources` | C (`.c`), C++ (`.cpp`, `.cc`, `.cxx`), Objective-C/Objective-C++ (`.m`, `.mm`), C# (`.cs`, auto-detected), or other source entries. |
| `headers` | `.h`, `.hpp`, `.hh`, and `.hxx`. |
| `resources` | Windows resource (`.rc`) files. |
| `masm` | MASM (`.asm`, `.masm`) files. |
| `nasm` | NASM (`.asm`, `.nasm`) files. |
| `mc` | Windows Message Compiler (`.mc`) files. |
| `idl` | MIDL (`.idl`) files. |
| `xaml` | WPF XAML (`.xaml`) files for C# projects. |
| `embedded_resources` | .NET embedded resources (`.resx`, `.resw`) for C# projects. |

`masm[x64]` and `nasm[x64]` add files for the selected platform and exclude them
from the others in the current matrix.

Backend support differs: Visual Studio handles all Windows-specific input types;
Make compiles C, C++, Objective-C/Objective-C++, and NASM; CMake emits C/C++,
Objective-C/Objective-C++, resource, MASM, and NASM rules. Verify generated
output when using specialized tools.

### C and C++ language selection

Native projects auto-detect their language from source extensions. Any C++ source
makes the project C++; a project containing only `.c` sources is C. `.cs` sources
select [C# project generation](#c-projects); native and C# sources must be in
separate projects.

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

`message` is accepted as an alias of `description`, and `additional_inputs`
as an alias of `inputs`. The single-line form is also accepted when each
argument is comma-separated. Visual Studio and CMake emit custom build rules. The Makefile generator currently
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

`depends` (alias `dependencies`) declares a build-order dependency only. The
referenced projects are built first, but nothing is linked and no usage
requirements propagate:

```ini
[project:App]
depends = CodeGenerator
```

To link against another project and inherit its public properties, use
`project_references` or `target_link_libraries()`:

```ini
[project:App]
project_references = Core, Graphics
```

`project_references` creates public link dependencies. For explicit visibility,
use `target_link_libraries()` inside the consuming project. Unlike CMake syntax, the current project name is not an argument:

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

Bare library names are backend-translated where possible. A `libs` or `link_libs`
entry that names a file by path (it contains a slash or backslash, for example
`deps/lib/libfoo.a`) is relative to the declaring buildscript, exactly like
`libdirs` and `outdir`, in project level and in `[config:...]` sections alike; the
Makefile generator writes it relative to the makefile. Plain names (`log`), linker
flags (`-framework Metal`) and entries that contain a macro (`$(NDK)/libfoo.a`)
are left as written. Raw `cflags` and
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
defines `${Package_LIBRARY_DIRS}`, `${Package_LIBRARY_DIRS_X86}`,
`${Package_LIBRARY_DIRS_X64}`, and `${Package_VERSION}`. Values containing multiple entries are semicolon-separated,
so automatic dependency propagation is preferable to copying those variables
into comma-separated buildscript lists.

On Windows the finders use installed system SDKs and variables such as
`VULKAN_SDK`, `SDL2_DIR`/`SDL2`, `SDL3_DIR`/`SDL3`, and `DXSDK_DIR`. On Linux and
macOS they first use `pkg-config` where applicable, then check standard paths.
DirectX finders are Windows-only. Discovery runs on the host executing sighmake,
not on a remote or Android target.

## SIMD and vectorization

sighmake has one dedicated SIMD setting, `simd`, plus the generic `cflags`
setting for compiler-specific vector flags. Which one you need depends on the
generator.

### The `simd` setting (Visual Studio)

`simd` (alias `enhanced_instruction_set`) maps directly to the MSVC
`EnableEnhancedInstructionSet` property, which is `/arch:` on the compiler
command line. The value is the MSBuild enum name, not the raw flag:

| `simd` value | MSVC flag | Notes |
| --- | --- | --- |
| `NotSet` | none | Compiler default: SSE2 on x64, no SIMD on Win32. |
| `StreamingSIMDExtensions` | `/arch:SSE` | Win32 only. |
| `StreamingSIMDExtensions2` | `/arch:SSE2` | Win32 only; already the default on x64. |
| `AdvancedVectorExtensions` | `/arch:AVX` | |
| `AdvancedVectorExtensions2` | `/arch:AVX2` | Most common choice for modern x64 builds. |
| `AdvancedVectorExtensions512` | `/arch:AVX512` | |
| `NoExtensions` | `/arch:IA32` | Win32 only; disables SSE code generation. |

The value is written into the project file verbatim, so any name accepted by
your Visual Studio version works even if it is not listed above.

Enable AVX2 for every configuration and platform of a project:

```ini
[project:Math]
type = lib
sources = src/*.cpp
simd = AdvancedVectorExtensions2
```

`simd` is applied to every cell of the solution matrix that exists when the line
is parsed, so declare the `[solution]` matrix first. Use an exact selector when
only some cells should be vectorized. This is the usual way to keep a Win32
build on SSE2 while x64 uses AVX2:

```ini
[solution]
name = Vectors
configurations = Debug, Release
platforms = Win32, x64

[project:Math]
type = lib
sources = src/*.cpp
simd[Debug|Win32] = StreamingSIMDExtensions2
simd[Release|Win32] = StreamingSIMDExtensions2
simd[Debug|x64] = AdvancedVectorExtensions2
simd[Release|x64] = AdvancedVectorExtensions2
```

`simd` is also accepted inside full configuration sections and templates:

```ini
[project:Math]
type = lib
sources = src/*.cpp

[config:Release|x64]
optimization = MaxSpeed
simd = AdvancedVectorExtensions2
floating_point = Fast
```

`simd` is not a per-file setting. If only a few translation units need a higher
instruction set, put them in a separate static library project with its own
`simd` value, or pass the flag per file with `cflags`:

```ini
src/kernels_avx2.cpp:cflags = /arch:AVX2
```

The Visual Studio converters preserve the setting: an imported `.vcxproj` with
`EnableEnhancedInstructionSet` produces a `simd = ...` line, and old `.vcproj`
values map to `StreamingSIMDExtensions` and `StreamingSIMDExtensions2`. The VPC
converter does not translate `/arch:` flags.

### Make and CMake

The Make and CMake generators ignore `simd`; they do not translate the MSBuild
enum into GCC or Clang flags. Pass the compiler flags through `cflags` instead.
An unqualified `cflags` line appends to any flags already set; a selector such
as `cflags[Linux]` replaces the flags for the selected cells (see
[Accumulating and replacing settings](#accumulating-and-replacing-settings)):

```ini
[project:Math]
type = lib
sources = src/*.cpp

# Windows/MSVC generators read this...
simd = AdvancedVectorExtensions2

# ...and Make/CMake read these.
cflags[Linux] = -mavx2 -mfma
cflags[macOS] = -mavx2 -mfma
```

Common GCC/Clang equivalents:

| Goal | Flag |
| --- | --- |
| SSE4.2 | `-msse4.2` |
| AVX | `-mavx` |
| AVX2 with FMA | `-mavx2 -mfma` |
| AVX-512 (foundation) | `-mavx512f` |
| Everything the build machine supports | `-march=native` |
| ARM NEON on 32-bit ARM | `-mfpu=neon` (NEON is always on for AArch64) |

`-march=native` produces binaries that may not run on other machines; use it
only for local builds or benchmarks.

Because `simd` is silently ignored outside Visual Studio and `cflags` values
such as `-mavx2` are rejected by MSVC, keep the two on separate platforms, as
above, or guard raw flags with a host condition:

```ini
if(windows) {
    simd = AdvancedVectorExtensions2
}
if(!windows) {
    cflags = -mavx2 -mfma
}
```

### Per-file SIMD dispatch

A common pattern is to compile most sources for a baseline and only the vector
kernels for a newer instruction set, then choose at runtime with `__cpuid` or
`__builtin_cpu_supports`. Per-file `cflags` accept a full `Config|Platform`
selector or `[*]`, and comma-separated values become separate flags:

```ini
[project:Math]
type = lib
sources = src/*.cpp

src/kernels_avx2.cpp:cflags = /arch:AVX2
src/kernels_avx512.cpp:cflags = /arch:AVX512
```

Per-file `cflags` are currently emitted only by the Visual Studio generator.
The Make and CMake generators honor per-file `defines`, `excluded`, `pch`,
`compile_as`, and `object_file`, but not per-file compiler flags. For a
portable per-file instruction set, split the kernels into their own static
library project and give that project the flags:

```ini
[project:MathKernelsAVX2]
type = lib
sources = src/kernels_avx2.cpp
if(windows) {
    simd = AdvancedVectorExtensions2
}
if(!windows) {
    cflags = -mavx2 -mfma
}

[project:Math]
type = lib
sources = src/*.cpp
src/kernels_avx2.cpp:excluded = true
depends = MathKernelsAVX2
```

The same per-file flags can be set through a `[file:...]` section or
`file_properties` block; see [Per-file settings](#per-file-settings).

### Related settings

| Setting | Purpose |
| --- | --- |
| `floating_point` (`fp_model`) | `Precise`, `Strict`, or `Fast`. `Fast` lets MSVC reorder and contract vector math; the GCC/Clang equivalent is `cflags = -ffast-math`. |
| `intrinsic_functions` | Boolean; enables `/Oi` so MSVC inlines intrinsic calls. Configuration setting, so use an exact selector. |
| `openmp` | Boolean; enables OpenMP, including `#pragma omp simd`, in Visual Studio. |
| `favor_size_or_speed` | `Speed` or `Size`; influences whether MSVC's auto-vectorizer is worth enabling. |

For Android builds, the NDK toolchain selects the ABI's baseline (NEON on
`arm64-v8a`); add `cflags` inside `if(Android)` if a higher level is required.
Also note that AVX intrinsics in a translation unit compiled without the
matching flag fail to compile on GCC/Clang and can crash at runtime on MSVC when
the CPU lacks the extension, so keep flags and intrinsics in the same file.

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
project and solution files carry a trailing underscore, for example
`App_.vcxproj` and `Solution_.sln`.

Next to each `.vcxproj` the generator also writes
`Write-SighmakeTargetReceipt.ps1` and `<Project>.runtime-dependencies.txt`. The
receipt step runs `powershell -NoProfile -ExecutionPolicy Bypass` after each
build, so PowerShell must be available on the build machine.

An explicit project toolset wins. Otherwise `-t` or
`SIGHMAKE_DEFAULT_TOOLSET` wins, followed by the detected installation's
toolset. `--list-toolsets` prints aliases such as `msvc2022`; raw identifiers
such as `v143` are also accepted. Unknown identifiers are passed through
verbatim without a warning, so check the spelling of `-t` values yourself.

Two toolset checks happen against the detected Visual Studio:

- A toolset newer than the installed Visual Studio is an error and stops
  generation.
- A known toolset that is not installed is retargeted to the detected toolset
  with a warning.

The build cache records one entry per project and one per distinct
`target_name`, so `sighmake --build --project` accepts either name.

### Make

```text
sighmake app.buildscript -g makefile
make -C build
make -C build Release
make -C build App
make -C build clean
```

Each project gets one Makefile per configuration, named `<Project>.<Config>`
(or `<Project>.<Config>.Android`). The master Makefile exposes matching
`<Project>.<Config>` targets; a bare `make <Project>` builds only the default
configuration (`Debug` when present, otherwise the first).

The Makefiles follow the usual GNU conventions, so they behave like any other
`make` project:

- **Toolchain.** The generated default (`g++` / `gcc` / `ar` / `strip`, or
  `clang++` / `clang` when generating on macOS, or the `cxx` / `cc` solution keys)
  applies only when nothing else is given. `CXX`, `CC`, `AR`, `STRIP` and
  `OBJCOPY` from the environment or the make command line win:

  ```text
  CXX=clang++ CC=clang make -C build
  make -C build CXX=clang++ CC=clang
  ```

  Android Makefiles keep the NDK toolchain they derive from `ANDROID_NDK_HOME`.
  `PYTHON ?= python3` can be overridden the same way.
- **User flags.** `CPPFLAGS`, `CXXFLAGS`, `CFLAGS`, `LDFLAGS` and `LDLIBS` belong to
  the person running `make`. They are appended to the project's flags (which
  live in `SIGHMAKE_CXXFLAGS`, `SIGHMAKE_CFLAGS`, `SIGHMAKE_LDFLAGS` and
  `SIGHMAKE_LDLIBS`), after them, so they can override `-O`/`-g`; `LDLIBS` ends
  the link line. Objective-C++ sources use `CXXFLAGS`. The compile line is
  `$(CXX) $(SIGHMAKE_CXXFLAGS) <per-file flags> $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c ...`:

  ```text
  make -C build CXXFLAGS="-O1 -g" LDFLAGS=-fuse-ld=lld
  CPPFLAGS=-DTRACE make -C build
  ```
- **Rebuilds.** Objects and binaries rebuild when the project's flags change (a
  stamp file written at generation time) and when the user's flags or the
  toolchain change: a second stamp per language and per link
  (`sighmake-<lang>.<config>.user`, in the intermediate directory) is rewritten
  by `make` only when the effective `$(CXX) $(CPPFLAGS) $(CXXFLAGS)` (or `$(CC)`
  ... `$(CFLAGS)`, or `$(CXX) $(LDFLAGS) $(LDLIBS)`) text differs from the stored
  one. Running `make` twice with the same flags rebuilds nothing; dropping a flag
  rebuilds. Other variables read only by build events are not tracked.
- **Targets.** `all` is the default goal; `clean`, `install`, `install-strip` and
  `uninstall` (below) are phony.

The master Makefile provides `install`, `install-strip` and `uninstall` for host
executables and shared libraries, with the usual variables: `DESTDIR` (staging
root), `PREFIX` (default `/usr/local`; the GNU spelling `prefix` also works),
`BINDIR` (default `$(PREFIX)/bin`, executables), `LIBDIR` (default
`$(PREFIX)/lib`, shared libraries; `bindir` / `libdir` also work) and `INSTALL`
(default `install`; `$(INSTALL) -d` creates directories, `-m 755` installs
programs and shared libraries). `install-strip` installs the same files and then
strips the installed copies with `$(STRIP)` (`--strip-unneeded` for shared
libraries, `-x` for macOS dylibs), which is the way to ship small binaries from a
configuration that keeps its symbols (`strip = false`); the build outputs stay
untouched. The targets install the `Release` configuration when one exists,
otherwise the default configuration. Static libraries and Android binaries are not
installed by these targets, and the targets exist only when a non-Android platform
is present.

```text
make -C build install DESTDIR=/tmp/stage PREFIX=/usr
make -C build install-strip prefix=/opt/app
make -C build install BINDIR=/usr/games LIBDIR=/usr/lib64
```

When both desktop and Android platforms exist, desktop configurations keep names
such as `Release` while Android configurations use `Release.Android`.
Linux and macOS configurations with the same name also share one desktop
Makefile name, so generate them in separate runs when their settings differ.

#### Position-independent code

`-fPIC` makes code slower: GCC and Clang reach global variables through the GOT
and, on ELF, must assume any exported function can be replaced at load time
(semantic interposition), so they inline less across functions. sighmake
therefore compiles position-independent code only where it is needed:

| Project | `-fPIC` |
| --- | --- |
| Shared library (`dll`/`shared`) | Always. |
| Static library linked by a shared library of the solution, directly or through other static or interface libraries | Yes. |
| Static library linked only into executables | No. |
| Executable | No. |

Only real link edges count: `target_link_libraries` and `project_references`,
not build-order-only `dependencies`, and an edge inside a platform block such as
`if(Android) { ... }` counts only for that platform. The project-level key
`position_independent` overrides the decision for static libraries and
executables:

```ini
[project:Plugin_Support]
type = lib
# auto (default) | true | false; true e.g. when linked into a shared library outside this solution
position_independent = true
```

`position_independent = false` on a static library that a shared library links
produces objects the linker rejects on most targets; shared libraries always
compile `-fPIC`, whatever the key says.

Position-independent code on ELF targets (Linux, Android) also gets
`-fno-semantic-interposition`, which lets the compiler inline and call functions
of the same library directly. A library whose functions are meant to be
replaced through `LD_PRELOAD` (or another library that defines the same symbol
first) sets `semantic_interposition = true` to keep the default model. GCC
supports the flag from version 5, so the generated Makefile sets
`SIGHMAKE_NO_SEMANTIC_INTERPOSITION ?= -fno-semantic-interposition`. Clang
before 10 rejects it (NDK r19 to r21 ship Clang 8 and 9), so when the compiler
is Clang (always on Android) the Makefile asks the compiler once, when make
reads it, and leaves the variable empty when the flag is not accepted. Setting
the variable on the make command line overrides both, e.g.
`make SIGHMAKE_NO_SEMANTIC_INTERPOSITION=` to turn the flag off when building
with an old Clang through `CXX=`. Apple targets do not use the flag; Mach-O has
no such interposition model.

ELF shared libraries (`dll`/`shared` on Linux and Android) also link with
`-Wl,-Bsymbolic-functions`: calls from the library to its own exported functions
bind inside the library at link time instead of going through the PLT, which
saves an indirect call and lets the dynamic loader resolve fewer symbols. Data
symbols keep the default binding, so copy relocations from executables still
work. The flag is the link-time half of the same trade-off as
`-fno-semantic-interposition` (a function of the library can no longer be
replaced from outside), so the same `semantic_interposition = true` turns both
off; there is no separate key. The generated Makefile asks the linker once, when
make reads it, and sets `SIGHMAKE_BSYMBOLIC_FUNCTIONS` to the flag only when the
link succeeds (GNU ld, gold, lld and mold accept it); `make
SIGHMAKE_BSYMBOLIC_FUNCTIONS=` turns it off for one build. Executables and
static libraries do not get the flag, and neither do Apple or Windows targets.

The CMake generator makes the same decision: `POSITION_INDEPENDENT_CODE ON` for
the targets above (a static library is PIC when any of its configurations needs
it), `POSITION_INDEPENDENT_CODE OFF` for `position_independent = false`, and
`-fno-semantic-interposition` behind a GCC / Clang 10+ check on non-Apple,
non-Windows hosts. Shared libraries get `-Wl,-Bsymbolic-functions` through
`target_link_options` on non-Apple, non-Windows hosts when
`check_linker_flag` accepts it.

#### Stripping and debug information

Two Make-only keys decide what happens to symbols; both are accepted at project
level (every configuration) and in a `[config:...]` section (that configuration
only, which wins). Visual Studio and CMake output ignore them.

- `debug_symbols = embedded | separate | none` chooses the mode directly.
  Aliases: `inline`, `full` (embedded); `split`, `external` (separate);
  `stripped`, `off` (none). Unknown values are ignored.
- `strip = true | false` is the older switch: `true` means `none`, `false` means
  `embedded`.

An explicit `debug_symbols` wins over `strip`. With neither key, `Release` is
`none` and every other configuration is `embedded`, exactly as before.

| Mode | Compiles | Executables and shared libraries | Where the symbols are |
| --- | --- | --- | --- |
| `embedded` (default for non-`Release`; `strip = false`) | With `-g`. | Unchanged, symbols stay in the binary. | In the binary. |
| `separate` | With `-g`. | Linux/Android: `$(OBJCOPY) --only-keep-debug $@ $@.debug`, `$(STRIP) --strip-unneeded $@`, `$(OBJCOPY) --add-gnu-debuglink=$@.debug $@` (`llvm-objcopy` / `llvm-strip` from the NDK on Android). macOS: `dsymutil $@ -o $@.dSYM`, then `strip` (`strip -x` for dylibs). | `<binary>.debug` next to the binary (macOS: the `<binary>.dSYM` bundle). |
| `none` (default for `Release`; `strip = true`) | Without `-g`, even when `debug_info` is set. | Stripped after linking (`$(STRIP)`; `-x` for macOS shared libraries; `--strip-unneeded` on Android). | Nowhere. |

How debuggers find a separate file: gdb and lldb read the `.gnu_debuglink` entry
(file name and CRC) added to the stripped ELF binary and look for `<name>.debug`
next to it, in a `.debug/` subdirectory, or under `/usr/lib/debug`; the file must
stay beside the binary (or be installed under `/usr/lib/debug`) when the
binary is moved. On macOS, lldb and Instruments locate the `.dSYM` bundle by the
binary's UUID, next to it or through Spotlight. Check with
`readelf --string-dump=.gnu_debuglink App` and `readelf -S App App.debug`
(`.debug_info` only in the latter). `make clean` removes the debug file too;
`install` does not install it, install it next to the binary yourself or use
`install-strip` plus your packaging's debug-package split.

Industry practice (CMake's `Release` is optimization plus `-DNDEBUG`, no `-g`, no
link-time stripping; stripping belongs to installation) is available with
`strip = false` or `debug_symbols = embedded` for a configuration, plus
`make install-strip`; `separate` gives the shipping binary and the debug file
from one build.

Debug information compiled into a configuration whose binaries are stripped never
reaches a binary and only costs compile time, memory and disk; in template-heavy
C++ it can be a third of the compile time and of the compiler's peak memory.
Static libraries are never stripped or split themselves, but their objects follow
the same `-g` rule, because they end up in the binaries.

To keep symbols in Release (for example to profile or debug an optimized build):

```ini
[config:Release|Linux]
strip = false          # or: debug_symbols = embedded
```

To ship a stripped Release binary with a separate debug file:

```ini
debug_symbols = separate
```

`strip = true` or `debug_symbols = none` at project level, or in another
configuration, strips that configuration and drops its `-g` too. A project that
wants `-g` on its static libraries while its binaries stay stripped can still pass
it through `cflags`. `debug_info` and `generate_debug_info` keep their meaning for
Visual Studio (`/Zi`, `/DEBUG` and a `.pdb`); the CMake generator does not use
`strip` or `debug_symbols`.

Changing either key changes the compile command line or the link stamp, so the
next build recompiles or relinks the configuration.

Build-event commands are written verbatim as recipe lines, so `$` keeps its
meaning to make: write `$$` for a literal shell `$` (for example `$$HOME`), and
`$(VAR)` refers to a make variable.

### CMake

```text
sighmake app.buildscript -g cmake
sighmake --build . --config Release -j 8
```

On the first direct build, sighmake configures the generated project with
`cmake -S <cache-dir> -B <cache-dir>/build`, then invokes `cmake --build`.

Configuration-specific settings are emitted as `$<CONFIG:...>` generator
expressions, and the generated project sets neither `CMAKE_BUILD_TYPE` nor
`CMAKE_CONFIGURATION_TYPES`. With a multi-config generator such as Visual
Studio or Ninja Multi-Config, `--config` selects the configuration. With a
single-config generator such as Unix Makefiles or plain Ninja, `--config` has no
effect and every per-configuration setting evaluates empty unless you configure
the build directory yourself with `-DCMAKE_BUILD_TYPE=<Config>` before running
`sighmake --build`.

The generated project enables only `C`, `CXX`, and, when MASM files exist,
`ASM_MASM`. Objective-C/Objective-C++ sources are listed but no `OBJC`/`OBJCXX`
language is enabled; NASM files are compiled through `add_custom_command` with
a default output format of `win64`.

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
solution matrix. The cache records the generator, configurations, platforms,
and backend paths. Only the Visual Studio cache records project entries, which
is why `--project` resolves project names only for MSBuild builds. The CMake
backend passes the value through as a raw target name; the Make backend builds the
configuration-specific target `<project>.<Config>` (`<project>.<Config>.Android`
for the Android build), so `--project App --config Release` runs `make App.Release`.

### Stale outputs

Every generation records the files it writes in a manifest,
`.sighmake-manifest`, next to its outputs:

| Generator | Manifest | Tracked files |
| --- | --- | --- |
| `vcxproj` | `build/.sighmake-manifest` (the `-B` directory) | `.vcxproj`, `.vcxproj.filters`, `.csproj`, `.sln` / `.slnx`, runtime dependency receipts |
| `makefile` (desktop and Android) | `build/.sighmake-manifest` | project Makefiles (`App.Release`, `App.Release.Android`), the master `Makefile`, `.csproj`, generated `AndroidManifest.xml` copies, runtime dependency receipts |
| `cmake` | `.sighmake-manifest` beside the root `CMakeLists.txt` | root and per-project `CMakeLists.txt` |

The manifest has one section per generator and target OS (for example
`[makefile linux]` and `[makefile android]`), so generating for several targets
or with several generators into one directory keeps each generation's files.

On the next generation with the same generator and target OS, a file listed in
the previous manifest that is not written again is deleted: the project was
removed or renamed, or it no longer applies to the target (a `platforms` filter,
a condition). This keeps `make -f build/<Project>.<Config>` and scripts that
glob `build/*.Release` from picking up outdated files. Each run prints a summary
line such as `Stale outputs: 2 removed, 0 listed`.

- Files in no manifest (hand-written files, other tools' files, `.vcxproj.user`)
  are never deleted, and a file listed by another section stays.
- A listed file is deleted only while it is still what sighmake wrote. The
  manifest records a content hash per file (a `[hash <generator> <os>]` section
  that older sighmake versions ignore). A stale file whose content changed since
  it was generated, for example a `CMakeLists.txt`, `Makefile` or `.vcxproj` you
  replaced with your own, is kept and reported as a warning; it is dropped from
  the manifest. Entries from a manifest written before hashes existed have no
  hash: such a file is deleted only if it carries the `# Generated by sighmake`
  header (Makefiles, `CMakeLists.txt`), matches the layout sighmake writes (project
  and solution files), or is a header-less helper file (`.txt`, `.xml`, `.py`,
  `.ps1` outputs such as runtime dependency receipts); otherwise it is kept.
- `--keep-stale` deletes nothing. It lists the stale files and keeps them in the
  manifest, so a later run without the option still removes them.
- The first generation in a directory without a manifest (for example the first
  run of this sighmake version in an existing `build/`) cannot know which files
  it wrote earlier. It lists the probable stale outputs as a warning and deletes
  nothing: files named like sighmake's outputs (`*_.vcxproj`, `*_.csproj`,
  `*_.slnx`, `*.runtime-dependencies.txt`, ...) or carrying the
  `# Generated by sighmake` header (Makefiles, project `CMakeLists.txt`) that
  this run did not write. Later runs only count them.
- `--prune-unlisted` deletes those probable stale outputs too. Project and
  solution files (`.vcxproj`, `.vcxproj.filters`, `.csproj`, `.slnx`) carry an XML
  comment `<!-- Generated by sighmake -->` before the root element, and a `.sln`
  a `# Generated by sighmake` line after its header lines; an unlisted one is
  counted (and pruned) when it has that marker. Files written by an older sighmake
  have no marker; for those the older layout check applies: LF line endings and
  sighmake's XML declaration, and for a `.sln` no `SolutionGuid` (Visual Studio
  adds one when it saves a solution). A hand-written or Visual Studio saved
  solution or project next to the generated files, for example with `-B .`, is
  left alone. A Visual Studio re-save drops the marker of a `.sln`/project it rewrites.

```text
sighmake app.buildscript --keep-stale       # list, delete nothing
sighmake app.buildscript --prune-unlisted   # also remove pre-manifest leftovers
```

Only generated project files are tracked. Build products and intermediate
directories (`bin/`, `obj/`, flag stamps) are not touched.

### Cross-generating for another OS

By default conditions describe the machine running sighmake, so a Windows host
cannot take the `if(posix)` or `[linux]` branches. `--target-os <os>` (or the
`SIGHMAKE_TARGET_OS` environment variable; the option wins) evaluates them for
another OS. Without either, behavior is unchanged.

| Target | `windows` | `linux` | `osx` | `unix`/`posix` |
| --- | --- | --- | --- | --- |
| `windows` | true | false | false | false |
| `linux` | false | true | false | true |
| `macos` | false | false | true | true |
| `ios`, `android` | false | false | false | true |

Android and iOS are POSIX targets that are neither desktop Linux nor macOS, so
put their settings in `if(Android)` blocks (platform filter) or `if(ios)`.
Package discovery (`find_package`) still inspects the host machine.

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

The generated files require NDK r19 or newer (clang toolchain, `llvm-ar`,
`llvm-strip`) and GNU make 4.0 or newer. The recipes use `mkdir -p` and `rm`, so a
POSIX shell must be reachable by make.

#### Finding the NDK

Each Android makefile looks for the NDK in this order and stops at the first hit:

1. `ANDROID_NDK_HOME`
2. `ANDROID_NDK_ROOT`
3. `ANDROID_NDK`
4. the highest-sorting `<ANDROID_HOME>/ndk/<version>`, then `ANDROID_SDK_ROOT`
5. the NDK sighmake itself found when it generated the makefile (it also checks
   Android Studio's default SDK location: `%LOCALAPPDATA%\Android\Sdk`,
   `~/Library/Android/sdk`, `~/Android/Sdk`, and picks the highest version).

If nothing is found the build stops with a message (`make clean` still works).
`find_package(Vulkan)` and `find_package(OpenGL)` use the same lookup at
generation time.

#### Shared libraries and executables

`type = shared` (alias of `dll`) produces `lib<target_name>.so`; the `lib` prefix is
added when missing. Android links with a response file, so the length of the object
and library list is no longer limited by the Windows command line:

```text
clang++ --target=aarch64-linux-android24 -shared -Wl,-soname,libgame.so \
    <LDFLAGS> -Wl,-z,max-page-size=16384 -o libgame.so @libgame.so.rsp
```

| Item | Behaviour |
| --- | --- |
| `-shared`, `-fPIC` | `-shared` for shared libraries. `-fPIC` for shared libraries and for the static libraries they link; see [Position-independent code](#position-independent-code). |
| `-Wl,-soname,<file name>` | Shared libraries only. The loader and the APK packager rely on it. |
| 16 KB page alignment | `-Wl,-z,max-page-size=16384` for `arm64-v8a` and `x86_64` (required for Android 15+ devices; NDK r27 does not enable it itself). `make ANDROID_MAX_PAGE_SIZE=4096` changes it, `ANDROID_MAX_PAGE_SIZE=` (empty) turns it off. `armeabi-v7a` and `x86` are never aligned. |
| Archives | Linked inside `--start-group ... --end-group`, so their order does not matter. `whole_archive` dependencies use `--whole-archive`. |
| C++ runtime | `clang++` from the NDK links `libc++_shared.so` dynamically. Bundle it with the app (the APK target below does) or link it statically with `ldflags = -static-libstdc++`. |
| Stripping | Release shared libraries and executables are stripped with `llvm-strip` and compiled without `-g`; `strip = false` keeps their symbols. See [Stripping and debug information](#stripping-and-debug-information). |

Executables use the same response-file link without `-shared`/`-soname`.

#### Using the NDK's make

The NDK ships GNU make for each host: `<ndk>/prebuilt/windows-x86_64/bin/make.exe`
(`linux-x86_64`, `darwin-x86_64`). A Windows machine without its own make can use
it directly:

```text
set ANDROID_HOME=C:\Users\me\AppData\Local\Android\Sdk
sighmake --target-os android -g makefile app.buildscript
"%ANDROID_HOME%\ndk\27.0.12077973\prebuilt\windows-x86_64\bin\make.exe" -C build Release.Android
```

`sighmake --build` does this automatically (next section). The recipes also need
`sh`, `mkdir` and `rm`; Git for Windows' `usr\bin` provides them and
`sighmake --build` adds it to the child's `PATH` when no `sh` is found.

#### Generating from a Windows host

Buildscripts that guard POSIX-only settings with `if(posix)` or `[linux]` tags
need the target OS set to `android` so those blocks are evaluated for the device
rather than for Windows:

```text
set ANDROID_NDK_HOME=C:\Android\ndk\27.0.12077973
sighmake --target-os android -g makefile app.buildscript
make -C build Release.Android ANDROID_ABI=arm64-v8a ANDROID_API=24
```

The same works with `set SIGHMAKE_TARGET_OS=android`. Static libraries pass
their object list to `llvm-ar` through a response file, so very large libraries
do not overflow the Windows command-line limit.

```text
make -C build Debug.Android
make -C build Release.Android ANDROID_ABI=x86_64 ANDROID_API=26
sighmake --build . --target Release.Android
```

#### Building Android from sighmake

```text
sighmake --target-os android -g makefile app.buildscript
sighmake --build . --config Release --platform android --abi arm64-v8a --api 26 -j 8
sighmake --build . --config Release --project Game        # Game.Release.Android
```

The cache of an Android-only solution builds Android without `--platform`. With
mixed desktop and Android platforms `--platform android` selects the Android
targets. `--abi` and `--api` set `ANDROID_ABI` and `ANDROID_API`. The make
program is `SIGHMAKE_MAKE` if set, else `make` from `PATH`, else the NDK's make.

Defaults and accepted overrides:

| Variable | Default | Accepted values |
| --- | --- | --- |
| `ANDROID_ABI` | `arm64-v8a` | `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86` |
| `ANDROID_API` | `24` | An NDK API level available in the installed toolchain |

Objects and outputs are scoped under
`build/<Config>/android/$(ANDROID_ABI)/` by default. Custom `outdir`/`intdir`
values may contain `$(ANDROID_ABI)` (make expands it), for example
`outdir = bin/android/$(ANDROID_ABI)/$(Configuration)`; a relative spelling is
anchored to the buildscript's directory. The first recipe line of every
archive rule creates the output directory, so a clean tree builds without
pre-created directories. Receipts use the first of `python3`, `python`, `py`
that runs (or `PYTHON=...`), because `python3` on Windows is often only a
Store stub. Android shared libraries receive a
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

#### Convert a `.sln` solution to a buildscript

1. Run the converter against your solution file (the `-c` flag is an alias
   for `--convert`):

```text
sighmake --convert Game.sln
sighmake -c Game.sln
sighmake --convert Game.slnx
```

2. Find the generated files next to the originals:

- One `<Project>.buildscript` is written next to each project file
  (for example `src/Core/Core.buildscript` for `src/Core/Core.vcxproj`).
- One root `<Solution>.buildscript` (for example `Game.buildscript`) is
  written next to the `.sln`/`.slnx` file. It holds the `[solution]` matrix
  plus one `include = ...` line per project.
- If a project shares the solution's name and directory, the two are merged
  into a single `<Solution>.buildscript` containing both sections.

Example layout after `sighmake --convert Game.sln`:

```text
Game.sln
Game.buildscript          # [solution] + include directives
src/Core/Core.vcxproj
src/Core/Core.buildscript
src/App/App.vcxproj
src/App/App.buildscript
```

The root file looks like this:

```ini
[solution]
name = Game
configurations = Debug, Release
platforms = Win32, x64

include = src/Core/Core.buildscript
include = src/App/App.buildscript
```

3. Review the generated files, then verify the round trip before deleting the
   originals:

```text
sighmake Game.buildscript -g vcxproj
sighmake --build . --config Release
```

A project listed in the solution but missing or unreadable on disk is
reported as a warning and skipped; conversion still succeeds with the
remaining projects.

Other conversion inputs use the same workflow:

```text
sighmake --convert Game.vcxproj
sighmake --convert Legacy.vcproj
```

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
| `cxx`, `cc` | C++ / C compiler drivers written into Makefiles (`cxx = clang++`, `cc = clang`); `--cxx` / `--cc` override them. |
| `defines` | Definitions applied to every project/configuration. Exact/platform selectors are accepted. |
| `include` | Settings fragment path. |

### Project identity and output

| Setting | Purpose |
| --- | --- |
| `type` | `exe`, `lib`, `dll`, `interface`, `sys`, or `sys_lib`. |
| `project_name`, `name` | Visual Studio display/project name override. |
| `uuid` | Explicit project GUID/UUID. Stable IDs are generated when omitted. |
| `root_namespace` | Visual Studio root namespace. |
| `toolset` | MSVC alias or platform toolset identifier. |
| `windows_sdk` | Windows target platform version. |
| `charset` | Common values are `MultiByte` and `Unicode`. |
| `target_name`, `target_ext` | Output basename and extension. |
| `outdir`, `intdir` | Output and intermediate directories. Makefile builds expand `$(Configuration)`, `$(Platform)` and `$(ProjectName)`; other `$(...)` references (for example `$(ANDROID_ABI)`) are left for make. |
| `platforms` | Project platform filter, see [Platform filter](#platform-filter). |
| `ignore_warn_duplicated_filename` | Suppress the duplicate-filename warning when true. |

### Common compiler settings

| Setting | Typical values or purpose |
| --- | --- |
| `includes` | Comma-separated include directories. |
| `defines` | Comma-separated preprocessor definitions. |
| `forced_includes` | Headers forcibly included by the compiler. |
| `std` | `14`, `17`, `20`, `23`, or `latest`. |
| `language` | `C`, `C++`, or `C#` (`CSharp` alias); normally auto-detected. |
| `c_standard` | `89`/`90`, `99`, `11`, `17`, or `23`. |
| `optimization` | `Disabled`, `MinSpace`, `MaxSpeed`, or `Full`. |
| `warning_level` | `Level0` through `Level4`. |
| `runtime_library` | `MultiThreaded`, `MultiThreadedDebug`, `MultiThreadedDLL`, or `MultiThreadedDebugDLL`. |
| `debug_info` | `EditAndContinue`, `ProgramDatabase`, or `OldStyle`. Makefiles compile `-g` for it unless the configuration strips its outputs (`strip`, `debug_symbols = none`). |
| `exceptions` | `true`/`Sync`, `Async`, or `false`. |
| `rtti` | Boolean runtime type information. |
| `support_just_my_code` | Optional Boolean for Visual Studio C/C++ Just My Code debugging. Unset preserves MSBuild defaults. |
| `multiprocessor` | Boolean parallel compilation setting. |
| `simd` | MSVC instruction set, e.g. `AdvancedVectorExtensions2`. Visual Studio only; see [SIMD and vectorization](#simd-and-vectorization). |
| `floating_point` | `Precise`, `Strict`, or `Fast`. |
| `utf8` | Use UTF-8 source/execution character sets (`/utf-8`; `-finput-charset=UTF-8 -fexec-charset=UTF-8` in Makefiles). Also accepted in `[solution]`, where it is the default for every project and configuration that does not set `utf8` itself; `utf8 = false` in a project or `[config:...]` opts out. |
| `cflags` | Raw additional compiler options. |
| `objcflags` | Additional flags for `.m`/`.mm` files. |
| `compile_as` | `Default`, `CompileAsC`, or `CompileAsCpp`. |
| `pch`, `pch_header`, `pch_output` | Precompiled-header policy. |
| `position_independent` (`pic`) | Project-level, Make and CMake only: `auto` (default), `true`, or `false`. See [Position-independent code](#position-independent-code). |
| `semantic_interposition` | Project-level, Make and CMake only. `true` keeps the default ELF interposition model: no `-fno-semantic-interposition` on position-independent code and no `-Wl,-Bsymbolic-functions` on shared libraries. Default `false`. |

For Visual Studio C/C++ projects, use `support_just_my_code` instead of adding
`/JMC-` to `cflags`. It emits `<SupportJustMyCode>false</SupportJustMyCode>`
(or `true`) inside the configuration's `ItemDefinitionGroup/ClCompile`.
Accepted values are `true`/`false`, `yes`/`no`, and `1`/`0` (case-sensitive);
other values are rejected. Omitting it emits no property and preserves prior
behavior. Makefile, CMake, and C# generation do not use this setting.

```ini
[project:App]
support_just_my_code = false
# Optional overrides, using the existing selector rules:
support_just_my_code[x64] = true
support_just_my_code[Debug|Win32] = false
```

Full `[config:Debug|Win32]` sections and configuration template inheritance also
support this property. A single-token bracket selector is a platform selector;
to target Debug on several platforms, specify each `Debug|Platform` pair.

Other recognized compiler keys include `disable_warnings`, `error_reporting`,
`assembler_listing`, `object_file_name`, `program_database_file`,
`browse_information`, `browse_information_file`, `basic_runtime_checks`,
`inline_function_expansion`, `favor_size_or_speed`,
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
| `subsystem` | Common values are `Console`, `Windows`, and `Native`. For C# `exe` projects, `Windows` selects `OutputType = WinExe` (windowed, no console); otherwise `Exe`. |
| `ignore_libs` | Specific default libraries to ignore. |
| `ignore_all_default_libraries` | Disable all default libraries. |
| `module_def` | Module definition/version-script path. |
| `generate_debug_info` | Emit linker debug information. |
| `strip` | Make only. Strip executables and shared libraries and compile without `-g`. Default `true` for `Release`, `false` otherwise. See [Stripping and debug information](#stripping-and-debug-information). |
| `debug_symbols` | Make only (Visual Studio and CMake ignore it). `embedded`, `separate` (symbols in `<binary>.debug` / `.dSYM`) or `none`; wins over `strip`. See [Stripping and debug information](#stripping-and-debug-information). |
| `link_incremental` | Incremental linking. |
| `whole_program_optimization` | Whole-program optimization/LTCG. |
| `optimize_references` | Remove unreferenced functions/data. |
| `enable_comdat_folding` | Fold identical COMDAT entries. |

Additional recognized linker keys include `show_progress`, `output_file`,
`suppress_startup_banner`, `link_pdb_file`, `base_address`,
`target_machine`, `link_error_reporting`, `safe_seh`, `entry_point`,
`link_version`, `generate_map_file`, `map_file_name`, `fixed_base_address`,
`randomized_base_address`, and `large_address_aware`.

Two keys deserve care. `version` is an alias of `link_version`, so a
descriptive `version = 1.0` line in a project silently becomes the linker
`/VERSION` switch. `program_database_file` at project level sets the compiler
PDB, and only inside a `[config:...]` section does it mean the linker PDB; use
`link_pdb_file` for the linker one at either level.

For `ProgramDatabase` / `EditAndContinue` debug information the generated
`ClCompile` `AdditionalOptions` include `/FS`, so concurrent `cl.exe` processes
(parallel MSBuild nodes, `/MP1`, shared static-library PDBs) serialize PDB writes
instead of failing with C1041. An explicit `/FS` in `cflags` is not duplicated.

`entry_point`, `link_version`, `large_address_aware`, `disable_warnings`, and
`lib_additional_dependencies` are project-level keys only; they are not
recognized with a selector or inside a `[config:...]` section.

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

### C# / .NET settings

| Setting | Purpose |
| --- | --- |
| `target_framework` | Required TFM, e.g. `net10.0` or `net10.0-windows`. |
| `csharp_version` | `LangVersion`, e.g. `14.0` or `latest`. |
| `nullable`, `implicit_usings` | `Nullable` / `ImplicitUsings` values such as `enable`. |
| `allow_unsafe` | `AllowUnsafeBlocks`. |
| `generate_runtime_configuration_files` | `GenerateRuntimeConfigurationFiles`. |
| `use_wpf`, `use_windows_forms` | `UseWPF` / `UseWindowsForms` for windowed apps. |
| `enable_windows_targeting` | `EnableWindowsTargeting` for `-windows` TFMs on other hosts. |
| `application_icon`, `application_manifest` | `ApplicationIcon` / `ApplicationManifest` paths. |
| `append_target_framework_to_output_path` | `AppendTargetFrameworkToOutputPath`. Default `true` (unchanged SDK behaviour); set `false` so `outdir` is used exactly, without an SDK-appended `net10.0/` subfolder. |
| `assembly_references` | Comma-separated paths to prebuilt managed assemblies. Each becomes a `<Reference Include="<stem>"><HintPath>...</HintPath><Private>true</Private></Reference>` item. |

See [C# projects](#c-projects) for the windowed-app recipe
(`net*-windows` + `subsystem = Windows` + `use_wpf` / `use_windows_forms`).

### Specialized tools

| Tool | Settings |
| --- | --- |
| Resource compiler | `resource_culture`, `resource_defines`, `resource_includes` |
| NASM | `nasm_path`, `nasm_format`, `nasm_flags`, `nasm_includes`, `nasm_defines` |
| Message compiler | `mc_header_dir`, `mc_rc_dir`, `mc_flags` |
| MIDL | `midl_output_dir`, `midl_header`, `midl_type_library`, `midl_dlldata`, `midl_iid`, `midl_proxy`, `midl_flags`, `midl_defines`, `midl_default_char_type`, `midl_target_environment` |
| Manifest | `generate_manifest`, `manifest_suppress_startup_banner`, `manifest_additional_files` |
| Browse/XML tools | `xdcmake_suppress_startup_banner`, `bscmake_suppress_startup_banner`, `bscmake_output_file` |

The manifest, browse, and XML tool keys, along with `ignore_import_library`
and `import_library`, are configuration-only: they are recognized with an exact
selector or inside a `[config:...]` section and silently dropped at project
level.

Additional configuration output keys are `executable_path`,
`ignore_import_library`, and `import_library`. `excluded_library[Config|Platform]`
adds a library file that is linked only in that cell.

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
the nested `build/` directory. The cache is written to the directory you ran
sighmake from, so `sighmake --build build` fails even though the project files are
in `build/`:

```text
sighmake app.buildscript
sighmake --build .
```

The error names the absolute directory it searched and, when the current
directory holds a cache, suggests `sighmake --build .`.

### A setting has no effect at all

Unknown keys are silently ignored; the parser warns about unknown sections,
conditions, and packages, but not about a misspelled setting name. Check the
spelling against the [Setting reference](#setting-reference), and check that
the key is accepted at the level you used it: some keys are project-only and
some are configuration-only.

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
generated Makefile or CMakeLists for the exact feature you need. `simd` is one
such key; [SIMD and vectorization](#simd-and-vectorization) shows the `cflags`
equivalent for GCC and Clang.

## C# projects

The Makefile generator (`-g makefile`) builds C# projects too, which is the way to
build them on Linux and macOS where no Visual Studio is installed: it writes each
project's `.csproj` next to the makefiles and gives every configuration a
`<Project>.<Config>` makefile that runs `dotnet build <csproj> -c <Config> -p:Platform=<platform>`
(the Windows-style platform of the project, `x64` for example, else the host platform). The master
Makefile orders the C# and native projects through their dependencies as usual.

The Visual Studio generator (`-g vcxproj`) supports SDK-style C# executables and
libraries alongside native projects. Install Visual Studio and a .NET SDK that
supports the requested target framework.

```ini
[solution]
name = ManagedExample
configurations = Debug, Release
platforms = x64

[project:ManagedHost]
language = C#
type = dll
target_framework = net10.0
sources = src/**/*.cs
nullable = enable
implicit_usings = enable
csharp_version = 14.0
allow_unsafe = true
generate_runtime_configuration_files = true
treat_warning_as_error = true
```

`language = CSharp` is an alias; `.cs` sources also enable automatic C# detection.
`target_framework` is required. Use `type = exe` for applications or `type = dll`
for managed libraries. `target_link_libraries(PRIVATE Contracts)` between C#
projects adds a managed assembly reference. `dependencies = Contracts` adds only
build ordering. Native projects can depend on managed projects for build ordering;
managed assemblies are not passed to the native linker. C# dependencies on native
projects must be declared as build-order-only references.

The optional C# settings shown above are project-wide. Existing `defines`,
`target_name`, `outdir`, `intdir`, per-file exclusions, and configuration sections
are supported. `outdir` and `intdir` paths are relative to the declaring buildscript;
the SDK appends the target framework to output paths, unless
`append_target_framework_to_output_path = false`. Default intermediate paths
are unique to each project. SDK source globbing is disabled, so only the listed
source files compile; because the generated project lists the files found by the
`sources` globs when sighmake ran, **re-run sighmake after adding or removing a
`.cs` file** (sighmake prints a reminder every time it writes a C# project).
Per-configuration settings (defines such as `FERRITE_AOT`, `outdir`, `intdir`)
are conditioned on `'$(Configuration)|$(Platform)'`; the generated project
therefore defaults `Platform` to the project's platform (`x64` when present)
before the SDK props are imported, so plain `dotnet build` applies them and
`-p:Platform=x64` is not needed. An explicit `-p:Platform=` still wins.
Source-directory `Directory.Build.props` files are not
automatically imported when the generated projects reside elsewhere; express the
needed settings in the buildscript.

### Copying runtime content

C# projects also support `runtime_dependencies = Name|Source|StagePath|Required`.
For example, `runtime_dependencies = Replay|data/replay.json|replay.json|true`
copies the declared file into both build and publish output using the SDK
`PreserveNewest` policy. Source paths are relative to the declaring buildscript;
`$(Configuration)` and other MSBuild property expressions remain evaluable.
Missing required files fail build and publish. Optional files are included only
when present at project evaluation. Stage paths must name safe relative files,
without traversal, absolute paths, MSBuild expressions, or reserved characters.
Destinations must be unique, including case differences, across the propagated
runtime dependency closure. C# content copying does not emit a native target receipt.

### Referencing a prebuilt assembly

Use `assembly_references` to link against a managed assembly that isn't a
sibling project in the solution, such as a prebuilt SDK DLL:

```ini
[project:ManagedHost]
language = C#
type = dll
target_framework = net10.0
sources = src/**/*.cs
outdir = bin/host
append_target_framework_to_output_path = false
assembly_references = ${FERRITE_SDK}/managed/Ferrite.Managed.Abstractions.dll
```

Paths are resolved relative to the declaring buildscript, like `outdir`, and
`$(...)` MSBuild expressions are left untouched. Each entry becomes a
`<Reference>` item with a `HintPath` pointing at the resolved assembly and
`Private = true` (the assembly is copied to the output directory); there is
currently no per-item syntax to disable that copy.

### Windowed apps (no console window)

A C# console app uses `OutputType = Exe`. A windowed app uses
`OutputType = WinExe`, which sighmake selects with `subsystem = Windows`:

```ini
[project:WpfApp]
language = C#
type = exe
target_framework = net10.0-windows
subsystem = Windows
use_wpf = true
nullable = enable
implicit_usings = enable
sources = src/**/*.cs
xaml = src/**/*.xaml
```

```ini
[project:FormsApp]
language = C#
type = exe
target_framework = net10.0-windows
subsystem = Windows
use_windows_forms = true
nullable = enable
implicit_usings = enable
sources = src/**/*.cs
```

Rules for windowed apps:

- Use a `-windows` target framework such as `net10.0-windows` with
  `use_wpf = true` (`UseWPF`), `use_windows_forms = true`
  (`UseWindowsForms`), or both.
- Keep `type = exe` with `subsystem = Windows` for a GUI app without a
  console window. Omit `subsystem` (or use `Console`) for a console app.
- List XAML with `xaml = ...` (`.xaml` files under `sources` are also
  detected). `App.xaml` becomes `ApplicationDefinition`; other pages become
  `Page` items with the `MSBuild:Compile` generator. A `Foo.xaml.cs`
  code-behind file automatically gets `DependentUpon = Foo.xaml`.
- List `.resx`/`.resw` files with `embedded_resources = ...` (also
  auto-detected under `sources`); image and font assets referenced by a
  windowed app are emitted as WPF `Resource` items.
- Optional branding and hosting settings:
  `application_icon = app.ico` (`ApplicationIcon`),
  `application_manifest = app.manifest` (`ApplicationManifest`), and
  `enable_windows_targeting = true` (`EnableWindowsTargeting`, for building a
  `-windows` target from a non-Windows host). Icon and manifest paths are
  resolved relative to the declaring buildscript, like `outdir`.

Generate and build the runnable example:

```powershell
cd examples/CSharp
sighmake managed.buildscript -g vcxproj
sighmake --build . --config Release --platform x64
```

`--build` restores SDK dependencies automatically for generated solutions containing
C# projects, and `--project` can select a managed project. The Makefile and CMake
generators currently reject C# projects. Importing existing `.csproj` files and
NuGet package declarations are not yet supported.

See the [.NET SDK property reference](https://learn.microsoft.com/en-us/dotnet/core/project-sdk/msbuild-props)
for the generated SDK properties.
