#include "pch.h"
#include "makefile_generator.hpp"
#include "common/build_cache.hpp"
#include "common/string_utils.hpp"
#include "common/file_types.hpp"
#include "common/compiler_flags.hpp"
#include "common/language_standards.hpp"

namespace vcxproj {

namespace {

std::map<std::string, const Project*> build_project_lookup(const Solution& solution) {
    std::map<std::string, const Project*> lookup;
    for (const auto& project : solution.projects) {
        if (!project.is_package_project) {
            lookup[project.name] = &project;
        }
    }
    return lookup;
}

const Project* find_project(const std::map<std::string, const Project*>& lookup, const std::string& name) {
    auto it = lookup.find(name);
    return it != lookup.end() ? it->second : nullptr;
}

template <typename T>
const T* find_config_setting(const std::map<std::string, T>& settings, const std::string& config_key) {
    auto it = settings.find(config_key);
    if (it != settings.end()) {
        return &it->second;
    }

    it = settings.find(ALL_CONFIGS);
    if (it != settings.end()) {
        return &it->second;
    }

    // Older generator code used this literal; keep it as a compatibility fallback.
    it = settings.find("ALL_CONFIGS");
    if (it != settings.end()) {
        return &it->second;
    }

    return nullptr;
}

// Host tag of the prebuilt LLVM toolchain shipped with the Android NDK.
// Determined by the machine running sighmake (Apple hosts use the x86_64
// directory even on arm64 - the NDK ships universal binaries there).
constexpr const char* ANDROID_HOST_TAG =
#if defined(_WIN32)
    "windows-x86_64";
#elif defined(__APPLE__)
    "darwin-x86_64";
#else
    "linux-x86_64";
#endif

std::string make_project_config_target(const Project& project, const std::string& config_name,
                                       bool android = false) {
    return project.name + "." + config_name + (android ? ".Android" : "");
}

// Find a config key the makefile generator can build for this config name.
// android=true selects Android configs; android=false selects the other
// non-Windows configs (Linux/macOS). They get separate makefiles so a solution
// can target both without the outputs colliding.
std::optional<std::string> find_makefile_config_key(const Project& project, const std::string& config_name,
                                                    bool android) {
    for (const auto& [config_key, config] : project.configurations) {
        (void)config;
        size_t pipe_pos = config_key.find('|');
        std::string cfg = pipe_pos != std::string::npos
            ? config_key.substr(0, pipe_pos)
            : config_key;
        std::string platform = pipe_pos != std::string::npos
            ? config_key.substr(pipe_pos + 1)
            : "";
        if (cfg == config_name && !is_windows_platform(platform) &&
            is_android_platform(platform) == android) {
            return config_key;
        }
    }
    return std::nullopt;
}

// Android loaders (System.loadLibrary, APK packaging) require the "lib" prefix
// on shared libraries.
std::string decorate_target_name(const std::string& name, const std::string& config_type, bool android) {
    if (android && config_type == "DynamicLibrary" && name.rfind("lib", 0) != 0) {
        return "lib" + name;
    }
    return name;
}

// Default output/intermediate directories. Android builds get an ABI-specific
// subdirectory ($(ANDROID_ABI) expands when make runs) so different ABIs don't
// overwrite each other's artifacts.
std::string default_makefile_out_dir(const std::string& config_name, bool android) {
    return android ? "build/" + config_name + "/android/$(ANDROID_ABI)"
                   : "build/" + config_name;
}

std::string default_makefile_int_dir(const std::string& config_name, const std::string& project_name,
                                     bool android) {
    return default_makefile_out_dir(config_name, android) + "/obj/" + project_name;
}

std::string stable_hash8(const std::string& value) {
    unsigned int hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }

    std::stringstream ss;
    ss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << hash;
    return ss.str();
}

std::string sanitize_object_stem(const std::string& source_identity) {
    std::string normalized = source_identity;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const size_t slash = normalized.find_last_of('/');
    const size_t dot = normalized.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        normalized.erase(dot);
    }

    std::vector<std::string> components;
    std::stringstream parts(normalized);
    std::string component;
    while (std::getline(parts, component, '/')) {
        if (component.empty() || component == ".") {
            continue;
        }

        if (component == "..") {
            components.push_back("__");
            continue;
        }

        std::string sanitized;
        for (unsigned char c : component) {
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
                sanitized.push_back(static_cast<char>(c));
            } else {
                sanitized.push_back('_');
            }
        }

        if (sanitized.empty() || sanitized == "." || sanitized == "..") {
            sanitized = "_";
        }
        components.push_back(sanitized);
    }

    if (components.empty()) {
        components.push_back("source");
    }

    components.back() += "_" + stable_hash8(normalized);

    std::string result;
    for (const auto& part : components) {
        if (!result.empty()) result += "/";
        result += part;
    }
    return result + ".o";
}

bool has_extension_case_insensitive(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) {
        return false;
    }

    std::string suffix = to_lower(path.substr(path.size() - ext.size()));
    return suffix == ext;
}

} // namespace

// Convert Windows path to Unix path (backslash to forward slash)
std::string MakefileGenerator::to_unix_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// Helper to compute relative path from makefile directory
std::string MakefileGenerator::compute_relative_path(const std::string& path, const std::filesystem::path& makefile_dir) {
    namespace fs = std::filesystem;

    if (path.empty()) return ".";

    try {
        // Make both paths absolute and normalize
        fs::path abs_target = fs::absolute(path).lexically_normal();
        fs::path abs_base = fs::absolute(makefile_dir).lexically_normal();

        // Try standard library function
        fs::path rel_path = fs::relative(abs_target, abs_base);
        std::string result = rel_path.string();
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;

    } catch (...) {
        // Manual fallback when fs::relative() fails
        try {
            fs::path abs_target = fs::absolute(path).lexically_normal();
            fs::path abs_base = fs::absolute(makefile_dir).lexically_normal();

            // Check for different drives (Windows-specific)
            if (abs_target.root_name() != abs_base.root_name()) {
                std::string result = abs_target.string();
                std::replace(result.begin(), result.end(), '\\', '/');
                return result;
            }

            // Split paths into components
            std::vector<fs::path> target_parts;
            std::vector<fs::path> base_parts;

            for (const auto& part : abs_target) {
                if (part != "/" && part != "\\") {
                    target_parts.push_back(part);
                }
            }
            for (const auto& part : abs_base) {
                if (part != "/" && part != "\\") {
                    base_parts.push_back(part);
                }
            }

            // Find common prefix length
            size_t common = 0;
            while (common < target_parts.size() &&
                   common < base_parts.size() &&
                   target_parts[common] == base_parts[common]) {
                common++;
            }

            // Build relative path: "../" for each level up, then remaining components
            std::string result;
            for (size_t i = common; i < base_parts.size(); i++) {
                if (!result.empty()) result += "/";
                result += "..";
            }
            for (size_t i = common; i < target_parts.size(); i++) {
                if (!result.empty()) result += "/";
                result += target_parts[i].string();
            }

            return result.empty() ? "." : result;

        } catch (...) {
            // Ultimate fallback - return with Unix separators
            std::string result = path;
            std::replace(result.begin(), result.end(), '\\', '/');
            return result;
        }
    }
}

std::string MakefileGenerator::make_object_path(const SourceFile& src, const std::string& src_relative,
                                                const std::string& config_key, const std::string& int_dir,
                                                const std::filesystem::path& makefile_dir) {
    if (const auto* object_file = find_config_setting(src.settings.object_file, config_key);
        object_file && !object_file->empty()) {
        std::string obj_path = to_unix_path(*object_file);
        if (has_extension_case_insensitive(obj_path, ".obj")) {
            obj_path.replace(obj_path.size() - 4, 4, ".o");
        }

        std::filesystem::path path(obj_path);
        if (path.is_absolute()) {
            return compute_relative_path(obj_path, makefile_dir);
        }

        return obj_path;
    }

    return int_dir + sanitize_object_stem(src_relative);
}

// Strip .lib or .dll extension from library names
std::string MakefileGenerator::strip_lib_extension(const std::string& lib) {
    std::string result = lib;

    // Remove .lib extension if present
    if (result.size() > 4 && result.substr(result.size() - 4) == ".lib") {
        result = result.substr(0, result.size() - 4);
    }

    // Remove .dll extension if present
    if (result.size() > 4 && result.substr(result.size() - 4) == ".dll") {
        result = result.substr(0, result.size() - 4);
    }

    // Remove .a extension if present
    if (result.size() > 2 && result.substr(result.size() - 2) == ".a") {
        result = result.substr(0, result.size() - 2);
    }

    // Remove lib prefix if present (e.g., libpthread -> pthread)
    if (result.size() > 3 && result.substr(0, 3) == "lib") {
        result = result.substr(3);
    }

    return result;
}

// Determine if PCH is enabled for a configuration and return the PCH header path
std::pair<bool, std::string> MakefileGenerator::get_pch_info(const Configuration& config) {
    bool has_pch = !config.cl_compile.pch.mode.empty() &&
                   config.cl_compile.pch.mode != "NotUsing";
    std::string header = config.cl_compile.pch.header;
    return {has_pch, header};
}

// Check if a source file uses, creates, or doesn't use PCH
// Returns: (mode, header) tuple
std::tuple<std::string, std::string> MakefileGenerator::get_file_pch_mode(
    const SourceFile& src,
    const std::string& config_key,
    const Configuration& config) {

    if (const auto* pch = find_config_setting(src.settings.pch, config_key);
        pch && !pch->mode.empty()) {
        return {pch->mode, pch->header};
    }

    // Fall back to project-level PCH settings
    return {config.cl_compile.pch.mode, config.cl_compile.pch.header};
}

// Get all compiler flags for a configuration
std::string MakefileGenerator::get_compiler_flags(const Configuration& config, const Project& project,
                                                   const std::filesystem::path& makefile_dir, bool c_flags) {
    std::stringstream ss;

    // Language standard. Mixed C/C++ projects need distinct flags for each
    // compiler. Empty values fall back to the shared defaults (C17 / C++17).
    if (c_flags) {
        ss << lang::c_standard_to_gnu_flag(project.c_standard) << " ";
    } else {
        ss << lang::cpp_standard_to_gnu_flag(config.cl_compile.language_standard) << " ";
    }

    // Optimization
    if (!config.cl_compile.optimization.empty()) {
        ss << flags::optimization_to_gnu_flag(config.cl_compile.optimization) << " ";
    }

    // Debug information
    if (!config.cl_compile.debug_information_format.empty()) {
        ss << "-g ";
    }

    // Warning level
    if (!config.cl_compile.warning_level.empty()) {
        ss << flags::warning_level_to_gnu_flags(config.cl_compile.warning_level) << " ";
    }

    // Position-independent code for shared libraries
    if (config.config_type == "DynamicLibrary") {
        ss << "-fPIC ";
    }

    // Include directories - convert to relative paths
    for (const auto& inc : config.cl_compile.additional_include_directories) {
        for (const auto& part : split_semicolons(inc)) {
            std::string inc_path = compute_relative_path(part, makefile_dir);
            ss << "-I\"" << inc_path << "\" ";
        }
    }

    // Preprocessor definitions
    for (const auto& def : config.cl_compile.preprocessor_definitions) {
        ss << "-D" << def << " ";
    }

    // UTF-8 source encoding (ensure source files are read as UTF-8)
    if (config.cl_compile.utf8_source) {
        ss << "-finput-charset=UTF-8 -fexec-charset=UTF-8 ";
    }

    // Additional options (raw flags)
    if (!config.cl_compile.additional_options.empty()) {
        ss << config.cl_compile.additional_options << " ";
    }

    // Function-level linking (allows linker to remove unused functions)
    if (config.cl_compile.function_level_linking.value_or(false)) {
        ss << "-ffunction-sections ";
    }

    // Data sections (allows linker to remove unused data)
    if (config.link.enable_comdat_folding.value_or(false) || config.link.optimize_references.value_or(false)) {
        ss << "-fdata-sections ";
    }

    return ss.str();
}

// Get linker flags (library directories)
std::string MakefileGenerator::get_linker_flags(const Configuration& config, const std::filesystem::path& makefile_dir,
                                                bool android) {
    std::stringstream ss;

    // Android always links ELF binaries with lld, even when the makefile is
    // generated on a macOS host, so Mach-O style flags only apply to
    // non-Android builds generated on Apple hosts.
#ifdef __APPLE__
    const bool macho_linker = !android;
#else
    const bool macho_linker = false;
    (void)android;
#endif

    // Library directories - convert to relative paths
    for (const auto& libdir : config.link.additional_library_directories) {
        for (const auto& part : split_semicolons(libdir)) {
            std::string lib_path = compute_relative_path(part, makefile_dir);
            ss << "-L\"" << lib_path << "\" ";
        }
    }

    // Additional linker options
    if (!config.link.additional_options.empty()) {
        ss << config.link.additional_options << " ";
    }

    // Garbage collection of unused sections (equivalent to MSVC optimize_references + enable_comdat_folding)
    if (config.link.optimize_references.value_or(false) || config.link.enable_comdat_folding.value_or(false)) {
        ss << (macho_linker ? "-Wl,-dead_strip " : "-Wl,--gc-sections ");
    }

    // Base address
    if (!config.link.base_address.empty()) {
        if (!macho_linker) {
            ss << "-Wl,--image-base=" << config.link.base_address << " ";
        }
    }

    // Module definition file (.def)
    if (!config.link.module_definition_file.empty()) {
        std::string def_path = compute_relative_path(config.link.module_definition_file, makefile_dir);
        if (macho_linker) {
            ss << "-Wl,-exported_symbols_list," << def_path << " ";
        } else {
            ss << "-Wl,--version-script=" << def_path << " ";
        }
    }

    // Ignore all default libraries
    if (config.link.ignore_all_default_libraries) {
        ss << "-nodefaultlibs ";
    }

    return ss.str();
}

// Get linker libraries
std::string MakefileGenerator::get_linker_libs(const Configuration& config) {
    std::stringstream ss;

    // Additional dependencies (libraries)
    for (const auto& lib : config.link.additional_dependencies) {
        for (const auto& part : split_semicolons(lib)) {
            // Skip empty library names
            if (part.empty()) continue;

            // Pass through flags that start with - (e.g., -framework Metal)
            if (part[0] == '-') {
                ss << part << " ";
                continue;
            }

            std::string libname = strip_lib_extension(part);
            if (libname.empty()) continue;

            // Check if it's a full path to a .lib or .a file
            if (part.find('/') != std::string::npos || part.find('\\') != std::string::npos) {
                // It's a path, use it directly
                ss << to_unix_path(part) << " ";
            } else {
                // It's a library name, use -l flag
                ss << "-l" << libname << " ";
            }
        }
    }

    return ss.str();
}

// Generate a single Makefile for a project and configuration
bool MakefileGenerator::generate_makefile(const Project& project, const Solution& solution,
                                         const std::string& config_key, const std::string& output_path) {
    auto project_lookup = build_project_lookup(solution);
    return generate_makefile_with_lookup(project, solution, config_key, output_path, project_lookup);
}

bool MakefileGenerator::generate_makefile_with_lookup(const Project& project, const Solution& solution,
                                                      const std::string& config_key, const std::string& output_path,
                                                      const MakefileGenerator::ProjectLookup& project_lookup) {
    namespace fs = std::filesystem;
    (void)solution;

    // Get configuration
    auto it = project.configurations.find(config_key);
    if (it == project.configurations.end()) {
        std::cerr << "Error: Configuration '" << config_key << "' not found for project '" << project.name << "'\n";
        return false;
    }
    const Configuration& config = it->second;

    // Parse config key into config and platform
    std::string config_name, platform;
    size_t pipe_pos = config_key.find('|');
    if (pipe_pos != std::string::npos) {
        config_name = config_key.substr(0, pipe_pos);
        platform = config_key.substr(pipe_pos + 1);
    } else {
        config_name = config_key;
        platform = "x64";
    }

    const bool android = is_android_platform(platform);

    // Determine target name and extension
    std::string target_name = decorate_target_name(
        config.target_name.empty() ? project.name : config.target_name, config.config_type, android);
    std::string target_ext = config.target_ext;
    if (target_ext.empty()) {
        if (config.config_type == "Application") {
            target_ext = "";  // No extension for executables on Linux
        } else if (config.config_type == "Driver") {
            target_ext = ".sys";
        } else if (config.config_type == "DynamicLibrary") {
            if (android) {
                target_ext = ".so";  // Android always uses ELF .so, even when generated on macOS
            } else {
#ifdef __APPLE__
                target_ext = ".dylib";
#else
                target_ext = ".so";
#endif
            }
        } else if (config.config_type == "StaticLibrary") {
            target_ext = ".a";
        }
    }

    // Strip .exe extension for Makefiles (Linux)
    if (target_ext == ".exe") {
        target_ext = "";
    }

    // Get the directory containing the makefile for computing relative paths
    fs::path makefile_dir = fs::path(output_path).parent_path();
    if (makefile_dir.empty()) {
        makefile_dir = ".";
    }

    // Output and intermediate directories - convert to relative paths
    std::string out_dir = compute_relative_path(
        config.out_dir.empty() ? default_makefile_out_dir(config_name, android) : config.out_dir, makefile_dir);
    std::string int_dir = compute_relative_path(
        config.int_dir.empty() ? default_makefile_int_dir(config_name, project.name, android) : config.int_dir, makefile_dir);

    // Ensure directories end with /
    if (!out_dir.empty() && out_dir.back() != '/') out_dir += '/';
    if (!int_dir.empty() && int_dir.back() != '/') int_dir += '/';

    // Full target path
    std::string target = out_dir + target_name + target_ext;

    // Open output file
    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: Failed to create Makefile: " << output_path << "\n";
        return false;
    }

    // Write header
    out << "# Auto-generated Makefile for " << project.name << " (" << config_name
        << (android ? ", Android" : "") << ")\n";
    out << "# Generated by sighmake\n\n";

    // Determine compiler
    bool has_cpp_files = false;
    bool has_c_files = false;
    bool has_objcxx_files = false;
    bool has_nasm_files = false;
    for (const auto& src : project.sources) {
        if (src.type == FileType::ClCompile) {
            std::string ext = file_types::lowercase_extension(src.path);
            if (file_types::is_cpp_source(ext)) {
                has_cpp_files = true;
            } else if (file_types::is_c_source(ext)) {
                has_c_files = true;
            }
        } else if (src.type == FileType::ObjCxx) {
            has_objcxx_files = true;
        } else if (src.type == FileType::NASM) {
            has_nasm_files = true;
        }
    }

    // Compiler variables
    if (android) {
        // Android NDK toolchain. The --target wrappers are what the NDK's own
        // clang launcher scripts do, but calling clang directly works on every
        // host OS (the .cmd wrappers are Windows-only, the bare ones POSIX-only).
        out << "# Android NDK toolchain (requires NDK r19 or newer)\n";
        out << "# Override at build time, e.g.: make ANDROID_ABI=x86_64 ANDROID_API=26\n";
        out << "ANDROID_ABI ?= arm64-v8a\n";
        out << "ANDROID_API ?= 24\n";
        out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
        out << "  ANDROID_NDK_HOME := $(ANDROID_NDK_ROOT)\n";
        out << "endif\n";
        out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
        out << "  ifneq ($(MAKECMDGOALS),clean)\n";
        out << "    $(error ANDROID_NDK_HOME is not set. Point it at your Android NDK installation)\n";
        out << "  endif\n";
        out << "endif\n";
        out << "ifeq ($(ANDROID_ABI),arm64-v8a)\n";
        out << "  ANDROID_TRIPLE := aarch64-linux-android\n";
        out << "else ifeq ($(ANDROID_ABI),armeabi-v7a)\n";
        out << "  ANDROID_TRIPLE := armv7a-linux-androideabi\n";
        out << "else ifeq ($(ANDROID_ABI),x86_64)\n";
        out << "  ANDROID_TRIPLE := x86_64-linux-android\n";
        out << "else ifeq ($(ANDROID_ABI),x86)\n";
        out << "  ANDROID_TRIPLE := i686-linux-android\n";
        out << "else\n";
        out << "  $(error Unsupported ANDROID_ABI '$(ANDROID_ABI)'. Supported: arm64-v8a, armeabi-v7a, x86_64, x86)\n";
        out << "endif\n";
        out << "ANDROID_TOOLCHAIN := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/"
            << ANDROID_HOST_TAG << "/bin\n";
        out << "CXX = \"$(ANDROID_TOOLCHAIN)/clang++\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)\n";
        out << "CC = \"$(ANDROID_TOOLCHAIN)/clang\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)\n";
        out << "AR = \"$(ANDROID_TOOLCHAIN)/llvm-ar\"\n";
        out << "STRIP = \"$(ANDROID_TOOLCHAIN)/llvm-strip\"\n";
    } else {
        if (has_cpp_files) {
#ifdef __APPLE__
            out << "CXX = clang++\n";
#else
            out << "CXX = g++\n";
#endif
        }
        if (has_c_files) {
#ifdef __APPLE__
            out << "CC = clang\n";
#else
            out << "CC = gcc\n";
#endif
        }
        if (has_objcxx_files) {
            // ObjC++ uses the same C++ compiler (clang++ auto-detects .mm)
            if (!has_cpp_files) {
#ifdef __APPLE__
                out << "CXX = clang++\n";
#else
                out << "CXX = g++\n";
#endif
            }
        }
    }
    if (has_nasm_files) {
        std::string nasm_exe = config.nasm.path.empty() ? "nasm" : config.nasm.path;
        out << "NASM = " << nasm_exe << "\n";
    }

    // Compiler flags
    std::string cxxflags = get_compiler_flags(config, project, makefile_dir, false);
    std::string cflags = get_compiler_flags(config, project, makefile_dir, true);
    std::string ldflags = get_linker_flags(config, makefile_dir, android);
    std::string ldlibs = get_linker_libs(config);

    // Add project reference outputs (.a files) to link line, including transitive PUBLIC deps
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        // Collect all project .a files by traversing the dependency tree
        struct ArchiveEntry {
            std::string path;
            bool whole_archive;
        };
        std::vector<ArchiveEntry> dep_archives;
        std::set<std::string> visited_deps;

        // Helper to compute a project's output archive path
        auto get_archive_path = [&](const Project& dep_proj, const std::string& cfg_key,
                                     const std::string& cfg_name) -> std::string {
            auto dep_config_it = dep_proj.configurations.find(cfg_key);
            if (dep_config_it == dep_proj.configurations.end()) return "";
            const auto& dep_config = dep_config_it->second;

            if (dep_config.config_type != "StaticLibrary" &&
                dep_config.config_type != "DynamicLibrary") return "";

            std::string dep_target_name = decorate_target_name(
                dep_config.target_name.empty() ? dep_proj.name : dep_config.target_name,
                dep_config.config_type, android);
            std::string dep_target_ext = dep_config.target_ext;
            if (dep_target_ext.empty()) {
                if (dep_config.config_type == "StaticLibrary") {
                    dep_target_ext = ".a";
                } else if (android) {
                    dep_target_ext = ".so";
                } else {
#ifdef __APPLE__
                    dep_target_ext = ".dylib";
#else
                    dep_target_ext = ".so";
#endif
                }
            }

            std::string dep_out_dir = dep_config.out_dir.empty()
                ? default_makefile_out_dir(cfg_name, android)
                : dep_config.out_dir;
            if (!dep_out_dir.empty() && dep_out_dir.back() != '/' && dep_out_dir.back() != '\\')
                dep_out_dir += '/';

            return dep_out_dir + dep_target_name + dep_target_ext;
        };

        // Recursive traversal: visit a dependency and its transitive PUBLIC deps
        // whole_archive flag only applies to the direct dependency, not transitive ones
        std::function<void(const std::string&, bool, bool)> collect_deps =
            [&](const std::string& dep_name, bool add_locally, bool is_whole_archive) {
            if (visited_deps.count(dep_name)) return;
            visited_deps.insert(dep_name);

            const Project* dep_proj = find_project(project_lookup, dep_name);
            if (!dep_proj) return;

            // Add this project's archive if it's a library
            if (add_locally) {
                std::string archive = get_archive_path(*dep_proj, config_key, config_name);
                if (!archive.empty()) {
                    dep_archives.push_back({compute_relative_path(archive, makefile_dir), is_whole_archive});
                }
            }

            // Follow transitive PUBLIC dependencies (transitive deps are never whole-archive)
            for (const auto& sub_dep : dep_proj->project_references) {
                if (sub_dep.visibility == DependencyVisibility::PRIVATE) continue;
                if (!sub_dep.link_library_dependencies) continue;
                collect_deps(sub_dep.name, true, false);
            }
        };

        // Start from the project's direct dependencies
        for (const auto& dep : project.project_references) {
            if (dep.visibility == DependencyVisibility::INTERFACE) continue;
            if (!dep.link_library_dependencies) continue;
            collect_deps(dep.name, true, dep.whole_archive);
        }

        // Prepend archives to ldlibs (reverse order so direct deps come first)
        for (auto archive_it = dep_archives.rbegin(); archive_it != dep_archives.rend(); ++archive_it) {
            if (archive_it->whole_archive) {
#ifdef __APPLE__
                const bool macho_linker = !android;
#else
                const bool macho_linker = false;
#endif
                if (macho_linker) {
                    ldlibs = "-Wl,-force_load," + archive_it->path + " " + ldlibs;
                } else {
                    ldlibs = "-Wl,--whole-archive " + archive_it->path + " -Wl,--no-whole-archive " + ldlibs;
                }
            } else {
                ldlibs = archive_it->path + " " + ldlibs;
            }
        }
    }

    if (has_cpp_files || has_objcxx_files) {
        out << "CXXFLAGS = " << cxxflags << "\n";
    }
    if (has_c_files) {
        out << "CFLAGS = " << cflags << "\n";
    }
    if (has_objcxx_files) {
        out << "OBJCXXFLAGS = $(CXXFLAGS)";
        if (!config.cl_compile.objcxx_flags.empty()) {
            out << " " << config.cl_compile.objcxx_flags;
        }
        out << "\n";
    }

    if (has_nasm_files) {
        // Build NASMFLAGS from config
        std::string nasm_fmt = config.nasm.format;
        if (nasm_fmt.empty()) nasm_fmt = "elf64";  // Default for Makefile (Linux)

        std::string nasmflags = "-f " + nasm_fmt;
        for (const auto& inc : config.nasm.include_directories) {
            std::string rel_inc = compute_relative_path(inc, makefile_dir);
            nasmflags += " -I" + rel_inc + "/";
        }
        for (const auto& def : config.nasm.preprocessor_definitions) {
            nasmflags += " -D" + def;
        }
        if (!config.nasm.additional_options.empty()) {
            nasmflags += " " + config.nasm.additional_options;
        }
        out << "NASMFLAGS = " << nasmflags << "\n";
    }

    if (!ldflags.empty()) {
        out << "LDFLAGS = " << ldflags << "\n";
    }
    if (!ldlibs.empty()) {
        out << "LDLIBS = " << ldlibs << "\n";
    }

    out << "\n";

    // Output paths
    out << "# Output\n";
    out << "TARGET = " << target << "\n";
    out << "OBJ_DIR = " << int_dir << "\n\n";

    // Check if PCH is enabled for this configuration
    auto [has_pch, pch_header] = get_pch_info(config);
    std::string pch_header_path;
    std::string pch_output_path;
    std::string pch_include_base; // The path to include (without .gch extension)

    if (has_pch && !pch_header.empty()) {
        // Compute relative path to PCH header
        pch_header_path = compute_relative_path(pch_header, makefile_dir);

        // PCH output path: $(OBJ_DIR)/pch_filename.gch
        pch_output_path = int_dir + fs::path(pch_header).filename().string() + ".gch";

        // The include base is the path without .gch extension (for -include flag)
        pch_include_base = int_dir + fs::path(pch_header).filename().string();

        // Write PCH variables
        out << "# Precompiled header\n";
        out << "PCH_HEADER = " << pch_header_path << "\n";
        out << "PCH_OUTPUT = " << pch_output_path << "\n\n";
    }

    // Collect source files and generate object file list
    std::vector<std::string> obj_files;
    std::vector<std::pair<std::string, std::string>> source_to_obj; // (source, object)
    std::vector<bool> source_uses_pch; // Track which files use PCH

    for (const auto& src : project.sources) {
        if (src.type == FileType::ClCompile || src.type == FileType::ObjCxx || src.type == FileType::NASM) {
            // Check if excluded for this config
            const bool* excluded = find_config_setting(src.settings.excluded, config_key);
            if (excluded && *excluded) {
                continue; // Skip excluded files
            }

            // Check PCH mode for this file
            auto [mode, header] = get_file_pch_mode(src, config_key, config);

            // Skip files with "Create" mode - in GCC, we compile the header directly
            if (mode == "Create") {
                continue;
            }

            // Generate object file path
            // Convert source path to relative (using the same logic as output dirs)
            std::string src_relative = compute_relative_path(src.path, makefile_dir);
            std::string obj_path = make_object_path(src, src_relative, config_key, int_dir, makefile_dir);

            // Determine if this file uses PCH
            bool uses_pch = has_pch && (mode == "Use" || (mode.empty() && !header.empty()));

            obj_files.push_back(obj_path);
            source_to_obj.push_back({src_relative, obj_path});
            source_uses_pch.push_back(uses_pch);
        }
    }

    // Object files list
    out << "# Object files\n";
    out << "OBJS =";
    for (const auto& obj : obj_files) {
        out << " \\\n  " << obj;
    }
    out << "\n\n";

    // Phony targets
    if (!config.pre_build_event.command.empty()) {
        out << ".PHONY: all clean prebuild\n\n";
    } else {
        out << ".PHONY: all clean\n\n";
    }

    // Pre-build event
    if (!config.pre_build_event.command.empty()) {
        out << "# Pre-build event\n";
        out << "prebuild:\n";
        out << "\t" << config.pre_build_event.command << "\n\n";
    }

    // Default target
    out << "all: $(TARGET)\n\n";

    // PCH compilation rule
    if (has_pch && !pch_header_path.empty()) {
        out << "# Precompiled header compilation\n";
        out << "$(PCH_OUTPUT): $(PCH_HEADER)\n";
        out << "\t@mkdir -p $(dir $@)\n";
        out << "\t$(CXX) $(CXXFLAGS) -x c++-header -o $@ $<\n\n";
    }

    // Link rule
    if (!config.pre_build_event.command.empty()) {
        out << "$(TARGET): prebuild $(OBJS)\n";
    } else {
        out << "$(TARGET): $(OBJS)\n";
    }
    out << "\t@mkdir -p $(dir $@)\n";

    // Pre-link event
    if (!config.pre_link_event.command.empty()) {
        out << "\t" << config.pre_link_event.command << "\n";
    }

    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        // Link executable or shared library
        std::string compiler = has_cpp_files ? "$(CXX)" : "$(CC)";
        if (config.config_type == "DynamicLibrary") {
            out << "\t" << compiler << " -shared $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)\n";
        } else {
            out << "\t" << compiler << " $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)\n";
        }
    } else if (config.config_type == "StaticLibrary") {
        // Create static library (Android uses the NDK's llvm-ar via $(AR))
        out << "\t" << (android ? "$(AR)" : "ar") << " rcs $@ $(OBJS)\n";
    }

    // Post-build event
    if (!config.post_build_event.command.empty()) {
        out << "\t" << config.post_build_event.command << "\n";
    }

    // Strip debug symbols for Release builds (executables and shared libraries only)
    // On Linux, debug symbols are embedded in the binary unlike Windows .pdb files
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        std::string release_config_name = config_key;
        size_t release_pipe_pos = release_config_name.find('|');
        if (release_pipe_pos != std::string::npos) {
            release_config_name = release_config_name.substr(0, release_pipe_pos);
        }
        if (release_config_name == "Release") {
            if (android) {
                // Host strip can't handle Android ELF binaries; use the NDK's llvm-strip
                out << "\t$(STRIP) --strip-unneeded $@\n";
            } else {
#ifdef __APPLE__
                if (config.config_type == "DynamicLibrary") {
                    out << "\tstrip -x $@\n";
                } else {
                    out << "\tstrip $@\n";
                }
#else
                out << "\tstrip $@\n";
#endif
            }
        }
    }

    out << "\n";

    // Compilation rules for each source file
    for (size_t i = 0; i < source_to_obj.size(); ++i) {
        const auto& [src, obj] = source_to_obj[i];
        bool uses_pch = source_uses_pch[i];

        std::string ext = file_types::lowercase_extension(src);

        std::string compiler;
        std::string flags;
        bool is_nasm = false;
        if (file_types::is_cpp_source(ext)) {
            compiler = "$(CXX)";
            flags = "$(CXXFLAGS)";
        } else if (file_types::is_c_source(ext)) {
            compiler = "$(CC)";
            flags = "$(CFLAGS)";
        } else if (file_types::is_objcxx_source(ext)) {
            compiler = "$(CXX)";
            flags = "$(OBJCXXFLAGS)";
        } else if (ext == ".asm" || ext == ".nasm") {
            compiler = "$(NASM)";
            flags = "$(NASMFLAGS)";
            is_nasm = true;
        } else {
            continue; // Skip unknown file types
        }

        // Write dependency line - add PCH dependency if file uses it
        out << obj << ": " << src;
        if (!is_nasm && uses_pch && has_pch) {
            out << " $(PCH_OUTPUT)";
        }
        out << "\n";

        out << "\t@mkdir -p $(dir $@)\n";

        if (is_nasm) {
            out << "\t" << compiler << " " << flags << " -o $@ $<\n\n";
        } else {
            out << "\t" << compiler << " " << flags;

            // Add -include flag to force PCH inclusion for files that use it
            if (uses_pch && has_pch && !pch_include_base.empty()) {
                out << " -include \"" << pch_include_base << "\"";
            }

            out << " -MMD -MP -c -o $@ $<\n\n";
        }
    }

    // Clean rule
    out << "clean:\n";
    if (has_pch && !pch_output_path.empty()) {
        out << "\trm -rf $(OBJ_DIR) $(TARGET) $(PCH_OUTPUT)\n\n";
    } else {
        out << "\trm -rf $(OBJ_DIR) $(TARGET)\n\n";
    }

    // Include dependency files
    if (!obj_files.empty()) {
        out << "# Include dependencies\n";
        out << "-include $(OBJS:.o=.d)\n";
    }

    out.close();

    std::cout << "Generated: " << output_path << "\n";
    return true;
}

// Generate all Makefiles for a solution
bool MakefileGenerator::generate(Solution& solution, const std::string& output_dir) {
    namespace fs = std::filesystem;

    // Create output directory if it doesn't exist
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        try {
            fs::create_directories(output_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create output directory: " << e.what() << "\n";
            return false;
        }
    }

    // Create build directory
    fs::path build_dir = fs::path(output_dir) / "build";
    if (!fs::exists(build_dir)) {
        try {
            fs::create_directories(build_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create build directory: " << e.what() << "\n";
            return false;
        }
    }

    std::cout << "Generating Makefiles for solution: " << solution.name << "\n";

    auto project_lookup = build_project_lookup(solution);

    // Generate Makefiles for each project and configuration
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;  // Skip synthetic find_package projects
        // Generate a Makefile for each configuration
        for (const auto& [config_key, config] : project.configurations) {
            // Parse config key
            std::string config_name, platform;
            size_t pipe_pos = config_key.find('|');
            if (pipe_pos != std::string::npos) {
                config_name = config_key.substr(0, pipe_pos);
                platform = config_key.substr(pipe_pos + 1);
            } else {
                config_name = config_key;
            }

            // Skip Windows configs for makefile generator
            if (is_windows_platform(platform)) continue;

            // Generate Makefile path (Android configs get their own makefiles,
            // e.g. App.Debug.Android, so they don't collide with Linux/macOS ones)
            std::string makefile_name =
                make_project_config_target(project, config_name, is_android_platform(platform));
            fs::path makefile_path = build_dir / makefile_name;

            if (!generate_makefile_with_lookup(project, solution, config_key, makefile_path.string(), project_lookup)) {
                return false;
            }
        }
    }

    // Generate master Makefile to build all projects
    if (!generate_master_makefile(solution, output_dir)) {
        return false;
    }

    std::cout << "Makefile generation complete!\n";

    // Write build cache for --build support
    {
        BuildCache cache;
        cache.generator = "makefile";
        cache.solution_name = solution.name;
        cache.configurations = solution.configurations;
        for (const auto& p : solution.platforms) {
            if (!is_windows_platform(p)) {
                cache.platforms.push_back(p);
            }
        }
        cache.build_dir = "build";
        cache.write(output_dir);
    }

    return true;
}

bool MakefileGenerator::generate_master_makefile(const Solution& solution, const std::string& output_dir) {
    namespace fs = std::filesystem;

    fs::path build_dir = fs::path(output_dir) / "build";
    fs::path makefile_path = build_dir / "Makefile";

    std::ofstream out(makefile_path);
    if (!out) {
        std::cerr << "Error: Failed to create master Makefile: " << makefile_path << "\n";
        return false;
    }

    // Collect unique config names (without platform), skipping Windows platforms.
    // Android configs are tracked separately - they build through the NDK and get
    // their own <Config>.Android targets.
    std::set<std::string> configs;
    std::set<std::string> android_configs;
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;  // Skip synthetic find_package projects
        for (const auto& [config_key, config] : project.configurations) {
            size_t pipe_pos = config_key.find('|');
            std::string config_name = (pipe_pos != std::string::npos)
                ? config_key.substr(0, pipe_pos)
                : config_key;
            std::string platform = (pipe_pos != std::string::npos)
                ? config_key.substr(pipe_pos + 1)
                : "";
            if (is_windows_platform(platform)) continue;  // Skip Windows configs
            if (is_android_platform(platform)) {
                android_configs.insert(config_name);
            } else {
                configs.insert(config_name);
            }
        }
    }

    if ((configs.empty() && android_configs.empty()) || solution.projects.empty()) {
        // Nothing to build
        out << "# Empty solution - no targets\n";
        out << "all:\n\t@echo \"No projects to build\"\n";
        std::cerr << "Warning: No non-Windows platforms found. Makefile has no targets.\n";
        std::cerr << "  Hint: Add 'Linux' or 'Android' to your platforms list, e.g.: platforms = x64, Linux\n";
        return true;
    }

    // Determine default config (prefer Debug, otherwise first alphabetically)
    std::string default_config = configs.count("Debug") ? "Debug" : (configs.empty() ? "" : *configs.begin());
    std::string default_android_config = android_configs.count("Debug")
        ? "Debug" : (android_configs.empty() ? "" : *android_configs.begin());
    auto project_lookup = build_project_lookup(solution);

    std::vector<const Project*> buildable_projects;
    for (const auto& project : solution.projects) {
        if (!project.is_package_project) {
            buildable_projects.push_back(&project);
        }
    }

    out << "# Master Makefile - generated by sighmake\n";
    out << "# Build all projects with: make\n";
    out << "# Build specific config:   make Release\n";
    out << "# Build specific project:  make ProjectName\n";
    if (!android_configs.empty()) {
        out << "# Build for Android:       make android\n";
        out << "# Android config/ABI:      make Release.Android ANDROID_ABI=x86_64 ANDROID_API=26\n";
    }
    out << "# Clean all:               make clean\n";
    out << "# Install:                 sudo make install\n";
    out << "# Install to custom dir:   make install PREFIX=/opt/myapp\n\n";

    out << "PREFIX ?= /usr/local\n\n";

    // .PHONY targets
    out << ".PHONY: all clean install uninstall";
    for (const auto& cfg : configs) {
        out << " " << cfg;
    }
    if (!android_configs.empty()) {
        out << " android";
        for (const auto& cfg : android_configs) {
            out << " " << cfg << ".Android";
        }
    }
    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;
        out << " " << proj.name;
    }
    for (const auto& cfg : configs) {
        for (const auto* proj : buildable_projects) {
            if (find_makefile_config_key(*proj, cfg, false)) {
                out << " " << make_project_config_target(*proj, cfg);
            }
        }
    }
    for (const auto& cfg : android_configs) {
        for (const auto* proj : buildable_projects) {
            if (find_makefile_config_key(*proj, cfg, true)) {
                out << " " << make_project_config_target(*proj, cfg, true);
            }
        }
    }
    out << "\n\n";

    // Default target. Solutions with only Android platforms default to the
    // Android build; otherwise desktop configs stay the default and Android is
    // built explicitly via `make android` / `make <Config>.Android`.
    if (!default_config.empty()) {
        out << "all: " << default_config << "\n\n";
    } else {
        out << "all: " << default_android_config << ".Android\n\n";
    }

    // Convenience alias for the default Android config
    if (!android_configs.empty()) {
        out << "android: " << default_android_config << ".Android\n\n";
    }

    // Per-configuration targets (e.g., make Debug, make Release, make Debug.Android)
    auto write_config_targets = [&](const std::set<std::string>& config_set, bool android) {
        for (const auto& cfg : config_set) {
            out << cfg << (android ? ".Android" : "") << ":";
            for (const auto* proj : buildable_projects) {
                if (find_makefile_config_key(*proj, cfg, android)) {
                    out << " " << make_project_config_target(*proj, cfg, android);
                }
            }
            out << "\n\n";

            for (const auto* proj : buildable_projects) {
                if (!find_makefile_config_key(*proj, cfg, android)) {
                    continue;
                }

                const std::string target = make_project_config_target(*proj, cfg, android);
                std::set<std::string> dependencies;
                for (const auto& dep : proj->project_references) {
                    const Project* dep_project = find_project(project_lookup, dep.name);
                    if (!dep_project || !find_makefile_config_key(*dep_project, cfg, android)) {
                        continue;
                    }
                    dependencies.insert(make_project_config_target(*dep_project, cfg, android));
                }

                out << target << ":";
                for (const auto& dep_target : dependencies) {
                    out << " " << dep_target;
                }
                out << "\n";
                out << "\t$(MAKE) -f " << target << "\n\n";
            }
        }
    };
    write_config_targets(configs, false);
    write_config_targets(android_configs, true);

    // Per-project targets (builds default config; falls back to the Android
    // default when the project only has Android configurations)
    for (const auto* proj : buildable_projects) {
        out << proj->name << ":";
        if (!default_config.empty() && find_makefile_config_key(*proj, default_config, false)) {
            out << " " << make_project_config_target(*proj, default_config);
        } else if (!default_android_config.empty() &&
                   find_makefile_config_key(*proj, default_android_config, true)) {
            out << " " << make_project_config_target(*proj, default_android_config, true);
        }
        out << "\n\n";
    }

    // Clean target
    out << "clean:\n";
    for (const auto* proj : buildable_projects) {
        for (const auto& cfg : configs) {
            if (find_makefile_config_key(*proj, cfg, false)) {
                out << "\t-$(MAKE) -f " << make_project_config_target(*proj, cfg) << " clean\n";
            }
        }
        for (const auto& cfg : android_configs) {
            if (find_makefile_config_key(*proj, cfg, true)) {
                out << "\t-$(MAKE) -f " << make_project_config_target(*proj, cfg, true) << " clean\n";
            }
        }
    }
    out << "\n";

    // Install target - install executable targets from Release (or default) config.
    // Android configs are excluded: their binaries target devices, not the host.
    if (!configs.empty()) {
        std::string install_config = configs.count("Release") ? "Release" : default_config;

        // Collect installable targets (executables and shared libraries)
        struct InstallTarget {
            std::string binary_path;  // Relative to build dir
            std::string name;         // Binary name for destination
        };
        std::vector<InstallTarget> exe_targets;
        std::vector<InstallTarget> lib_targets;

        for (const auto& proj : solution.projects) {
            if (proj.is_package_project) continue;
            for (const auto& [config_key, config] : proj.configurations) {
                size_t pipe_pos = config_key.find('|');
                std::string cfg_name = (pipe_pos != std::string::npos)
                    ? config_key.substr(0, pipe_pos) : config_key;
                std::string plat = (pipe_pos != std::string::npos)
                    ? config_key.substr(pipe_pos + 1) : "";

                if (cfg_name != install_config) continue;
                if (is_windows_platform(plat)) continue;
                if (is_android_platform(plat)) continue;  // Device binaries - never installed on the host

                std::string target_name = config.target_name.empty() ? proj.name : config.target_name;
                std::string out_dir = config.out_dir.empty() ? "build/" + cfg_name : config.out_dir;
                // Make path relative to build dir
                fs::path out_path = fs::path(out_dir);
                std::string rel_out = out_path.is_absolute()
                    ? compute_relative_path(out_path.string(), build_dir)
                    : compute_relative_path((fs::path(output_dir) / out_dir).string(), build_dir);
                if (!rel_out.empty() && rel_out.back() != '/') rel_out += '/';

                if (config.config_type == "Application") {
                    exe_targets.push_back({rel_out + target_name, target_name});
                } else if (config.config_type == "DynamicLibrary") {
#ifdef __APPLE__
                    std::string ext = ".dylib";
#else
                    std::string ext = ".so";
#endif
                    lib_targets.push_back({rel_out + "lib" + target_name + ext, "lib" + target_name + ext});
                }
                break;  // Only need one non-Windows platform config per project
            }
        }

        if (!exe_targets.empty() || !lib_targets.empty()) {
            out << "install: " << install_config << "\n";
            if (!exe_targets.empty()) {
                out << "\tinstall -d $(DESTDIR)$(PREFIX)/bin\n";
                for (const auto& t : exe_targets) {
                    out << "\tinstall -m 755 " << t.binary_path << " $(DESTDIR)$(PREFIX)/bin/" << t.name << "\n";
                }
            }
            if (!lib_targets.empty()) {
                out << "\tinstall -d $(DESTDIR)$(PREFIX)/lib\n";
                for (const auto& t : lib_targets) {
                    out << "\tinstall -m 755 " << t.binary_path << " $(DESTDIR)$(PREFIX)/lib/" << t.name << "\n";
                }
            }
            out << "\n";

            out << "uninstall:\n";
            for (const auto& t : exe_targets) {
                out << "\trm -f $(DESTDIR)$(PREFIX)/bin/" << t.name << "\n";
            }
            for (const auto& t : lib_targets) {
                out << "\trm -f $(DESTDIR)$(PREFIX)/lib/" << t.name << "\n";
            }
            out << "\n";
        }
    }

    std::cout << "Generated master Makefile: " << makefile_path << "\n";
    return true;
}

// Register this generator with the factory
REGISTER_GENERATOR(MakefileGenerator, "makefile");

} // namespace vcxproj
