#pragma once

#include "build_cache.hpp"
#include <string>

namespace vcxproj {

struct BuildOptions {
    std::string directory;          // The --build <dir> argument
    std::string config;             // --config <cfg> (optional)
    std::string target;             // --target <tgt> (optional)
    bool clean_first = false;       // --clean-first (optional)
    int parallel = 0;               // --parallel <N> (optional, 0 = default)
};

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

    // Find MSBuild.exe given VS installation path
    static std::string find_msbuild(const std::string& vs_installation_path);
};

} // namespace vcxproj
