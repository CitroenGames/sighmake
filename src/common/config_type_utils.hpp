#pragma once

#include <string>

// Single source of truth for project/configuration types. Maps between the
// buildscript "type" values (exe, lib, dll, ...), the MSVC ConfigurationType
// strings used internally (Application, StaticLibrary, ...), and the
// human-readable labels used in reports.

namespace vcxproj {
namespace config_type {

struct ConfigTypeInfo {
    const char* msvc_name;    // Internal/MSVC ConfigurationType ("Application")
    bool kernel_mode;         // True for kernel-mode types (sys / sys_lib)
    const char* buildscript;  // Canonical buildscript type value ("exe")
    const char* aliases[5];   // Accepted buildscript spellings (null-terminated)
    const char* label;        // Human-readable label, or nullptr to fall back
    const char* css_class;    // CSS class for the HTML report, or nullptr
};

// Non-kernel entries come first: name-only lookups (label, css_class, and the
// to_buildscript fallback) must find them before their kernel-mode variants.
inline constexpr ConfigTypeInfo kConfigTypes[] = {
    {"Application",    false, "exe",       {"exe", "application", "Application", nullptr},          "Executable",      "exe"},
    {"StaticLibrary",  false, "lib",       {"lib", "static", "staticlib", "StaticLibrary", nullptr}, "Static Library",  "staticlib"},
    {"DynamicLibrary", false, "dll",       {"dll", "shared", "dynamiclib", "DynamicLibrary", nullptr}, "Dynamic Library", "dll"},
    {"Utility",        false, "interface", {"interface", "header-only", "Utility", nullptr},         "Utility",         "utility"},
    {"Driver",         true,  "sys",       {"sys", "driver", "Driver", nullptr},                     nullptr,           nullptr},
    {"StaticLibrary",  true,  "sys_lib",   {"sys_lib", "kernel_lib", nullptr},                       nullptr,           nullptr},
};

// Parse a buildscript "type" value into the internal ConfigurationType string.
// Sets kernel_mode for the kernel-mode types. Unknown values pass through.
inline std::string from_buildscript(const std::string& value, bool& kernel_mode) {
    kernel_mode = false;
    for (const auto& info : kConfigTypes) {
        for (const char* const* alias = info.aliases; *alias; ++alias) {
            if (value == *alias) {
                kernel_mode = info.kernel_mode;
                return info.msvc_name;
            }
        }
    }
    return value;
}

// Inverse: internal ConfigurationType -> canonical buildscript type value.
inline std::string to_buildscript(const std::string& config_type, bool kernel_mode) {
    for (const auto& info : kConfigTypes) {
        if (config_type == info.msvc_name && kernel_mode == info.kernel_mode) {
            return info.buildscript;
        }
    }
    // No exact kernel-mode match — fall back to the first entry for the name.
    for (const auto& info : kConfigTypes) {
        if (config_type == info.msvc_name) {
            return info.buildscript;
        }
    }
    return config_type;
}

// Human-readable label for reports ("Executable", "Static Library", ...).
inline std::string label(const std::string& config_type) {
    for (const auto& info : kConfigTypes) {
        if (config_type == info.msvc_name) {
            if (info.label) return info.label;
            break;
        }
    }
    return config_type.empty() ? "Unknown" : config_type;
}

// CSS class for the HTML dependency report.
inline std::string css_class(const std::string& config_type) {
    for (const auto& info : kConfigTypes) {
        if (config_type == info.msvc_name) {
            if (info.css_class) return info.css_class;
            break;
        }
    }
    return "unknown";
}

} // namespace config_type
} // namespace vcxproj
