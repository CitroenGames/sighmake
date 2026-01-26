#include "pch.h"
#include "makefile_generator.hpp"

namespace vcxproj {

// Convert Windows path to Unix path (backslash to forward slash)
std::string MakefileGenerator::to_unix_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// Make path relative or keep as-is
std::string MakefileGenerator::make_relative_or_keep(const std::string& path, const std::string& /*base*/) {
    // For simplicity, just convert to Unix path
    // In a more complete implementation, we would compute relative paths
    return to_unix_path(path);
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

// Map MSVC optimization to GCC optimization flags
std::string MakefileGenerator::map_optimization(const std::string& opt) {
    if (opt == "Disabled") {
        return "-O0";
    } else if (opt == "MinSpace") {
        return "-Os";
    } else if (opt == "MaxSpeed") {
        return "-O3";
    } else if (opt == "Full") {
        return "-O3";
    }
    return "-O2"; // Default
}

// Map MSVC warning level to GCC warning flags
std::string MakefileGenerator::map_warning_level(const std::string& level) {
    if (level == "Level0") {
        return "-w"; // Disable all warnings
    } else if (level == "Level1") {
        return "-Wall";
    } else if (level == "Level2") {
        return "-Wall";
    } else if (level == "Level3") {
        return "-Wall";
    } else if (level == "Level4") {
        return "-Wall -Wextra";
    }
    return "-Wall"; // Default
}

// Map C++ standard (stdcpp14, stdcpp17, stdcpp20) to GCC standard flags
std::string MakefileGenerator::map_language_standard(const std::string& std) {
    if (std == "stdcpp14" || std == "14") {
        return "-std=c++14";
    } else if (std == "stdcpp17" || std == "17") {
        return "-std=c++17";
    } else if (std == "stdcpp20" || std == "20") {
        return "-std=c++20";
    } else if (std == "stdcpp23" || std == "23") {
        return "-std=c++23";
    } else if (std == "stdcpplatest") {
        return "-std=c++23";
    }
    return "-std=c++17"; // Default to C++17
}

// Map C standard (89, 99, 11, 17, 23) to GCC standard flags
static std::string map_c_standard(const std::string& std) {
    if (std == "89" || std == "90") {
        return "-std=c89";
    } else if (std == "99") {
        return "-std=c99";
    } else if (std == "11") {
        return "-std=c11";
    } else if (std == "17") {
        return "-std=c17";
    } else if (std == "23") {
        return "-std=c2x";  // GCC uses c2x for C23
    }
    return "-std=c17"; // Default to C17
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

    // Check file-level PCH settings first (config-specific)
    auto pch_it = src.settings.pch.find(config_key);
    if (pch_it != src.settings.pch.end() && !pch_it->second.mode.empty()) {
        return {pch_it->second.mode, pch_it->second.header};
    }

    // Fall back to ALL_CONFIGS wildcard
    pch_it = src.settings.pch.find("ALL_CONFIGS");
    if (pch_it != src.settings.pch.end() && !pch_it->second.mode.empty()) {
        return {pch_it->second.mode, pch_it->second.header};
    }

    // Fall back to project-level PCH settings
    return {config.cl_compile.pch.mode, config.cl_compile.pch.header};
}

// Get all compiler flags for a configuration
std::string MakefileGenerator::get_compiler_flags(const Configuration& config, const Project& project,
                                                   const std::filesystem::path& makefile_dir) {
    std::stringstream ss;

    // Language standard - detect project language and use appropriate standard
    std::string detected_language = detect_project_language(project);
    if (detected_language == "C") {
        // C project - use C standard
        if (!project.c_standard.empty()) {
            ss << map_c_standard(project.c_standard) << " ";
        } else {
            ss << "-std=c17 "; // Default C standard
        }
    } else {
        // C++ project - use C++ standard
        if (!config.cl_compile.language_standard.empty()) {
            ss << map_language_standard(config.cl_compile.language_standard) << " ";
        } else {
            ss << "-std=c++17 "; // Default C++ standard
        }
    }

    // Optimization
    if (!config.cl_compile.optimization.empty()) {
        ss << map_optimization(config.cl_compile.optimization) << " ";
    }

    // Debug information
    if (!config.cl_compile.debug_information_format.empty()) {
        ss << "-g ";
    }

    // Warning level
    if (!config.cl_compile.warning_level.empty()) {
        ss << map_warning_level(config.cl_compile.warning_level) << " ";
    }

    // Position-independent code for shared libraries
    if (config.config_type == "DynamicLibrary") {
        ss << "-fPIC ";
    }

    // Include directories - convert to relative paths
    for (const auto& inc : config.cl_compile.additional_include_directories) {
        std::string inc_path = compute_relative_path(inc, makefile_dir);
        ss << "-I" << inc_path << " ";
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
    if (config.cl_compile.function_level_linking) {
        ss << "-ffunction-sections ";
    }

    // Data sections (allows linker to remove unused data)
    if (config.link.enable_comdat_folding || config.link.optimize_references) {
        ss << "-fdata-sections ";
    }

    return ss.str();
}

// Get linker flags (library directories)
std::string MakefileGenerator::get_linker_flags(const Configuration& config, const std::filesystem::path& makefile_dir) {
    std::stringstream ss;

    // Library directories - convert to relative paths
    for (const auto& libdir : config.link.additional_library_directories) {
        std::string lib_path = compute_relative_path(libdir, makefile_dir);
        ss << "-L" << lib_path << " ";
    }

    // Additional linker options
    if (!config.link.additional_options.empty()) {
        ss << config.link.additional_options << " ";
    }

    // Garbage collection of unused sections (equivalent to MSVC optimize_references + enable_comdat_folding)
    if (config.link.optimize_references || config.link.enable_comdat_folding) {
        ss << "-Wl,--gc-sections ";
    }

    return ss.str();
}

// Get linker libraries
std::string MakefileGenerator::get_linker_libs(const Configuration& config) {
    std::stringstream ss;

    // Additional dependencies (libraries)
    for (const auto& lib : config.link.additional_dependencies) {
        std::string libname = strip_lib_extension(lib);

        // Skip empty library names
        if (libname.empty()) continue;

        // Check if it's a full path to a .lib or .a file
        if (lib.find('/') != std::string::npos || lib.find('\\') != std::string::npos) {
            // It's a path, use it directly
            ss << to_unix_path(lib) << " ";
        } else {
            // It's a library name, use -l flag
            ss << "-l" << libname << " ";
        }
    }

    return ss.str();
}

// Generate a single Makefile for a project and configuration
bool MakefileGenerator::generate_makefile(const Project& project, const Solution& /*solution*/,
                                         const std::string& config_key, const std::string& output_path) {
    namespace fs = std::filesystem;

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

    // Determine target name and extension
    std::string target_name = config.target_name.empty() ? project.name : config.target_name;
    std::string target_ext = config.target_ext;
    if (target_ext.empty()) {
        if (config.config_type == "Application") {
            target_ext = "";  // No extension for executables on Linux
        } else if (config.config_type == "DynamicLibrary") {
            target_ext = ".so";
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
    std::string out_dir = compute_relative_path(config.out_dir.empty() ? "build/" + config_name : config.out_dir, makefile_dir);
    std::string int_dir = compute_relative_path(config.int_dir.empty() ? "build/" + config_name + "/obj" : config.int_dir, makefile_dir);

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
    out << "# Auto-generated Makefile for " << project.name << " (" << config_name << ")\n";
    out << "# Generated by sighmake\n\n";

    // Determine compiler
    bool has_cpp_files = false;
    bool has_c_files = false;
    for (const auto& src : project.sources) {
        if (src.type == FileType::ClCompile) {
            std::string ext = fs::path(src.path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                has_cpp_files = true;
            } else if (ext == ".c") {
                has_c_files = true;
            }
        }
    }

    // Compiler variables
    if (has_cpp_files) {
        out << "CXX = g++\n";
    }
    if (has_c_files) {
        out << "CC = gcc\n";
    }

    // Compiler flags
    std::string cxxflags = get_compiler_flags(config, project, makefile_dir);
    std::string ldflags = get_linker_flags(config, makefile_dir);
    std::string ldlibs = get_linker_libs(config);

    if (has_cpp_files) {
        out << "CXXFLAGS = " << cxxflags << "\n";
    }
    if (has_c_files) {
        out << "CFLAGS = " << cxxflags << "\n";
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
        if (src.type == FileType::ClCompile) {
            // Check if excluded for this config
            auto excl_it = src.settings.excluded.find(config_key);
            if (excl_it != src.settings.excluded.end() && excl_it->second) {
                continue; // Skip excluded files
            }

            // Check PCH mode for this file
            auto [mode, header] = get_file_pch_mode(src, config_key, config);

            // Skip files with "Create" mode - in GCC, we compile the header directly
            if (mode == "Create") {
                continue;
            }

            // Generate object file path
            fs::path src_path(src.path);
            std::string obj_name = src_path.stem().string() + ".o";
            std::string obj_path = int_dir + obj_name;

            // Convert source path to relative (using the same logic as output dirs)
            std::string src_relative = compute_relative_path(src.path, makefile_dir);

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
    out << ".PHONY: all clean\n\n";

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
    out << "$(TARGET): $(OBJS)\n";
    out << "\t@mkdir -p $(dir $@)\n";

    if (config.config_type == "Application" || config.config_type == "DynamicLibrary") {
        // Link executable or shared library
        std::string compiler = has_cpp_files ? "$(CXX)" : "$(CC)";
        if (config.config_type == "DynamicLibrary") {
            out << "\t" << compiler << " -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)\n";
        } else {
            out << "\t" << compiler << " $(LDFLAGS) -o $@ $^ $(LDLIBS)\n";
        }
    } else if (config.config_type == "StaticLibrary") {
        // Create static library
        out << "\tar rcs $@ $^\n";
    }

    // Strip debug symbols for Release builds (executables and shared libraries only)
    // On Linux, debug symbols are embedded in the binary unlike Windows .pdb files
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary") {
        std::string config_name = config_key;
        size_t pipe_pos = config_name.find('|');
        if (pipe_pos != std::string::npos) {
            config_name = config_name.substr(0, pipe_pos);
        }
        if (config_name == "Release") {
            out << "\tstrip $@\n";
        }
    }

    out << "\n";

    // Compilation rules for each source file
    for (size_t i = 0; i < source_to_obj.size(); ++i) {
        const auto& [src, obj] = source_to_obj[i];
        bool uses_pch = source_uses_pch[i];

        fs::path src_path(src);
        std::string ext = src_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        std::string compiler;
        std::string flags;
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
            compiler = "$(CXX)";
            flags = "$(CXXFLAGS)";
        } else if (ext == ".c") {
            compiler = "$(CC)";
            flags = "$(CFLAGS)";
        } else {
            continue; // Skip unknown file types
        }

        // Write dependency line - add PCH dependency if file uses it
        out << obj << ": " << src;
        if (uses_pch && has_pch) {
            out << " $(PCH_OUTPUT)";
        }
        out << "\n";

        out << "\t@mkdir -p $(dir $@)\n";
        out << "\t" << compiler << " " << flags;

        // Add -include flag to force PCH inclusion for files that use it
        if (uses_pch && has_pch && !pch_include_base.empty()) {
            out << " -include " << pch_include_base;
        }

        out << " -MMD -MP -c -o $@ $<\n\n";
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

    // Generate Makefiles for each project and configuration
    for (const auto& project : solution.projects) {
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

            // Generate Makefile path
            std::string makefile_name = project.name + "." + config_name;
            fs::path makefile_path = build_dir / makefile_name;

            if (!generate_makefile(project, solution, config_key, makefile_path.string())) {
                return false;
            }
        }
    }

    // Generate master Makefile to build all projects
    if (!generate_master_makefile(solution, output_dir)) {
        return false;
    }

    std::cout << "Makefile generation complete!\n";
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

    // Collect unique config names (without platform), skipping Windows platforms
    std::set<std::string> configs;
    for (const auto& project : solution.projects) {
        for (const auto& [config_key, config] : project.configurations) {
            size_t pipe_pos = config_key.find('|');
            std::string config_name = (pipe_pos != std::string::npos)
                ? config_key.substr(0, pipe_pos)
                : config_key;
            std::string platform = (pipe_pos != std::string::npos)
                ? config_key.substr(pipe_pos + 1)
                : "";
            if (is_windows_platform(platform)) continue;  // Skip Windows configs
            configs.insert(config_name);
        }
    }

    if (configs.empty() || solution.projects.empty()) {
        // Nothing to build
        out << "# Empty solution - no targets\n";
        out << "all:\n\t@echo \"No projects to build\"\n";
        return true;
    }

    // Determine default config (prefer Debug, otherwise first alphabetically)
    std::string default_config = configs.count("Debug") ? "Debug" : *configs.begin();

    out << "# Master Makefile - generated by sighmake\n";
    out << "# Build all projects with: make\n";
    out << "# Build specific config:   make Release\n";
    out << "# Build specific project:  make ProjectName\n";
    out << "# Clean all:               make clean\n\n";

    // .PHONY targets
    out << ".PHONY: all clean";
    for (const auto& cfg : configs) {
        out << " " << cfg;
    }
    for (const auto& proj : solution.projects) {
        out << " " << proj.name;
    }
    out << "\n\n";

    // Default target
    out << "all: " << default_config << "\n\n";

    // Per-configuration targets (e.g., make Debug, make Release)
    for (const auto& cfg : configs) {
        out << cfg << ":\n";
        for (const auto& proj : solution.projects) {
            out << "\t$(MAKE) -f " << proj.name << "." << cfg << "\n";
        }
        out << "\n";
    }

    // Per-project targets (builds default config)
    for (const auto& proj : solution.projects) {
        out << proj.name << ":\n";
        out << "\t$(MAKE) -f " << proj.name << "." << default_config << "\n\n";
    }

    // Clean target
    out << "clean:\n";
    for (const auto& proj : solution.projects) {
        for (const auto& cfg : configs) {
            out << "\t-$(MAKE) -f " << proj.name << "." << cfg << " clean\n";
        }
    }
    out << "\n";

    std::cout << "Generated master Makefile: " << makefile_path << "\n";
    return true;
}

// Register this generator with the factory
REGISTER_GENERATOR(MakefileGenerator, "makefile");

} // namespace vcxproj
