#pragma once

#include <filesystem>
#include <string>

#include "string_utils.hpp"

// Single source of truth for classifying files by extension. Parsers and
// generators should use these predicates instead of maintaining their own
// ".cpp" / ".cc" / ".cxx" chains — adding a new extension here updates every
// consumer at once.

namespace vcxproj {
namespace file_types {

// Lowercased extension (including the leading dot) of a path: "Foo.CPP" -> ".cpp"
inline std::string lowercase_extension(const std::string& path) {
    return to_lower(std::filesystem::path(path).extension().string());
}

// C++ translation units (compiled with the C++ compiler)
inline bool is_cpp_source(const std::string& ext) {
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx";
}

// C translation units (compiled with the C compiler)
inline bool is_c_source(const std::string& ext) {
    return ext == ".c";
}

// Any C or C++ translation unit
inline bool is_c_or_cpp_source(const std::string& ext) {
    return is_cpp_source(ext) || is_c_source(ext);
}

// Header files (not compiled directly)
inline bool is_header(const std::string& ext) {
    return ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh";
}

// Objective-C / Objective-C++ translation units
inline bool is_objcxx_source(const std::string& ext) {
    return ext == ".mm" || ext == ".m";
}

} // namespace file_types
} // namespace vcxproj
