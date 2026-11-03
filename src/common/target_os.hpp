#pragma once

#include "common/string_utils.hpp"

#include <cstdlib>
#include <optional>
#include <string>

namespace vcxproj {

// Operating system that buildscript conditions (if(windows), [linux], ...) are evaluated against.
// Defaults to the machine running sighmake; set it to cross-generate for another target.
enum class TargetOS { Windows, Linux, MacOS, IOS, Android };

// Parse a target OS name (case-insensitive). Accepts windows/win, linux, macos/osx/darwin, ios, android.
inline std::optional<TargetOS> parse_target_os(const std::string& name) {
    const std::string value = to_lower(trim(name));
    if (value == "windows" || value == "win" || value == "win32") return TargetOS::Windows;
    if (value == "linux") return TargetOS::Linux;
    if (value == "macos" || value == "osx" || value == "darwin") return TargetOS::MacOS;
    if (value == "ios") return TargetOS::IOS;
    if (value == "android") return TargetOS::Android;
    return std::nullopt;
}

inline TargetOS host_os() {
#if defined(_WIN32)
    return TargetOS::Windows;
#elif defined(__APPLE__)
    return TargetOS::MacOS;
#else
    return TargetOS::Linux;
#endif
}

// Explicit override (from --target-os); empty means "use SIGHMAKE_TARGET_OS, then the host".
inline std::optional<TargetOS>& target_os_override() {
    static std::optional<TargetOS> value;
    return value;
}

inline void set_target_os(std::optional<TargetOS> os) { target_os_override() = os; }

// Effective target OS: --target-os, else SIGHMAKE_TARGET_OS, else the host OS.
inline TargetOS target_os() {
    if (target_os_override()) return *target_os_override();
    if (const char* env = std::getenv("SIGHMAKE_TARGET_OS")) {
        if (*env) {
            if (auto parsed = parse_target_os(env)) return *parsed;
        }
    }
    return host_os();
}

} // namespace vcxproj
