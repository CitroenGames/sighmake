#pragma once

#include "build_cache.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vcxproj {

struct BuildOptions {
    std::string directory;          // The --build <dir> argument
    std::string config;             // --config <cfg> (optional)
    std::string platform;           // --platform <name> (optional, vcxproj only)
    std::string target;             // --target <tgt> (optional)
    std::string project;            // --project <name|file> (optional)
    bool clean_first = false;       // --clean-first (optional)
    bool clean_only = false;        // --clean (optional, clean without building)
    bool build_project_references = true; // false with --no-project-references
    int parallel = 0;               // --parallel <N> (optional, 0 = default)
    std::string abi;                // --abi <abi> (optional, Android makefile builds: ANDROID_ABI)
    bool aab = false;               // --aab (Android makefile builds: also build the .aab, ANDROID_AAB=1)
    std::string api;                // --api <level> (optional, Android makefile builds: ANDROID_API)
};

// Resolve an explicitly requested platform, including aliases such as
// x86 -> Win32, or choose the default platform when none was requested.
// Returns std::nullopt when the requested platform is not available.
std::optional<std::string> resolve_build_platform(
    const std::vector<std::string>& available_platforms,
    const std::string& requested_platform);

// Split an --abi value ("arm64-v8a,x86_64" or space separated) into ABI names.
std::vector<std::string> split_abi_list(const std::string& abi_list);

// True when the requested --platform selects the Android build of a makefile cache.
bool make_build_targets_android(const BuildCache& cache, const BuildOptions& options);

// Resolve the target understood by the generated master Makefile. Project
// aliases select the default configuration, so an explicit project build must
// name its configuration-specific target (for example, App.Release).
std::string resolve_make_build_target(
    const BuildCache& cache,
    const BuildOptions& options,
    const std::string& config);

// Message printed when --build finds no cache in `directory`. Names the directory that was searched and,
// when the cache is in `cwd` instead, hints at `--build .`.
std::string missing_cache_message(const std::string& directory, const std::filesystem::path& cwd);

class BuildRunner {
public:
    // Execute a build based on cache in the given directory
    // Returns the exit code from the build tool (0 = success)
    static int run(const BuildOptions& options);

private:
    // Invoke MSBuild on a .sln/.slnx file
    static int run_msbuild(const BuildCache& cache, const BuildOptions& options,
                           const std::string& cache_dir);

    // Invoke make on the master Makefile
    static int run_make(const BuildCache& cache, const BuildOptions& options,
                        const std::string& cache_dir);

    // Invoke cmake --build on a CMake build directory
    static int run_cmake(const BuildCache& cache, const BuildOptions& options,
                         const std::string& cache_dir);

    // Find MSBuild.exe given VS installation path
    static std::string find_msbuild(const std::string& vs_installation_path);
};

} // namespace vcxproj
