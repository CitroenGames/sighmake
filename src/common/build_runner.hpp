#pragma once

#include "build_cache.hpp"
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
};

// Resolve an explicitly requested platform, including aliases such as
// x86 -> Win32, or choose the default platform when none was requested.
// Returns std::nullopt when the requested platform is not available.
std::optional<std::string> resolve_build_platform(
    const std::vector<std::string>& available_platforms,
    const std::string& requested_platform);

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
