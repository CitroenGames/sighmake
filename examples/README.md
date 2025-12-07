# sighmake Examples

This directory contains example projects demonstrating various features of sighmake.

## multi_project.buildscript

A complete example demonstrating:
- **Multiple projects** in a single solution
- **DLL project** (MathLib) - A simple math library
- **EXE project** (Calculator) - An application that uses the DLL
- **Project dependencies** - Calculator depends on MathLib
- **DLL export/import** - Proper use of `__declspec(dllexport)` and `__declspec(dllimport)`
- **Configuration-specific settings** - Different output directories for Debug/Release and Win32/x64
- **Post-build events** - Copying DLL to output directory

### Project Structure

```
examples/
├── multi_project.buildscript   # Build configuration
├── mathlib/                     # DLL project
│   ├── mathlib.h               # Header with DLL exports
│   └── mathlib.cpp             # Implementation
└── calculator/                  # EXE project
    └── main.cpp                # Application that uses MathLib
```

### Building the Example

1. Generate the Visual Studio project files:
   ```
   sighmake examples/multi_project.buildscript -b examples/project_files
   ```

2. Open the solution:
   ```
   examples/project_files/MultiProjectExample.sln
   ```

3. Build in Visual Studio (F7) or from command line:
   ```
   msbuild examples/project_files/MultiProjectExample.sln /p:Configuration=Release /p:Platform=x64
   ```

4. Run the calculator:
   ```
   examples/output/x64/Release/Calculator.exe
   ```

### Features Demonstrated

#### MathLib (DLL)
- Exports math functions using `MATHLIB_EXPORTS` define
- Demonstrates proper DLL export macros
- Provides basic and advanced math operations
- Version information export

#### Calculator (EXE)
- Imports and uses MathLib functions
- Links to MathLib.lib (import library)
- Demonstrates both automatic and interactive usage
- Shows proper dependency setup in buildscript

#### Buildscript Features
- Multi-project solution
- Project dependencies (`depends = MathLib`)
- Library linking (`libs = MathLib.lib`)
- Configuration-specific output directories
- Post-build events for DLL copying
- Different settings for different configurations
