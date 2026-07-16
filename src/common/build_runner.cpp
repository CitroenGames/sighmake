#include "pch.h"
#include "build_runner.hpp"
#include "project_types.hpp"
#include "defaults.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

static bool contains_path_separator(const std::string& value) {
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos;
}

static std::optional<fs::path> existing_path(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return path;
    }
    return std::nullopt;
}

static std::optional<fs::path> resolve_msbuild_project(
    const BuildCache& cache,
    const std::string& cache_dir,
    const std::string& requested_project)
{
    if (requested_project.empty()) {
        return std::nullopt;
    }

    fs::path requested_path(requested_project);
    if (requested_path.has_extension() || contains_path_separator(requested_project)) {
        fs::path candidate = requested_path.is_absolute()
            ? requested_path
            : fs::path(cache_dir) / requested_path;
        if (auto found = existing_path(candidate)) {
            return found;
        }
    }

    const std::string requested_lower = to_lower(requested_project);
    for (const auto& project : cache.projects) {
        fs::path project_file(project.file);
        if (to_lower(project.name) == requested_lower ||
            to_lower(project_file.stem().string()) == requested_lower) {
            return fs::path(cache_dir) / project_file;
        }
    }

    // Backward-compatible fallback for caches generated before project files
    // were recorded. Debug sighmake builds historically append '_' before the
    // .vcxproj extension; release builds do not.
    const fs::path build_dir = cache.build_dir.empty()
        ? fs::path()
        : fs::path(cache.build_dir);
    for (const char* suffix : { "_.vcxproj", ".vcxproj" }) {
        fs::path candidate = fs::path(cache_dir) / build_dir / (requested_project + suffix);
        if (auto found = existing_path(candidate)) {
            return found;
        }
    }

    return std::nullopt;
}

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

    const bool building_project = !options.project.empty();
    fs::path build_path;
    if (building_project) {
        auto project_path = resolve_msbuild_project(cache, cache_dir, options.project);
        if (!project_path) {
            std::cerr << "Error: Project '" << options.project << "' not found in build cache.\n";
            if (!cache.projects.empty()) {
                std::cerr << "  Available projects: ";
                for (size_t i = 0; i < cache.projects.size(); ++i) {
                    if (i > 0) std::cerr << ", ";
                    std::cerr << cache.projects[i].name;
                }
                std::cerr << "\n";
            }
            return 1;
        }
        build_path = *project_path;
    } else {
        build_path = fs::path(cache_dir) / cache.solution_file;
        if (!fs::exists(build_path)) {
            std::cerr << "Error: Solution file not found: " << build_path << "\n";
            std::cerr << "  Run sighmake to regenerate project files.\n";
            return 1;
        }
    }

    // Determine configuration
    std::string config = options.config;
    if (config.empty()) {
        // Default to the standard configuration, or first available
        if (!cache.configurations.empty()) {
            bool has_default = false;
            for (const auto& c : cache.configurations) {
                if (c == defaults::kBuildConfiguration) { has_default = true; break; }
            }
            config = has_default ? defaults::kBuildConfiguration : cache.configurations[0];
        } else {
            config = defaults::kBuildConfiguration;
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
    std::string cmd = "\"\"" + msbuild_path + "\" \"" + build_path.string() + "\"";
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

    if (building_project && !options.build_project_references) {
        cmd += " /p:BuildProjectReferences=false";
    }

    // Parallel build
    if (options.parallel > 0) {
        cmd += " /m:" + std::to_string(options.parallel);
    } else {
        cmd += " /m";
    }

    cmd += "\"";

    std::cout << (options.clean_only ? "Cleaning: " : "Building: ") << cache.solution_name;
    if (building_project) {
        std::cout << " project " << options.project;
    }
    std::cout << " [" << config << "|" << platform << "]\n";
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

    const std::string requested_target = options.project.empty() ? options.target : options.project;
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
        int clean_result = std::system(clean_cmd.c_str());
        if (clean_result != 0) {
            std::cerr << "Warning: clean step returned non-zero exit code ("
                      << clean_result << "), continuing with build.\n";
        }
    }

    // Add target or config
    if (!requested_target.empty()) {
        cmd += " " + requested_target;
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

int BuildRunner::run_cmake(const BuildCache& cache, const BuildOptions& options,
                            const std::string& cache_dir) {
    // Verify build directory exists
    fs::path build_dir = fs::path(cache_dir) / cache.build_dir;
    if (!fs::exists(build_dir)) {
        // Create build directory and run cmake configure first
        try {
            fs::create_directories(build_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create build directory: " << e.what() << "\n";
            return 1;
        }

        // Configure
        std::string configure_cmd = "cmake -S \"" + cache_dir + "\" -B \"" + build_dir.string() + "\"";
        std::cout << "Configuring CMake project...\n";
        int ret = std::system(configure_cmd.c_str());
        if (ret != 0) {
            std::cerr << "Error: CMake configuration failed\n";
            return ret;
        }
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

    const std::string requested_target = options.project.empty() ? options.target : options.project;

    // Build cmake --build command
    std::string cmd = "cmake --build \"" + build_dir.string() + "\" --config " + config;

    if (options.clean_only) {
        cmd += " --target clean";
        std::cout << "Cleaning: " << cache.solution_name << " [" << config << "]" << std::endl;
        return std::system(cmd.c_str());
    }

    if (options.clean_first) {
        cmd += " --clean-first";
    }

    if (!requested_target.empty()) {
        cmd += " --target " + requested_target;
    }

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
    } else if (cache->generator == "cmake") {
        return run_cmake(*cache, options, cache_dir);
    } else {
        std::cerr << "Error: Unknown generator '" << cache->generator << "' in cache file.\n";
        return 1;
    }
}

} // namespace vcxproj
