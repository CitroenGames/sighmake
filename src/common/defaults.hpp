#pragma once

#include <string>
#include <vector>

// Central defaults applied when an input file (buildscript, CMakeLists, VPC,
// .vcxproj/.sln) does not declare its own values. Kept in one place so every
// parser and the CLI agree on what an unspecified solution looks like.

namespace vcxproj {
namespace defaults {

// Solution configurations assumed when none are declared
inline const std::vector<std::string>& configurations() {
    static const std::vector<std::string> value = {"Debug", "Release"};
    return value;
}

// Solution platforms assumed when none are declared
inline const std::vector<std::string>& platforms() {
    static const std::vector<std::string> value = {"Win32", "x64"};
    return value;
}

// Configuration used by --build when none is given
inline constexpr const char* kBuildConfiguration = "Debug";

// Platform toolset used when neither the input file nor the installed Visual
// Studio detection yields one (v143 = VS 2022)
inline constexpr const char* kFallbackToolset = "v143";

// Character set assumed when a configuration does not declare one
inline constexpr const char* kCharacterSet = "MultiByte";

// Windows SDK version assumed when a configuration does not declare one
inline constexpr const char* kWindowsSdkVersion = "10.0";

// Compiler optimization applied when a configuration does not set one.
// Debug configurations build unoptimized; everything else builds for speed.
inline const char* optimization(bool debug) {
    return debug ? "Disabled" : "MaxSpeed";
}

// MSVC runtime library applied when a configuration does not set one
inline const char* runtime_library(bool debug) {
    return debug ? "MultiThreadedDebugDLL" : "MultiThreadedDLL";
}

// Debug information format applied when a configuration does not set one.
// EditAndContinue is only used for Debug|Win32; every other config gets
// ProgramDatabase.
inline const char* debug_information_format(bool debug, const std::string& platform) {
    return (debug && platform == "Win32") ? "EditAndContinue" : "ProgramDatabase";
}

} // namespace defaults
} // namespace vcxproj
