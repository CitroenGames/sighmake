#include "pch.h"
#include "build_runner.hpp"
#include "project_types.hpp"
#include "defaults.hpp"
#include "android_sdk.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

std::optional<std::string> resolve_build_platform(
    const std::vector<std::string>& available_platforms,
    const std::string& requested_platform)
{
    if (!requested_platform.empty()) {
        const std::string normalized_request = normalize_platform(requested_platform);
        if (available_platforms.empty()) {
            return normalized_request;
        }

        const std::string requested_lower = to_lower(normalized_request);
        for (const auto& available : available_platforms) {
            const std::string normalized_available = normalize_platform(available);
            if (to_lower(normalized_available) == requested_lower) {
                return normalized_available;
            }
        }
        return std::nullopt;
    }

    for (const auto& available : available_platforms) {
        const std::string normalized_available = normalize_platform(available);
        if (to_lower(normalized_available) == "x64") {
            return normalized_available;
        }
    }

    if (!available_platforms.empty()) {
        return normalize_platform(available_platforms.front());
    }
    return std::string("x64");
}

std::optional<int> parse_parallel_jobs(const std::string& value) {
    if (value.empty() || value.size() > 9 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    const int jobs = std::stoi(value);
    if (jobs <= 0) return std::nullopt;
    return jobs;
}

std::vector<std::string> split_abi_list(const std::string& abi_list) {
    std::vector<std::string> abis;
    std::string current;
    for (char c : abi_list + ",") {
        if (c == ',' || c == ' ' || c == ';') {
            if (!current.empty()) abis.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    return abis;
}

bool make_build_targets_android(const BuildCache& cache, const BuildOptions& options) {
    const bool has_android = std::any_of(
        cache.platforms.begin(), cache.platforms.end(), is_android_platform);
    const bool has_desktop = std::any_of(
        cache.platforms.begin(), cache.platforms.end(), [](const std::string& platform) {
            return !is_windows_platform(platform) && !is_android_platform(platform);
        });
    if (!options.platform.empty()) {
        // An explicit platform decides: Android builds use the .Android targets.
        return is_android_platform(options.platform);
    }
    return has_android && !has_desktop;
}

std::string resolve_make_build_target(
    const BuildCache& cache,
    const BuildOptions& options,
    const std::string& config)
{
    const bool android = make_build_targets_android(cache, options);
    if (!options.project.empty()) {
        return options.project + "." + config + (android ? ".Android" : "");
    }
    if (!options.target.empty()) {
        return options.target;
    }
    return config + (android ? ".Android" : "");
}

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

std::optional<fs::path> resolve_msbuild_project(
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
            to_lower(project_file.stem().string()) == requested_lower ||
            to_lower(project_file.filename().string()) == requested_lower) {
            return fs::path(cache_dir) / project_file;
        }
    }

    // Backward-compatible fallback for caches generated before project files
    // were recorded. Debug sighmake builds historically append '_' before the
    // .vcxproj extension; release builds do not.
    const fs::path build_dir = cache.build_dir.empty()
        ? fs::path()
        : fs::path(cache.build_dir);
    for (const char* suffix : { "_.vcxproj", ".vcxproj", "_.csproj", ".csproj" }) {
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
    auto selected_platform = resolve_build_platform(cache.platforms, options.platform);
    if (!selected_platform) {
        std::cerr << "Error: Platform '" << options.platform << "' not available.\n";
        std::cerr << "  Available: ";
        for (size_t i = 0; i < cache.platforms.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << normalize_platform(cache.platforms[i]);
        }
        std::cerr << "\n";
        return 1;
    }

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

    const std::string& platform = *selected_platform;

    // Build MSBuild command
    std::string cmd = "\"\"" + msbuild_path + "\" \"" + build_path.string() + "\"";
    cmd += " /p:Configuration=\"" + config + "\"";
    cmd += " /p:Platform=\"" + platform + "\"";

    if (!options.clean_only && std::any_of(cache.projects.begin(), cache.projects.end(),
        [](const BuildProjectEntry& project) { return fs::path(project.file).extension() == ".csproj"; })) {
        cmd += " /restore";
    }

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

// Look for an executable on PATH (Windows also tries .exe/.cmd/.bat). Empty when absent.
static std::string find_on_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
#ifdef _WIN32
    const char separator = ';';
    const std::vector<std::string> suffixes = {".exe", ".cmd", ".bat", ""};
#else
    const char separator = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    std::stringstream entries(path_env);
    std::string dir;
    while (std::getline(entries, dir, separator)) {
        if (dir.empty()) continue;
        for (const auto& suffix : suffixes) {
            std::error_code ec;
            fs::path candidate = fs::path(dir) / (name + suffix);
            if (fs::is_regular_file(candidate, ec)) return candidate.string();
        }
    }
    return "";
}

// Choose the make program. SIGHMAKE_MAKE wins. Otherwise `make` from PATH; Android builds on a host
// without one (typically Windows) fall back to the GNU make that ships with the NDK.
std::string resolve_make_program(bool android, std::string* note) {
    if (const char* override_make = std::getenv("SIGHMAKE_MAKE")) {
        if (*override_make) return override_make;
    }
    if (!find_on_path("make").empty()) return "make";
    if (android) {
        if (auto ndk = android_sdk::find_ndk()) {
            if (auto ndk_make = android_sdk::find_ndk_make(*ndk)) {
                if (note) *note = "make not found on PATH; using the NDK's make: " + ndk_make->string();
                return ndk_make->string();
            }
        }
    }
    return "make";
}

// The generated Makefiles use POSIX shell tools (mkdir -p, rm). On Windows, make needs `sh` on PATH
// for that; add Git for Windows' usr/bin when nothing provides one. Affects this process only.
static void ensure_posix_shell_on_path() {
#ifdef _WIN32
    if (!find_on_path("sh").empty()) return;
    std::vector<fs::path> candidates;
    for (const char* var : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
        if (const char* root = std::getenv(var)) {
            candidates.push_back(fs::path(root) / "Git" / "usr" / "bin");
        }
    }
    for (const auto& dir : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(dir / "sh.exe", ec)) {
            const char* current = std::getenv("PATH");
            const std::string updated = dir.string() + ";" + (current ? current : "");
            _putenv_s("PATH", updated.c_str());
            return;
        }
    }
#endif
}

int BuildRunner::run_make(const BuildCache& cache, const BuildOptions& options,
                           const std::string& cache_dir) {
    if (!options.platform.empty()) {
        // Makefile caches have one build per OS family; --platform only picks between the
        // desktop and Android builds that the cache contains.
        bool known = cache.platforms.empty();
        for (const auto& available : cache.platforms) {
            if (to_lower(available) == to_lower(options.platform)) known = true;
        }
        if (!known && !is_android_platform(options.platform)) {
            std::cerr << "Error: Platform '" << options.platform << "' is not part of this Makefile build.\n";
            return 1;
        }
        if (is_android_platform(options.platform) &&
            std::none_of(cache.platforms.begin(), cache.platforms.end(), is_android_platform) &&
            !cache.platforms.empty()) {
            std::cerr << "Error: This Makefile build has no Android platform.\n"
                      << "  Add Android to the solution platforms and regenerate with --target-os android.\n";
            return 1;
        }
    }

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

    const std::string requested_target = resolve_make_build_target(cache, options, config);
    const bool android_build = make_build_targets_android(cache, options);

    // Build make command
    std::string make_note;
    const std::string make_program = resolve_make_program(android_build, &make_note);
    if (!make_note.empty()) {
        std::cout << make_note << "\n";
    }
    ensure_posix_shell_on_path();
    std::string cmd = (make_program == "make" ? make_program : "\"" + make_program + "\"") +
                      " -C \"" + build_dir.string() + "\"";
    // Android: --abi may name several ABIs (multi-ABI APK). Each is built in its own make run with
    // packaging skipped; a final run packages every listed ABI. Passed as make variables; the
    // generated Makefiles default them with ?=.
    std::vector<std::string> abis;
    if (android_build) {
        abis = split_abi_list(options.abi);
        if (!options.api.empty()) cmd += " ANDROID_API=" + options.api;
    }
#ifdef _WIN32
    // cmd.exe /c strips the outer quotes of a command that starts with a quote; keep the program
    // path quoted by wrapping the whole command line once more.
    auto run_command = [](const std::string& command) {
        const std::string wrapped = command.rfind('"', 0) == 0 ? "\"" + command + "\"" : command;
        return std::system(wrapped.c_str());
    };
#else
    auto run_command = [](const std::string& command) { return std::system(command.c_str()); };
#endif

    // ANDROID_ABI variants of a command: one entry per ABI (or the plain command).
    auto abi_commands = [&](const std::string& base) {
        std::vector<std::string> commands;
        if (abis.empty()) {
            commands.push_back(base);
        } else {
            for (const auto& abi : abis) commands.push_back(base + " ANDROID_ABI=" + abi);
        }
        return commands;
    };

    // Clean only if requested
    if (options.clean_only) {
        std::cout << "Cleaning: " << cache.solution_name << " [" << config << "]" << std::endl;
        int result = 0;
        for (const auto& clean_cmd : abi_commands(cmd + " clean")) {
            const int code = run_command(clean_cmd);
            if (code != 0) result = code;
        }
        return result;
    }

    // Clean first if requested
    if (options.clean_first) {
        std::cout << "Cleaning...\n";
        for (const auto& clean_cmd : abi_commands(cmd + " clean")) {
            int clean_result = run_command(clean_cmd);
            if (clean_result != 0) {
                std::cerr << "Warning: clean step returned non-zero exit code ("
                          << clean_result << "), continuing with build.\n";
            }
        }
    }

    cmd += " " + requested_target;

    // Parallel build
    if (options.parallel > 0) {
        cmd += " -j " + std::to_string(options.parallel) + " SIGHMAKE_JOBS=" + std::to_string(options.parallel);
    }

    if (options.aab) {
        cmd += " ANDROID_AAB=1";
    }

    std::cout << "Building: " << cache.solution_name << " [" << config << "]" << std::endl;

    if (abis.size() > 1) {
        std::string abi_list;
        for (const auto& abi : abis) abi_list += (abi_list.empty() ? "" : " ") + abi;
        for (size_t i = 0; i < abis.size(); ++i) {
            const bool last = i + 1 == abis.size();
            std::string abi_cmd = cmd + " ANDROID_ABI=" + abis[i];
            if (!last) {
                std::cout << "Android ABI " << abis[i] << "\n";
                abi_cmd += " SIGHMAKE_SKIP_PACKAGING=1";
            } else {
                std::cout << "Android ABI " << abis[i] << " (packaging " << abi_list << ")\n";
                abi_cmd += " ANDROID_APK_ABIS=\"" + abi_list + "\"";
            }
            const int code = run_command(abi_cmd);
            if (code != 0) return code;
        }
        return 0;
    }
    if (abis.size() == 1) cmd += " ANDROID_ABI=" + abis[0];

    return run_command(cmd);
}

int BuildRunner::run_cmake(const BuildCache& cache, const BuildOptions& options,
                            const std::string& cache_dir) {
    if (!options.platform.empty()) {
        std::cerr << "Error: --platform is not supported by the CMake build backend.\n";
        std::cerr << "  Select the target platform when configuring CMake instead.\n";
        return 1;
    }

    // Verify build directory exists
    fs::path build_dir = fs::path(cache_dir) / cache.build_dir;
    // The directory may exist without being a CMake build tree (makefile or vcxproj output of an
    // earlier generation into the same root): configure whenever CMakeCache.txt is missing.
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
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
    std::string cmd = "cmake --build \"" + build_dir.string() + "\" --config \"" + config + "\"";

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

std::string missing_cache_message(const std::string& directory, const std::filesystem::path& cwd) {
    std::error_code ec;
    const fs::path searched = fs::absolute(directory, ec);
    std::string message = "Error: No sighmake cache found in " + (ec ? directory : searched.lexically_normal().string()) + "\n";
    message += "  Run sighmake to generate project files first.\n";
    message += "  The cache is written to the directory sighmake was run from (the output root), not to the\n"
               "  'build' subdirectory that holds the generated project files.\n";
    std::error_code cwd_ec;
    if (fs::exists(cwd / BuildCache::CACHE_FILENAME, cwd_ec) &&
        fs::weakly_canonical(searched, ec) != fs::weakly_canonical(cwd, cwd_ec)) {
        message += "  Found a cache in the current directory: try 'sighmake --build .'\n";
    }
    return message;
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
        std::cerr << missing_cache_message(options.directory, std::filesystem::current_path());
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
