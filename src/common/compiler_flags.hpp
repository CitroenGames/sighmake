#pragma once

#include <string>

// Single source of truth for translating the MSVC-style setting values stored
// in Configuration ("MaxSpeed", "Level4", "MultiThreadedDLL", "Console", ...)
// into concrete compiler/linker flags per backend. Generators use these tables
// instead of maintaining their own if/else chains.

namespace vcxproj {
namespace flags {

// ---- Optimization (ClCompile <Optimization>) ----

struct Optimization {
    const char* name;  // "MaxSpeed"
    const char* msvc;  // "/O2"
    const char* gnu;   // "-O3"
};

inline const Optimization* find_optimization(const std::string& name) {
    static const Optimization kTable[] = {
        {"Disabled", "/Od", "-O0"},
        {"MinSpace", "/O1", "-Os"},
        {"MaxSpeed", "/O2", "-O3"},
        {"Full",     "/Ox", "-O3"},
    };
    for (const auto& entry : kTable) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

// GCC/Clang flag; unknown values fall back to -O2.
inline std::string optimization_to_gnu_flag(const std::string& name) {
    if (const auto* entry = find_optimization(name)) return entry->gnu;
    return "-O2";
}

// ---- Warning level (ClCompile <WarningLevel>) ----

struct WarningLevel {
    const char* name;     // "Level4"
    const char* msvc;     // "/W4"
    const char* msbuild;  // MSBuild WarningLevel element value
    const char* gnu;      // "-Wall -Wextra" (space-separated when multiple)
};

inline const WarningLevel* find_warning_level(const std::string& name) {
    static const WarningLevel kTable[] = {
        {"Level0", "/W0", "TurnOffAllWarnings", "-w"},
        {"Level1", "/W1", "Level1",             "-Wall"},
        {"Level2", "/W2", "Level2",             "-Wall"},
        {"Level3", "/W3", "Level3",             "-Wall"},
        {"Level4", "/W4", "Level4",             "-Wall -Wextra"},
    };
    for (const auto& entry : kTable) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

// GCC/Clang flags; unknown values fall back to -Wall.
inline std::string warning_level_to_gnu_flags(const std::string& name) {
    if (const auto* entry = find_warning_level(name)) return entry->gnu;
    return "-Wall";
}

// MSBuild WarningLevel value; unknown values pass through unchanged.
inline std::string warning_level_to_msbuild(const std::string& name) {
    if (const auto* entry = find_warning_level(name)) return entry->msbuild;
    return name;
}

// ---- Runtime library (ClCompile <RuntimeLibrary>) ----

// MSVC flag, or nullptr for unknown values.
inline const char* runtime_library_to_msvc_flag(const std::string& name) {
    if (name == "MultiThreaded") return "/MT";
    if (name == "MultiThreadedDebug") return "/MTd";
    if (name == "MultiThreadedDLL") return "/MD";
    if (name == "MultiThreadedDebugDLL") return "/MDd";
    return nullptr;
}

// ---- Debug information format (ClCompile <DebugInformationFormat>) ----

// MSVC flag, or nullptr for unknown values.
inline const char* debug_format_to_msvc_flag(const std::string& name) {
    if (name == "ProgramDatabase") return "/Zi";
    if (name == "EditAndContinue") return "/ZI";
    if (name == "OldStyle") return "/Z7";
    return nullptr;
}

// ---- Subsystem (Link <SubSystem>) ----

// MSVC linker flag, or nullptr for unknown values.
inline const char* subsystem_to_msvc_flag(const std::string& name) {
    if (name == "Console") return "/SUBSYSTEM:CONSOLE";
    if (name == "Windows") return "/SUBSYSTEM:WINDOWS";
    if (name == "Native") return "/SUBSYSTEM:NATIVE";
    return nullptr;
}

} // namespace flags
} // namespace vcxproj
