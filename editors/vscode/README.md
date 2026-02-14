# sighmake Buildscript

Language support for sighmake `.buildscript` files in Visual Studio Code.

## Features

### Syntax Highlighting

Full TextMate grammar covering all buildscript constructs:

- Section headers: `[solution]`, `[project:Name]`, `[config:Debug|Win32]`, `[file:path]`
- Comments: `#` and `;` line comments
- Key-value pairs with config qualifiers: `key[Config|Platform] = value`
- Per-file settings: `src/pch.cpp:pch = Create`
- Control flow: `if(windows) { ... }`
- Built-in functions: `find_package()`, `target_link_libraries()`, `uses_pch()`, `folder()`
- Variable expansion: `${VARIABLE}`
- Strings, triple-quoted strings, escape sequences
- Known enum values, booleans, numbers, glob patterns
- Visibility keywords: `PUBLIC`, `PRIVATE`, `INTERFACE`
- Template inheritance: `[config:Profile|x64] : Template:Release`

### Autocomplete

Context-aware completions that adapt to your cursor position:

- **Section headers** — type `[` to get `solution`, `project:`, `config:`, `file:` completions with snippet templates
- **Keywords** — all ~100 keywords with aliases, filtered by current section context (solution, project, config, or file)
- **Values** — valid enum values after `=` (e.g., `type =` suggests `exe`, `lib`, `dll`; `optimization =` suggests `Disabled`, `MaxSpeed`, etc.)
- **Booleans** — `true`/`false` for boolean keywords
- **Platform conditions** — inside `if()`, suggests `windows`, `linux`, `osx`, `win32`, `x64`
- **Function arguments** — `PUBLIC`/`PRIVATE`/`INTERFACE` inside `target_link_libraries()`, package names inside `find_package()`

### Hover Documentation

Hover over any keyword, value, function, or section type to see documentation:

- **Keywords**: description, aliases, valid values, value type, and whether config qualifiers are supported
- **Enum values**: what the value means in context of its keyword
- **Functions**: signature and usage description
- **Section types**: what the section is used for
- **Platform conditions**: which platforms match
- **Visibility modifiers**: `PUBLIC`/`PRIVATE`/`INTERFACE` semantics

### Snippets

| Prefix | Description |
|--------|-------------|
| `solution` | Solution section with name, configurations, and platforms |
| `project` | Executable project section |
| `project-lib` | Static library project section |
| `project-dll` | DLL/shared library project section |
| `config` | Configuration section with optimization, runtime library, debug info |
| `config-template` | Configuration section with template inheritance |
| `pch` | Precompiled header setup (Use + Create) |
| `outdir` | Output and intermediate directory settings |
| `if-windows` | Conditional block for Windows |
| `if-linux` | Conditional block for Linux |
| `target_link_libraries` | Dependency declaration with visibility |
| `find_package` | External package discovery |
| `file` | Per-file settings section |
| `buildscript-full` | Complete buildscript template with solution and project |

### Other

- Comment toggling with `Ctrl+/` (uses `#`)
- Bracket matching for `[]`, `()`, `{}`
- Auto-closing pairs including `"`, `${}`
- Section-based code folding

## Buildscript Syntax Overview

```ini
# Solution definition
[solution]
name = MySolution
configurations = Debug, Release
platforms = Win32, x64

# Project definition
[project:MyProject]
type = exe
sources = src/**/*.cpp
headers = include/**/*.h
includes = include
defines = _CRT_SECURE_NO_WARNINGS
std = 17

# Configuration-specific settings
optimization[Debug|Win32] = Disabled
optimization[Release|x64] = MaxSpeed
runtime_library[Debug] = MultiThreadedDebug

# Per-file settings
src/pch.cpp:pch = Create
src/pch.cpp:pch_header = pch.h

# Platform conditionals
if(windows) {
    libs = user32.lib, gdi32.lib
}

# Dependencies with visibility
target_link_libraries(
    MathLib PUBLIC
    ExternalLib PRIVATE
)

# External packages
find_package(Vulkan, REQUIRED)

# Configuration templates
[config:Profile|x64] : Template:Release

# Per-file section
[file:src/special.cpp]
defines = SPECIAL_BUILD
```

## Installation

> **Note:** This is a **Visual Studio Code** extension, not a Visual Studio extension. Do not open the `.vsix` file with Visual Studio's VSIX Installer.

### From VSIX

After packaging (see [Packaging](#packaging) below), install with either:

```bash
code --install-extension sighmake-buildscript-0.1.0.vsix
```

Or in VS Code: **Extensions** sidebar → **`...`** menu → **Install from VSIX...** → select the `.vsix` file.

## Development

```bash
cd editors/vscode
npm install
npm run compile
```

Press **F5** in VS Code (with the `editors/vscode` folder open) to launch an Extension Development Host and test the extension.

Use `npm run watch` during development for automatic recompilation on file changes.

### Packaging

```bash
npx vsce package
```

This produces a `.vsix` file that can be installed in VS Code via **Extensions > Install from VSIX**.
