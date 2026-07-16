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

} // namespace defaults
} // namespace vcxproj
