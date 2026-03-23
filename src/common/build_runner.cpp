#include "pch.h"
#include "build_runner.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

std::string BuildRunner::find_msbuild(const std::string& vs_installation_path) {
    // Standard MSBuild location for VS 2017+
    fs::path msbuild = fs::path(vs_installation_path) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe";
    if (fs::exists(msbuild)) {
        return msbuild.string();
    }

    // Try amd64 variant
    fs::path msbuild_amd64 = fs::path(vs_installation_path) / "MSBuild" / "Current" / "Bin" / "amd64" / "MSBuild.exe";
    if (fs::exists(msbuild_amd64)) {
        return msbuild_amd64.string();
    }

    return "";
}

int BuildRunner::run_msbuild(const BuildCache& cache, const BuildOptions& options,
                              const std::string& cache_dir) {
    // Find MSBuild
    std::string msbuild_path = find_msbuild(cache.vs_installation_path);
    if (msbuild_path.empty()) {
        std::cerr << "Error: MSBuild.exe not found. Check your Visual Studio installation.\n";
        if (!cache.vs_installation_path.empty()) {
            std::cerr << "  VS installation path: " << cache.vs_installation_path << "\n";
        }
        return 1;
    }

    // Verify solution file exists
    fs::path sln_path = fs::path(cache_dir) / cache.solution_file;
    if (!fs::exists(sln_path)) {
        std::cerr << "Error: Solution file not found: " << sln_path << "\n";
        std::cerr << "  Run sighmake to regenerate project files.\n";
        return 1;
    }

    // Determine configuration
    std::string config = options.config;
    if (config.empty()) {
        // Default to Debug, or first available
        if (!cache.configurations.empty()) {
            bool has_debug = false;
            for (const auto& c : cache.configurations) {
                if (c == "Debug") { has_debug = true; break; }
            }
            config = has_debug ? "Debug" : cache.configurations[0];
        } else {
            config = "Debug";
        }
    } else {
        // Validate the config exists
        bool found = false;
        for (const auto& c : cache.configurations) {
            if (c == config) { found = true; break; }
        }
        if (!found && !cache.configurations.empty()) {
            std::cerr << "Error: Configuration '" << config << "' not available.\n";
            std::cerr << "  Available: ";
            for (size_t i = 0; i < cache.configurations.size(); i++) {
                if (i > 0) std::cerr << ", ";
                std::cerr << cache.configurations[i];
            }
            std::cerr << "\n";
            return 1;
        }
    }

    // Determine platform (default to x64 if available)
    std::string platform;
    if (!cache.platforms.empty()) {
        bool has_x64 = false;
        for (const auto& p : cache.platforms) {
            if (p == "x64") { has_x64 = true; break; }
        }
        platform = has_x64 ? "x64" : cache.platforms[0];
    } else {
        platform = "x64";
    }

    // Build MSBuild command
    std::string cmd = "\"\"" + msbuild_path + "\" \"" + sln_path.string() + "\"";
    cmd += " /p:Configuration=" + config;
    cmd += " /p:Platform=" + platform;

    // Handle target
    if (options.clean_only) {
        cmd += " /t:Clean";
    } else if (!options.target.empty()) {
        cmd += " /t:" + options.target;
    } else if (options.clean_first) {
        cmd += " /t:Rebuild";
    }

    // Parallel build
    if (options.parallel > 0) {
        cmd += " /m:" + std::to_string(options.parallel);
    } else {
        cmd += " /m";
    }

    cmd += "\"";

    std::cout << (options.clean_only ? "Cleaning: " : "Building: ") << cache.solution_name << " [" << config << "|" << platform << "]\n";
    std::cout << "MSBuild: " << msbuild_path << "\n" << std::endl;

    return std::system(cmd.c_str());
}

int BuildRunner::run_make(const BuildCache& cache, const BuildOptions& options,
                           const std::string& cache_dir) {
    // Verify build directory exists
    fs::path build_dir = fs::path(cache_dir) / cache.build_dir;
    fs::path makefile_path = build_dir / "Makefile";
    if (!fs::exists(makefile_path)) {
        std::cerr << "Error: Makefile not found at " << makefile_path << "\n";
        std::cerr << "  Run sighmake to regenerate project files.\n";
        return 1;
    }

    // Determine configuration
    std::string config = options.config;
    if (config.empty()) {
        if (!cache.configurations.empty()) {
            bool has_debug = false;
            for (const auto& c : cache.configurations) {
                if (c == "Debug") { has_debug = true; break; }
            }
            config = has_debug ? "Debug" : cache.configurations[0];
        } else {
            config = "Debug";
        }
    } else {
        // Validate
        bool found = false;
        for (const auto& c : cache.configurations) {
            if (c == config) { found = true; break; }
        }
        if (!found && !cache.configurations.empty()) {
            std::cerr << "Error: Configuration '" << config << "' not available.\n";
            std::cerr << "  Available: ";
            for (size_t i = 0; i < cache.configurations.size(); i++) {
                if (i > 0) std::cerr << ", ";
                std::cerr << cache.configurations[i];
            }
            std::cerr << "\n";
            return 1;
        }
    }

    // Build make command
    std::string cmd = "make -C \"" + build_dir.string() + "\"";

    // Clean only if requested
    if (options.clean_only) {
        std::string clean_cmd = cmd + " clean";
        std::cout << "Cleaning: " << cache.solution_name << " [" << config << "]" << std::endl;
        return std::system(clean_cmd.c_str());
    }

    // Clean first if requested
    if (options.clean_first) {
        std::string clean_cmd = cmd + " clean";
        std::cout << "Cleaning...\n";
        std::system(clean_cmd.c_str());
    }

    // Add target or config
    if (!options.target.empty()) {
        cmd += " " + options.target;
    } else {
        cmd += " " + config;
    }

    // Parallel build
    if (options.parallel > 0) {
        cmd += " -j " + std::to_string(options.parallel);
    }

    std::cout << "Building: " << cache.solution_name << " [" << config << "]" << std::endl;

    return std::system(cmd.c_str());
}

int BuildRunner::run(const BuildOptions& options) {
    // Verify directory exists
    if (!fs::exists(options.directory)) {
        std::cerr << "Error: Directory not found: " << options.directory << "\n";
        return 1;
    }

    if (!fs::is_directory(options.directory)) {
        std::cerr << "Error: Not a directory: " << options.directory << "\n";
        return 1;
    }

    // Read cache
    auto cache = BuildCache::read(options.directory);
    if (!cache) {
        std::cerr << "Error: No sighmake cache found in " << options.directory << "\n";
        std::cerr << "  Run sighmake to generate project files first.\n";
        return 1;
    }

    std::string cache_dir = fs::canonical(options.directory).string();

    if (cache->generator == "vcxproj") {
        return run_msbuild(*cache, options, cache_dir);
    } else if (cache->generator == "makefile") {
        return run_make(*cache, options, cache_dir);
    } else {
        std::cerr << "Error: Unknown generator '" << cache->generator << "' in cache file.\n";
        return 1;
    }
}

} // namespace vcxproj
