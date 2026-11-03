#pragma once

#include "common/string_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// Discovery helpers for the Android SDK and NDK. They are used at generation time (find_package
// under --target-os android) and by `sighmake --build` (picking the NDK's make). The generated
// Makefiles repeat the NDK lookup in make syntax so they also work when built on another machine.

namespace vcxproj {
namespace android_sdk {

namespace fs = std::filesystem;

inline std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Host tag used by the NDK for its prebuilt directories (toolchains/llvm/prebuilt/<tag>,
// prebuilt/<tag>). Apple hosts use the x86_64 directory even on arm64.
inline const char* ndk_host_tag() {
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__)
    return "darwin-x86_64";
#else
    return "linux-x86_64";
#endif
}

// Compare two dotted version strings numerically ("27.0.12077973" < "27.1.12297006" < "28.0.1").
// Non-numeric components compare as 0. Returns true when a < b.
inline bool version_less(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long long> out;
        std::string current;
        for (char c : v) {
            if (c == '.' || c == '-' || c == '_') {
                out.push_back(current.empty() ? 0 : std::atoll(current.c_str()));
                current.clear();
            } else if (c >= '0' && c <= '9') {
                current += c;
            } else {
                current.clear();
            }
        }
        out.push_back(current.empty() ? 0 : std::atoll(current.c_str()));
        return out;
    };
    return parts(a) < parts(b);
}

// The highest-versioned immediate subdirectory of `dir` (nullopt when there is none).
inline std::optional<fs::path> newest_subdirectory(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    std::optional<fs::path> best;
    std::string best_name;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (!best || version_less(best_name, name)) {
            best = entry.path();
            best_name = name;
        }
    }
    return best;
}

inline bool dir_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

// Android SDK root: ANDROID_HOME, ANDROID_SDK_ROOT, then the Android Studio default location.
inline std::optional<fs::path> find_sdk() {
    for (const char* name : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const std::string value = env_value(name);
        if (!value.empty() && dir_exists(value)) return fs::path(value);
    }
    std::vector<fs::path> defaults;
#if defined(_WIN32)
    const std::string local = env_value("LOCALAPPDATA");
    if (!local.empty()) defaults.push_back(fs::path(local) / "Android" / "Sdk");
#elif defined(__APPLE__)
    const std::string home = env_value("HOME");
    if (!home.empty()) defaults.push_back(fs::path(home) / "Library" / "Android" / "sdk");
#else
    const std::string home = env_value("HOME");
    if (!home.empty()) {
        defaults.push_back(fs::path(home) / "Android" / "Sdk");
        defaults.push_back(fs::path(home) / "Android" / "sdk");
    }
#endif
    for (const auto& candidate : defaults) {
        if (dir_exists(candidate)) return candidate;
    }
    return std::nullopt;
}

// NDK root: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_NDK, then the newest <sdk>/ndk/<version>
// (falling back to the legacy <sdk>/ndk-bundle).
inline std::optional<fs::path> find_ndk() {
    for (const char* name : {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"}) {
        const std::string value = env_value(name);
        if (!value.empty() && dir_exists(value)) return fs::path(value);
    }
    if (auto sdk = find_sdk()) {
        if (auto newest = newest_subdirectory(*sdk / "ndk")) return newest;
        if (dir_exists(*sdk / "ndk-bundle")) return *sdk / "ndk-bundle";
    }
    return std::nullopt;
}

// Directory holding the NDK's clang, llvm-ar, llvm-strip, ... for this host.
inline fs::path ndk_toolchain_bin(const fs::path& ndk) {
    return ndk / "toolchains" / "llvm" / "prebuilt" / ndk_host_tag() / "bin";
}

// GNU make shipped with the NDK (Windows hosts have no other make by default). Empty when absent.
inline std::optional<fs::path> find_ndk_make(const fs::path& ndk) {
    const fs::path candidate = ndk / "prebuilt" / ndk_host_tag() / "bin" /
#if defined(_WIN32)
        "make.exe";
#else
        "make";
#endif
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    return std::nullopt;
}

// Sysroot of the NDK's clang toolchain (headers and per-API libraries of the platform).
inline fs::path ndk_sysroot(const fs::path& ndk) {
    return ndk / "toolchains" / "llvm" / "prebuilt" / ndk_host_tag() / "sysroot";
}

// Newest <sdk>/build-tools/<version> (aapt2, d8, zipalign, apksigner).
inline std::optional<fs::path> find_build_tools(const fs::path& sdk) {
    return newest_subdirectory(sdk / "build-tools");
}

// Newest installed platform (<sdk>/platforms/android-<level>) that contains android.jar. `api` is the
// numeric API level of that platform ("36" for android-36.1), empty when none is installed.
struct PlatformJar {
    fs::path jar;
    std::string api;
};

inline std::optional<PlatformJar> find_platform_jar(const fs::path& sdk) {
    std::error_code ec;
    const fs::path platforms = sdk / "platforms";
    if (!fs::is_directory(platforms, ec)) return std::nullopt;
    std::optional<PlatformJar> best;
    std::string best_version;
    for (const auto& entry : fs::directory_iterator(platforms, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("android-", 0) != 0) continue;
        const std::string version = name.substr(8);
        if (version.empty() || version[0] < '0' || version[0] > '9') continue;
        const fs::path jar = entry.path() / "android.jar";
        std::error_code jar_ec;
        if (!fs::is_regular_file(jar, jar_ec)) continue;
        if (!best || version_less(best_version, version)) {
            std::string api;
            for (char c : version) {
                if (c < '0' || c > '9') break;
                api += c;
            }
            best = PlatformJar{jar, api};
            best_version = version;
        }
    }
    return best;
}

} // namespace android_sdk
} // namespace vcxproj
