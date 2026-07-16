#pragma once

#include <string>
#include <iostream>

// Single source of truth for language-standard names and their per-backend
// spellings. Generators translate the canonical form ("14", "17", ..., or the
// MSVC-style "stdcpp17" a buildscript may carry) through these helpers instead
// of maintaining their own if/else chains.

namespace vcxproj {
namespace lang {

// Strip an MSVC-style "stdcpp" prefix: "stdcpp17" -> "17". Other values pass through.
inline std::string normalize_cpp_standard(const std::string& value) {
    if (value.rfind("stdcpp", 0) == 0) return value.substr(6);
    return value;
}

struct CppStandard {
    const char* canonical;  // "17"
    const char* msvc;       // "stdcpp17"   (LanguageStandard in .vcxproj)
    const char* gnu_flag;   // "-std=c++17" (GCC/Clang)
    const char* cmake;      // "17"         (CXX_STANDARD)
};

inline const CppStandard* find_cpp_standard(const std::string& value) {
    static const CppStandard kCppStandards[] = {
        {"14",     "stdcpp14",     "-std=c++14", "14"},
        {"17",     "stdcpp17",     "-std=c++17", "17"},
        {"20",     "stdcpp20",     "-std=c++20", "20"},
        {"23",     "stdcpp23",     "-std=c++23", "23"},
        {"latest", "stdcpplatest", "-std=c++23", "23"},
    };
    std::string canonical = normalize_cpp_standard(value);
    for (const auto& info : kCppStandards) {
        if (canonical == info.canonical) return &info;
    }
    return nullptr;
}

struct CStandard {
    const char* canonical;  // "17"
    const char* msvc;       // "stdc17", or nullptr where MSVC has no support
    const char* gnu_flag;   // "-std=c17"
    const char* cmake;      // "17" (C_STANDARD)
};

inline const CStandard* find_c_standard(const std::string& value) {
    static const CStandard kCStandards[] = {
        {"89", "stdc89", "-std=c89", "90"},
        {"90", "stdc89", "-std=c89", "90"},
        {"99", nullptr,  "-std=c99", "99"},
        {"11", "stdc11", "-std=c11", "11"},
        {"17", nullptr,  "-std=c17", "17"},
        {"23", nullptr,  "-std=c2x", "23"},  // GCC uses c2x for C23
    };
    for (const auto& info : kCStandards) {
        if (value == info.canonical) return &info;
    }
    return nullptr;
}

// MSVC LanguageStandard value. Warns and falls back for pre-C++14 or unknown values.
inline std::string cpp_standard_to_msvc(const std::string& value) {
    if (const auto* info = find_cpp_standard(value)) return info->msvc;

    std::string canonical = normalize_cpp_standard(value);
    if (canonical == "11" || canonical == "03" || canonical == "98") {
        std::cerr << "Warning: C++" << canonical << " is not supported by VS 2022+ (minimum is C++14). "
                  << "Falling back to stdcpp14.\n";
        return "stdcpp14";
    }
    if (!canonical.empty()) {
        std::cerr << "Warning: Unknown C++ standard '" << canonical << "'. Falling back to stdcpp17.\n";
    }
    return "stdcpp17";  // Safe default
}

// GCC/Clang -std flag. Unknown or empty values fall back to C++17.
inline std::string cpp_standard_to_gnu_flag(const std::string& value) {
    if (const auto* info = find_cpp_standard(value)) return info->gnu_flag;
    return "-std=c++17";
}

// CMake CXX_STANDARD value. Unknown or empty values fall back to 17.
inline std::string cpp_standard_to_cmake(const std::string& value) {
    if (const auto* info = find_cpp_standard(value)) return info->cmake;
    return "17";
}

// MSVC LanguageStandard_C value. MSVC only ships stdc89/stdc11; other standards
// warn and fall back. Unknown values return "" (compiler default).
inline std::string c_standard_to_msvc(const std::string& value) {
    if (const auto* info = find_c_standard(value)) {
        if (info->msvc) return info->msvc;
        if (value == "99") {
            std::cerr << "Warning: C99 (stdc99) is not fully supported by MSVC. "
                      << "Falling back to stdc11.\n";
        } else {
            std::cerr << "Warning: C" << value << " is not supported by MSVC. "
                      << "Falling back to stdc11.\n";
        }
        return "stdc11";
    }
    return "";  // Empty = compiler default
}

// GCC/Clang -std flag for C. Unknown or empty values fall back to C17.
inline std::string c_standard_to_gnu_flag(const std::string& value) {
    if (const auto* info = find_c_standard(value)) return info->gnu_flag;
    return "-std=c17";
}

// CMake C_STANDARD value. Unknown or empty values fall back to 17.
inline std::string c_standard_to_cmake(const std::string& value) {
    if (const auto* info = find_c_standard(value)) return info->cmake;
    return "17";
}

} // namespace lang
} // namespace vcxproj
