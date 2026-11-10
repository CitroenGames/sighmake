#include "pch.h"
#include "cmake_generator.hpp"
#include "common/build_cache.hpp"
#include "common/string_utils.hpp"
#include "common/compiler_flags.hpp"
#include "common/language_standards.hpp"
#include "common/output_manifest.hpp"

namespace vcxproj {

namespace fs = std::filesystem;

namespace {
std::string cmake_quote(const std::string& value) {
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}

// Quote a command argument that would otherwise be split (spaces) or misread (quotes, parentheses, ...)
std::string cmake_arg(const std::string& value) {
    if (!value.empty() && value.find_first_of(" \t\n\"()#\\") == std::string::npos) return value;
    return cmake_quote(value);
}

// True when `text` starts like a Windows path or a path made of a variable reference
bool starts_like_path(const std::string& text, size_t pos) {
    auto at = [&](size_t i) { return pos + i < text.size() ? text[pos + i] : '\0'; };
    if (std::isalpha(static_cast<unsigned char>(at(0))) && at(1) == ':' && (at(2) == '\\' || at(2) == '/')) return true;
    if (at(0) == '.' && at(1) == '\\') return true;
    if (at(0) == '.' && at(1) == '.' && at(2) == '\\') return true;
    if (at(0) == '\\' && at(1) == '\\') return true;
    return (at(0) == '$' && (at(1) == '{' || at(1) == '(' || at(1) == '<')) || at(0) == '%';
}

// Converts the backslashes of a build command line. Windows path separators become `/`. Backslashes
// inside a quoted string that does not start like a path are escape sequences for the program
// (`printf "a\n"`) and stay. With `words` false the result is CMake source text (quotes kept, kept
// backslashes escaped); with `words` true it is the list of decoded arguments.
std::vector<std::string> convert_command(const std::string& command, bool words) {
    std::vector<std::string> result;
    std::string current;
    bool in_word = false;
    char quote = 0;
    bool path_region = false;
    auto flush = [&]() {
        if (in_word) result.push_back(current);
        current.clear();
        in_word = false;
    };
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (words) flush();
            else current += c;
            continue;
        }
        in_word = true;
        if (quote == 0 && (c == '"' || c == '\'')) {
            quote = c;
            path_region = starts_like_path(command, i + 1);
            if (!words) current += c;
        } else if (quote != 0 && c == quote) {
            quote = 0;
            if (!words) current += c;
        } else if (c == '\\') {
            // CMake passes arguments to the shell without escaping backslashes, so a backslash that
            // must reach the program is doubled in the argument (`bs` is one backslash of the
            // argument, written as CMake source text when `words` is false)
            const std::string bs = words ? "\\" : "\\\\";
            const char next = i + 1 < command.size() ? command[i + 1] : '\0';
            if (quote == 0 || path_region) {
                if (quote == 0 && next == '"') {
                    current += bs + (words ? "\"" : "\\\"");
                    ++i;
                } else {
                    current += '/';
                }
            } else if (quote == '"' && next == '"') {
                current += bs + (words ? "\"" : "\\\"");
                ++i;
            } else if (quote == '"' && next == '\\') {
                current += bs + bs;
                ++i;
            } else {
                current += bs + bs;
            }
        } else {
            current += c;
        }
    }
    if (words) flush();
    else result.push_back(current);
    return result;
}

std::string command_to_cmake(const std::string& command) {
    return convert_command(command, false).front();
}

// Writes `COMMAND` lines. `commands` holds one (configuration, command text) pair per configuration;
// with `all_same` the first entry is written once for every configuration, otherwise each command is
// guarded so it only exists in its own configuration.
void write_command_lines(std::ostream& out, const std::vector<std::pair<std::string, std::string>>& commands,
                         bool all_same) {
    for (const auto& [cfg_name, command] : commands) {
        if (command.empty()) continue;
        // Each line is a separate command
        std::istringstream lines(unescape_newlines(command));
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty()) continue;
            if (all_same) {
                out << "    COMMAND " << command_to_cmake(line) << "\n";
                continue;
            }
            // Configuration-specific: every word becomes empty in the other configurations,
            // which drops the command there
            out << "    COMMAND";
            for (const auto& word : convert_command(line, true)) {
                std::string escaped;
                for (char c : word) {
                    if (c == '>') escaped += "$<ANGLE-R>";
                    else if (c == ',') escaped += "$<COMMA>";
                    else if (c == ';') escaped += "$<SEMICOLON>";
                    else escaped += c;
                }
                out << " " << cmake_arg("$<$<CONFIG:" + cfg_name + ">:" + escaped + ">");
            }
            out << "\n";
        }
        if (all_same) break;
    }
}
}


// Convert Windows path to CMake path (backslash to forward slash)
std::string CMakeGenerator::to_cmake_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// Compute relative path from base_dir
std::string CMakeGenerator::compute_relative_path(const std::string& path, const fs::path& base_dir) {
    if (path.empty()) return ".";

    try {
        fs::path abs_target = fs::absolute(path).lexically_normal();
        fs::path abs_base = fs::absolute(base_dir).lexically_normal();
        fs::path rel_path = fs::relative(abs_target, abs_base);
        return to_cmake_path(rel_path.string());
    } catch (...) {
        return to_cmake_path(path);
    }
}

// Collect unique config names (without platform) from solution
std::vector<std::string> CMakeGenerator::get_config_names(const Solution& solution) const {
    return solution.configurations;
}

// Find the first configuration matching a config name (any platform)
const Configuration* CMakeGenerator::find_config(const Project& project, const std::string& config_name) const {
    for (const auto& [key, config] : project.configurations) {
        auto [cfg, plat] = parse_config_key(key);
        if (cfg == config_name) return &config;
    }
    return nullptr;
}

// ============================================================================
// Target definition
// ============================================================================

void CMakeGenerator::write_target_definition(std::ostream& out, const Project& project, const Solution& /*solution*/) {
    // Determine target type from the first configuration
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }

    if (config_type == "ExternalCMake" || config_type == "AndroidApk") {
        // Third-party CMake builds are driven by the Makefile generator; keep the target name resolvable.
        out << "# external_cmake project: not built by this generator\n";
        out << "add_library(" << project.name << " INTERFACE)\n";
    } else if (config_type == "StaticLibrary") {
        out << "add_library(" << project.name << " STATIC)\n";
    } else if (config_type == "DynamicLibrary") {
        out << "add_library(" << project.name << " SHARED)\n";
    } else if (config_type == "Utility") {
        out << "add_library(" << project.name << " INTERFACE)\n";
    } else {
        // Application or Driver
        out << "add_executable(" << project.name << ")\n";
    }
}

// ============================================================================
// Sources
// ============================================================================

void CMakeGenerator::write_sources(std::ostream& out, const Project& project, const std::string& project_dir) {
    // Determine target type for visibility keyword
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }

    // Interface libraries cannot have PRIVATE sources
    if (config_type == "Utility") return;

    // Collect source files by type
    std::vector<std::string> compile_sources;
    std::vector<std::string> header_sources;
    std::vector<std::string> resource_sources;
    std::vector<std::string> masm_sources;
    std::vector<std::string> def_sources;

    for (const auto& src : project.sources) {
        std::string rel_path = compute_relative_path(src.path, project_dir);

        switch (src.type) {
            case FileType::ClCompile:
            case FileType::ObjCxx:
                compile_sources.push_back(rel_path);
                break;
            case FileType::ClInclude:
                header_sources.push_back(rel_path);
                break;
            case FileType::ResourceCompile:
                resource_sources.push_back(rel_path);
                break;
            case FileType::MASM:
                masm_sources.push_back(rel_path);
                break;
            case FileType::NASM:
                // Handled separately in write_nasm_support
                break;
            default:
                // Check for .def files
                if (src.path.size() > 4 && src.path.substr(src.path.size() - 4) == ".def") {
                    def_sources.push_back(rel_path);
                }
                break;
        }
    }

    if (compile_sources.empty() && header_sources.empty() &&
        resource_sources.empty() && masm_sources.empty() && def_sources.empty()) {
        return;
    }

    out << "\ntarget_sources(" << project.name << " PRIVATE\n";

    for (const auto& s : compile_sources) {
        out << "    " << cmake_arg(s) << "\n";
    }
    for (const auto& s : header_sources) {
        out << "    " << cmake_arg(s) << "\n";
    }
    for (const auto& s : resource_sources) {
        out << "    " << cmake_arg(s) << "\n";
    }
    for (const auto& s : masm_sources) {
        out << "    " << cmake_arg(s) << "\n";
    }
    for (const auto& s : def_sources) {
        out << "    " << cmake_arg(s) << "\n";
    }

    out << ")\n";
}

// ============================================================================
// Include directories
// ============================================================================

void CMakeGenerator::write_include_directories(std::ostream& out, const Project& project,
                                                const Solution& solution, const std::string& project_dir) {
    // Determine target type
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    std::string visibility = (config_type == "Utility") ? "INTERFACE" : "PRIVATE";

    // Collect includes that are common to all configs
    std::set<std::string> common_includes;
    bool first = true;
    for (const auto& [key, config] : project.configurations) {
        std::set<std::string> cfg_includes;
        for (const auto& inc : config.cl_compile.additional_include_directories) {
            for (const auto& part : split_semicolons(inc)) {
                cfg_includes.insert(to_cmake_path(part));
            }
        }
        if (first) {
            common_includes = cfg_includes;
            first = false;
        } else {
            std::set<std::string> intersection;
            for (const auto& i : common_includes) {
                if (cfg_includes.count(i)) intersection.insert(i);
            }
            common_includes = intersection;
        }
    }

    // Public includes
    std::vector<std::string> public_incs;
    for (const auto& inc : project.public_includes) {
        public_incs.push_back(compute_relative_path(inc, project_dir));
    }

    if (!common_includes.empty() || !public_incs.empty()) {
        out << "\ntarget_include_directories(" << project.name << "\n";

        if (!public_incs.empty()) {
            out << "    PUBLIC\n";
            for (const auto& inc : public_incs) {
                out << "        " << cmake_arg(inc) << "\n";
            }
        }

        if (!common_includes.empty()) {
            out << "    " << visibility << "\n";
            for (const auto& inc : common_includes) {
                out << "        " << cmake_arg(compute_relative_path(inc, project_dir)) << "\n";
            }
        }

        out << ")\n";
    }

    // Per-config includes that differ from common
    auto config_names = get_config_names(solution);
    bool has_per_config = false;
    for (const auto& cfg_name : config_names) {
        const Configuration* config = find_config(project, cfg_name);
        if (!config) continue;

        std::vector<std::string> extra;
        for (const auto& inc : config->cl_compile.additional_include_directories) {
            for (const auto& part : split_semicolons(inc)) {
                std::string normalized = to_cmake_path(part);
                if (!common_includes.count(normalized)) {
                    // CMake does not absolutize paths produced by a generator expression
                    std::string rel = compute_relative_path(normalized, project_dir);
                    if (!fs::path(rel).is_absolute()) rel = "${CMAKE_CURRENT_SOURCE_DIR}/" + rel;
                    extra.push_back(rel);
                }
            }
        }
        if (!extra.empty()) {
            if (!has_per_config) {
                out << "\n# Per-configuration include directories\n";
                has_per_config = true;
            }
            out << "target_include_directories(" << project.name << " " << visibility << "\n";
            for (const auto& e : extra) {
                out << "    " << cmake_arg("$<$<CONFIG:" + cfg_name + ">:" + e + ">") << "\n";
            }
            out << ")\n";
        }
    }
}

// ============================================================================
// Compile definitions
// ============================================================================

void CMakeGenerator::write_compile_definitions(std::ostream& out, const Project& project, const Solution& solution) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    std::string visibility = (config_type == "Utility") ? "INTERFACE" : "PRIVATE";

    // Collect common defines across all configs
    std::set<std::string> common_defines;
    bool first = true;
    // Release gains NDEBUG and Debug gains _DEBUG (unless the project sets either), the
    // same default the other generators apply. Utility targets compile nothing.
    auto effective_defines = [&](const std::string& cfg_name, const Configuration& config) {
        if (config_type == "Utility") return config.cl_compile.preprocessor_definitions;
        return flags::with_default_build_defines(cfg_name, config.cl_compile.preprocessor_definitions);
    };
    for (const auto& [key, config] : project.configurations) {
        auto defs = effective_defines(parse_config_key(key).first, config);
        std::set<std::string> cfg_defines(defs.begin(), defs.end());
        if (first) {
            common_defines = cfg_defines;
            first = false;
        } else {
            std::set<std::string> intersection;
            for (const auto& d : common_defines) {
                if (cfg_defines.count(d)) intersection.insert(d);
            }
            common_defines = intersection;
        }
    }

    // Public defines
    std::vector<std::string> public_defs = project.public_defines;

    auto config_names = get_config_names(solution);

    // Per-config defines
    struct ConfigDefines {
        std::string name;
        std::vector<std::string> defines;
    };
    std::vector<ConfigDefines> per_config;
    for (const auto& cfg_name : config_names) {
        const Configuration* config = find_config(project, cfg_name);
        if (!config) continue;

        std::vector<std::string> extra;
        for (const auto& def : effective_defines(cfg_name, *config)) {
            if (!common_defines.count(def)) {
                extra.push_back(def);
            }
        }
        if (!extra.empty()) {
            per_config.push_back({cfg_name, extra});
        }
    }

    if (common_defines.empty() && public_defs.empty() && per_config.empty()) return;

    out << "\ntarget_compile_definitions(" << project.name << "\n";

    if (!public_defs.empty()) {
        out << "    PUBLIC\n";
        for (const auto& d : public_defs) {
            out << "        " << cmake_arg(d) << "\n";
        }
    }

    if (!common_defines.empty() || !per_config.empty()) {
        out << "    " << visibility << "\n";
        for (const auto& d : common_defines) {
            out << "        " << cmake_arg(d) << "\n";
        }
        for (const auto& pc : per_config) {
            for (const auto& d : pc.defines) {
                out << "        " << cmake_arg("$<$<CONFIG:" + pc.name + ">:" + d + ">") << "\n";
            }
        }
    }

    out << ")\n";
}

// ============================================================================
// Compile options
// ============================================================================

void CMakeGenerator::write_compile_options(std::ostream& out, const Project& project, const Solution& solution) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    if (config_type == "Utility") return;

    auto config_names = get_config_names(solution);
    const std::string cid = detect_project_language(project) == "C" ? "C_COMPILER_ID" : "CXX_COMPILER_ID";

    // Collect MSVC and GCC/Clang options per config
    struct ConfigOpts {
        std::string name;
        std::vector<std::string> msvc_opts;
        std::vector<std::string> gcc_opts;
    };
    std::vector<ConfigOpts> per_config;

    // Shared options (non-config-specific)
    std::vector<std::string> shared_msvc;
    std::vector<std::string> shared_gcc;

    // Get the first config for shared options
    const Configuration* first_config = nullptr;
    for (const auto& [key, config] : project.configurations) {
        first_config = &config;
        break;
    }
    if (!first_config) return;

    // Warning level (usually same across configs)
    if (const auto* wl = flags::find_warning_level(first_config->cl_compile.warning_level)) {
        shared_msvc.push_back(wl->msvc);
        std::istringstream gnu_flags(wl->gnu);
        std::string flag;
        while (gnu_flags >> flag) {
            shared_gcc.push_back(flag);
        }
    }

    // Exception handling
    if (!first_config->cl_compile.exception_handling.empty()) {
        const std::string& eh = first_config->cl_compile.exception_handling;
        if (eh == "false") {
            // No exceptions - MSVC doesn't set /EH, GCC uses -fno-exceptions
            shared_gcc.push_back("-fno-exceptions");
        } else if (eh == "Sync") {
            shared_msvc.push_back("/EHsc");
        } else if (eh == "Async") {
            shared_msvc.push_back("/EHa");
        }
    }

    // RTTI
    if (!first_config->cl_compile.runtime_type_info) {
        shared_msvc.push_back("/GR-");
        shared_gcc.push_back("-fno-rtti");
    }

    // UTF-8
    if (first_config->cl_compile.utf8_source.value_or(false)) {
        shared_msvc.push_back("/utf-8");
        shared_gcc.push_back("-finput-charset=UTF-8");
        shared_gcc.push_back("-fexec-charset=UTF-8");
    }

    // Multi-processor compilation
    if (first_config->cl_compile.multi_processor_compilation) {
        shared_msvc.push_back("/MP");
    }

    // Treat warnings as errors
    if (first_config->cl_compile.treat_warning_as_error) {
        shared_msvc.push_back("/WX");
        shared_gcc.push_back("-Werror");
    }

    // Forced include files
    for (const auto& fi : first_config->cl_compile.forced_include_files) {
        shared_msvc.push_back("/FI\"" + to_cmake_path(fi) + "\"");
        shared_gcc.push_back("-include");
        shared_gcc.push_back(to_cmake_path(fi));
    }

    // Kernel mode flags
    if (project.is_kernel_mode) {
        shared_msvc.push_back("/kernel");
        shared_gcc.push_back("-ffreestanding");
        shared_gcc.push_back("-fno-exceptions");
        shared_gcc.push_back("-fno-rtti");
    }

    // Per-config options
    for (const auto& cfg_name : config_names) {
        const Configuration* config = find_config(project, cfg_name);
        if (!config) continue;

        ConfigOpts opts;
        opts.name = cfg_name;

        // Optimization
        if (const auto* opt = flags::find_optimization(config->cl_compile.optimization)) {
            opts.msvc_opts.push_back(opt->msvc);
            opts.gcc_opts.push_back(opt->gnu);
        }

        // Debug info
        if (!config->cl_compile.debug_information_format.empty()) {
            if (const char* flag = flags::debug_format_to_msvc_flag(config->cl_compile.debug_information_format)) {
                opts.msvc_opts.push_back(flag);
            }
            opts.gcc_opts.push_back("-g");
        }

        // Runtime library
        if (const char* flag = flags::runtime_library_to_msvc_flag(config->cl_compile.runtime_library)) {
            opts.msvc_opts.push_back(flag);
        }

        // Basic runtime checks
        if (!config->cl_compile.basic_runtime_checks.empty() &&
            config->cl_compile.basic_runtime_checks != "Default") {
            if (config->cl_compile.basic_runtime_checks == "EnableFastChecks") {
                opts.msvc_opts.push_back("/RTC1");
            }
        }

        // Function-level linking
        if (config->cl_compile.function_level_linking.value_or(false)) {
            opts.msvc_opts.push_back("/Gy");
            opts.gcc_opts.push_back("-ffunction-sections");
            opts.gcc_opts.push_back("-fdata-sections");
        }

        // Additional raw flags
        if (!config->cl_compile.additional_options.empty()) {
            // Split by spaces and add each as a separate option
            std::istringstream iss(config->cl_compile.additional_options);
            std::string flag;
            while (iss >> flag) {
                // Determine if this is an MSVC or GCC flag
                if (!flag.empty() && flag[0] == '/') {
                    opts.msvc_opts.push_back(flag);
                } else if (!flag.empty() && flag[0] == '-') {
                    opts.gcc_opts.push_back(flag);
                }
            }
        }

        if (!opts.msvc_opts.empty() || !opts.gcc_opts.empty()) {
            per_config.push_back(std::move(opts));
        }
    }

    bool has_content = !shared_msvc.empty() || !shared_gcc.empty() || !per_config.empty();
    if (!has_content) return;

    out << "\ntarget_compile_options(" << project.name << " PRIVATE\n";

    // Shared MSVC options
    if (!shared_msvc.empty()) {
        for (const auto& opt : shared_msvc) {
            out << "    $<$<" << cid << ":MSVC>:" << opt << ">\n";
        }
    }

    // Shared GCC/Clang options
    if (!shared_gcc.empty()) {
        for (const auto& opt : shared_gcc) {
            out << "    $<$<NOT:$<" << cid << ":MSVC>>:" << opt << ">\n";
        }
    }

    // Per-config options
    for (const auto& pc : per_config) {
        for (const auto& opt : pc.msvc_opts) {
            out << "    $<$<AND:$<" << cid << ":MSVC>,$<CONFIG:" << pc.name << ">>:" << opt << ">\n";
        }
        for (const auto& opt : pc.gcc_opts) {
            out << "    $<$<AND:$<NOT:$<" << cid << ":MSVC>>,$<CONFIG:" << pc.name << ">>:" << opt << ">\n";
        }
    }

    out << ")\n";
}

// ============================================================================
// Link libraries
// ============================================================================

void CMakeGenerator::write_link_libraries(std::ostream& out, const Project& project, const Solution& solution) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }

    // Collect project dependencies grouped by visibility
    // Each entry is (name, whole_archive)
    std::vector<std::pair<std::string, bool>> public_deps;
    std::vector<std::pair<std::string, bool>> private_deps;
    std::vector<std::pair<std::string, bool>> interface_deps;

    for (const auto& dep : project.project_references) {
        auto entry = std::make_pair(dep.name, dep.whole_archive);
        switch (dep.visibility) {
            case DependencyVisibility::PUBLIC:
                public_deps.push_back(entry);
                break;
            case DependencyVisibility::PRIVATE:
                private_deps.push_back(entry);
                break;
            case DependencyVisibility::INTERFACE:
                interface_deps.push_back(entry);
                break;
        }
    }

    // Collect library dependencies common across all configs
    std::set<std::string> common_libs;
    bool first = true;
    for (const auto& [key, config] : project.configurations) {
        std::set<std::string> cfg_libs;
        for (const auto& lib : config.link.additional_dependencies) {
            for (const auto& part : split_semicolons(lib)) {
                if (!part.empty()) cfg_libs.insert(part);
            }
        }
        if (first) {
            common_libs = cfg_libs;
            first = false;
        } else {
            std::set<std::string> intersection;
            for (const auto& l : common_libs) {
                if (cfg_libs.count(l)) intersection.insert(l);
            }
            common_libs = intersection;
        }
    }

    // Per-config libs
    auto config_names = get_config_names(solution);
    const std::string cid = detect_project_language(project) == "C" ? "C_COMPILER_ID" : "CXX_COMPILER_ID";
    struct ConfigLibs {
        std::string name;
        std::vector<std::string> libs;
    };
    std::vector<ConfigLibs> per_config;
    for (const auto& cfg_name : config_names) {
        const Configuration* config = find_config(project, cfg_name);
        if (!config) continue;

        std::vector<std::string> extra;
        for (const auto& lib : config->link.additional_dependencies) {
            for (const auto& part : split_semicolons(lib)) {
                if (!part.empty() && !common_libs.count(part)) {
                    extra.push_back(part);
                }
            }
        }
        if (!extra.empty()) {
            per_config.push_back({cfg_name, extra});
        }
    }

    // Public libs from project
    for (const auto& lib : project.public_libs) {
        common_libs.insert(lib);
    }

    bool has_content = !public_deps.empty() || !private_deps.empty() || !interface_deps.empty() ||
                       !common_libs.empty() || !per_config.empty();
    std::string default_vis = (config_type == "Utility") ? "INTERFACE" : "PRIVATE";

    if (has_content) {
        out << "\ntarget_link_libraries(" << project.name << "\n";

        if (!public_deps.empty()) {
            out << "    PUBLIC\n";
            for (const auto& [name, wa] : public_deps) {
                if (wa) {
                    out << "        $<LINK_LIBRARY:WHOLE_ARCHIVE," << name << ">\n";
                } else {
                    out << "        " << name << "\n";
                }
            }
        }
        if (!private_deps.empty()) {
            out << "    PRIVATE\n";
            for (const auto& [name, wa] : private_deps) {
                if (wa) {
                    out << "        $<LINK_LIBRARY:WHOLE_ARCHIVE," << name << ">\n";
                } else {
                    out << "        " << name << "\n";
                }
            }
        }
        if (!interface_deps.empty()) {
            out << "    INTERFACE\n";
            for (const auto& [name, wa] : interface_deps) {
                if (wa) {
                    out << "        $<LINK_LIBRARY:WHOLE_ARCHIVE," << name << ">\n";
                } else {
                    out << "        " << name << "\n";
                }
            }
        }

        if (!common_libs.empty() || !per_config.empty()) {
            // Use default visibility for raw libs
            out << "    " << default_vis << "\n";
            for (const auto& lib : common_libs) {
                // Skip libs that are already project deps
                bool is_proj_dep = false;
                for (const auto& dep : project.project_references) {
                    if (dep.name == lib) { is_proj_dep = true; break; }
                }
                if (is_proj_dep) continue;
                out << "        " << cmake_arg(to_cmake_path(lib)) << "\n";
            }
            for (const auto& pc : per_config) {
                for (const auto& lib : pc.libs) {
                    out << "        " << cmake_arg("$<$<CONFIG:" + pc.name + ">:" + to_cmake_path(lib) + ">") << "\n";
                }
            }
        }

        out << ")\n";
    }

    // Linker options (subsystem, entry point, etc.)
    std::vector<std::string> msvc_link_opts;
    std::vector<std::string> gcc_link_opts;

    if (!project.configurations.empty()) {
        const Configuration* first_cfg = nullptr;
        for (const auto& [key, config] : project.configurations) {
            first_cfg = &config;
            break;
        }
        if (first_cfg) {
            // Subsystem
            if (const char* flag = flags::subsystem_to_msvc_flag(first_cfg->link.sub_system)) {
                msvc_link_opts.push_back(flag);
            }

            // Entry point
            if (!first_cfg->link.entry_point_symbol.empty()) {
                msvc_link_opts.push_back("/ENTRY:" + first_cfg->link.entry_point_symbol);
                gcc_link_opts.push_back("-e");
                gcc_link_opts.push_back(first_cfg->link.entry_point_symbol);
            }

            // Ignore all default libraries
            if (first_cfg->link.ignore_all_default_libraries) {
                msvc_link_opts.push_back("/NODEFAULTLIB");
                gcc_link_opts.push_back("-nodefaultlibs");
            }

            // Ignore specific default libraries
            for (const auto& lib : first_cfg->link.ignore_specific_default_libraries) {
                if (!lib.empty()) {
                    msvc_link_opts.push_back("/NODEFAULTLIB:" + lib);
                }
            }

            // Additional linker options
            if (!first_cfg->link.additional_options.empty()) {
                std::istringstream iss(first_cfg->link.additional_options);
                std::string flag;
                while (iss >> flag) {
                    if (!flag.empty() && flag[0] == '/') {
                        msvc_link_opts.push_back(flag);
                    } else if (!flag.empty() && flag[0] == '-') {
                        gcc_link_opts.push_back(flag);
                    }
                }
            }

            // Generate debug info
            if (first_cfg->link.generate_debug_info) {
                msvc_link_opts.push_back("/DEBUG");
            }

            // Map file
            if (first_cfg->link.generate_map_file) {
                msvc_link_opts.push_back("/MAP");
            }

            // Large address aware
            if (first_cfg->link.large_address_aware) {
                msvc_link_opts.push_back("/LARGEADDRESSAWARE");
            }
        }
    }

    // Per-config gc-sections for release-type configs
    std::vector<std::string> gc_configs;
    for (const auto& cfg_name : config_names) {
        const Configuration* cfg = find_config(project, cfg_name);
        if (cfg && (cfg->link.optimize_references.value_or(false) || cfg->link.enable_comdat_folding.value_or(false))) {
            gc_configs.push_back(cfg_name);
        }
    }

    if (!msvc_link_opts.empty() || !gcc_link_opts.empty() || !gc_configs.empty()) {
        out << "\ntarget_link_options(" << project.name << " " << default_vis << "\n";
        for (const auto& opt : msvc_link_opts) {
            out << "    $<$<" << cid << ":MSVC>:" << opt << ">\n";
        }
        for (const auto& opt : gcc_link_opts) {
            out << "    $<$<NOT:$<" << cid << ":MSVC>>:" << opt << ">\n";
        }

        for (const auto& cfg_name : gc_configs) {
            out << "    $<$<AND:$<NOT:$<" << cid << ":MSVC>>,$<CONFIG:" << cfg_name << ">>:-Wl,--gc-sections>\n";
            out << "    $<$<AND:$<" << cid << ":MSVC>,$<CONFIG:" << cfg_name << ">>:/OPT:REF>\n";
            out << "    $<$<AND:$<" << cid << ":MSVC>,$<CONFIG:" << cfg_name << ">>:/OPT:ICF>\n";
        }

        out << ")\n";
    }
}

// ============================================================================
// Link directories
// ============================================================================

void CMakeGenerator::write_link_directories(std::ostream& out, const Project& project,
                                             const Solution& solution, const std::string& project_dir) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    if (config_type == "Utility") return;

    // Collect common library directories
    std::set<std::string> common_libdirs;
    bool first = true;
    for (const auto& [key, config] : project.configurations) {
        std::set<std::string> cfg_libdirs;
        for (const auto& libdir : config.link.additional_library_directories) {
            for (const auto& part : split_semicolons(libdir)) {
                cfg_libdirs.insert(to_cmake_path(part));
            }
        }
        if (first) {
            common_libdirs = cfg_libdirs;
            first = false;
        } else {
            std::set<std::string> intersection;
            for (const auto& l : common_libdirs) {
                if (cfg_libdirs.count(l)) intersection.insert(l);
            }
            common_libdirs = intersection;
        }
    }

    // Public lib dirs
    std::vector<std::string> public_dirs;
    for (const auto& dir : project.public_libdirs) {
        public_dirs.push_back(compute_relative_path(dir, project_dir));
    }

    if (!common_libdirs.empty() || !public_dirs.empty()) {
        out << "\ntarget_link_directories(" << project.name << "\n";

        if (!public_dirs.empty()) {
            out << "    PUBLIC\n";
            for (const auto& d : public_dirs) {
                out << "        " << cmake_arg(d) << "\n";
            }
        }

        if (!common_libdirs.empty()) {
            out << "    PRIVATE\n";
            for (const auto& d : common_libdirs) {
                out << "        " << cmake_arg(compute_relative_path(d, project_dir)) << "\n";
            }
        }

        out << ")\n";
    }

    // Per-config library directories that differ from common
    for (const auto& cfg_name : get_config_names(solution)) {
        const Configuration* config = find_config(project, cfg_name);
        if (!config) continue;

        std::vector<std::string> extra;
        for (const auto& libdir : config->link.additional_library_directories) {
            for (const auto& part : split_semicolons(libdir)) {
                std::string normalized = to_cmake_path(part);
                if (common_libdirs.count(normalized)) continue;
                std::string rel = compute_relative_path(normalized, project_dir);
                if (!fs::path(rel).is_absolute()) rel = "${CMAKE_CURRENT_SOURCE_DIR}/" + rel;
                extra.push_back(rel);
            }
        }
        if (!extra.empty()) {
            out << "target_link_directories(" << project.name << " PRIVATE\n";
            for (const auto& e : extra) {
                out << "    " << cmake_arg("$<$<CONFIG:" + cfg_name + ">:" + e + ">") << "\n";
            }
            out << ")\n";
        }
    }
}

// ============================================================================
// Target properties
// ============================================================================

void CMakeGenerator::write_target_properties(std::ostream& out, const Project& project, const Solution& solution) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }

    std::vector<std::string> props;

    // Language standard
    std::string detected_lang = detect_project_language(project);
    if (detected_lang == "C") {
        props.push_back("C_STANDARD " + lang::c_standard_to_cmake(project.c_standard));
        props.push_back("C_STANDARD_REQUIRED ON");
    } else {
        // Check first config for language standard
        std::string cpp_std = "17";
        for (const auto& [key, config] : project.configurations) {
            if (!config.cl_compile.language_standard.empty()) {
                cpp_std = lang::cpp_standard_to_cmake(config.cl_compile.language_standard);
                break;
            }
        }
        props.push_back("CXX_STANDARD " + cpp_std);
        props.push_back("CXX_STANDARD_REQUIRED ON");
    }

    // Target name (if different from project name)
    std::string target_name;
    for (const auto& [key, config] : project.configurations) {
        if (!config.target_name.empty() && config.target_name != project.name) {
            target_name = config.target_name;
            break;
        }
    }
    if (!target_name.empty()) {
        props.push_back("OUTPUT_NAME \"" + target_name + "\"");
    }

    // Target extension
    std::string target_ext;
    for (const auto& [key, config] : project.configurations) {
        if (!config.target_ext.empty()) {
            target_ext = config.target_ext;
            break;
        }
    }
    if (!target_ext.empty()) {
        props.push_back("SUFFIX \"" + target_ext + "\"");
    }

    // Position-independent code, decided like the Makefile generator: shared libraries, static
    // libraries a shared library of the solution links, and `position_independent` overrides.
    // A static library is PIC when any of its configurations needs it (one CMake target serves
    // every configuration). Projects in "auto" mode that need no PIC keep CMake's default.
    bool position_independent = false;
    for (const auto& [key, config] : project.configurations) {
        (void)config;
        if (project_uses_position_independent_code(solution, project, key)) {
            position_independent = true;
            break;
        }
    }
    const bool pic_forced_off = position_independent_override(project) == 0 && config_type != "DynamicLibrary";
    if (position_independent) {
        props.push_back("POSITION_INDEPENDENT_CODE ON");
    } else if (pic_forced_off && (config_type == "StaticLibrary" || config_type == "Application")) {
        props.push_back("POSITION_INDEPENDENT_CODE OFF");
    }

    if (props.empty()) return;

    out << "\nset_target_properties(" << project.name << " PROPERTIES\n";
    for (const auto& p : props) {
        out << "    " << p << "\n";
    }
    out << ")\n";

    // PIC objects on ELF targets: -fno-semantic-interposition (GCC 5+, Clang 10+; older Clang
    // rejects the flag). `semantic_interposition = true` keeps the default model (LD_PRELOAD).
    if (position_independent && !project.semantic_interposition) {
        const std::string lang = detected_lang == "C" ? "C" : "CXX";
        out << "if(NOT APPLE AND NOT WIN32 AND (CMAKE_" << lang << "_COMPILER_ID STREQUAL \"GNU\" OR "
            << "(CMAKE_" << lang << "_COMPILER_ID STREQUAL \"Clang\" AND CMAKE_" << lang
            << "_COMPILER_VERSION VERSION_GREATER_EQUAL 10)))\n";
        out << "    target_compile_options(" << project.name << " PRIVATE -fno-semantic-interposition)\n";
        out << "endif()\n";
    }

    // ELF shared libraries: -Wl,-Bsymbolic-functions binds calls between the library's own functions
    // at link time, when the linker accepts it. The same `semantic_interposition = true` opts out.
    if (config_type == "DynamicLibrary" && !project.semantic_interposition) {
        const std::string lang = detected_lang == "C" ? "C" : "CXX";
        const std::string result_var = "SIGHMAKE_LINKER_BSYMBOLIC_FUNCTIONS_" + lang;
        out << "if(NOT APPLE AND NOT WIN32)\n";
        out << "    include(CheckLinkerFlag)\n";
        out << "    check_linker_flag(" << lang << " \"-Wl,-Bsymbolic-functions\" " << result_var << ")\n";
        out << "    if(" << result_var << ")\n";
        out << "        target_link_options(" << project.name << " PRIVATE -Wl,-Bsymbolic-functions)\n";
        out << "    endif()\n";
        out << "endif()\n";
    }

    // Per-config output directories
    auto config_names = get_config_names(solution);
    bool has_outdir = false;
    for (const auto& cfg_name : config_names) {
        const Configuration* config = find_config(project, cfg_name);
        if (config && !config->out_dir.empty()) {
            has_outdir = true;
            break;
        }
    }

    if (has_outdir) {
        out << "\n# Per-configuration output directories\n";
        for (const auto& cfg_name : config_names) {
            const Configuration* config = find_config(project, cfg_name);
            if (!config || config->out_dir.empty()) continue;

            std::string out_dir = to_cmake_path(config->out_dir);
            std::string upper_cfg = to_upper(cfg_name);

            if (config_type == "Application" || config_type == "Driver") {
                out << "set_target_properties(" << project.name << " PROPERTIES RUNTIME_OUTPUT_DIRECTORY_"
                    << upper_cfg << " \"" << out_dir << "\")\n";
            } else if (config_type == "DynamicLibrary") {
                out << "set_target_properties(" << project.name << " PROPERTIES LIBRARY_OUTPUT_DIRECTORY_"
                    << upper_cfg << " \"" << out_dir << "\")\n";
                out << "set_target_properties(" << project.name << " PROPERTIES RUNTIME_OUTPUT_DIRECTORY_"
                    << upper_cfg << " \"" << out_dir << "\")\n";
            } else if (config_type == "StaticLibrary") {
                out << "set_target_properties(" << project.name << " PROPERTIES ARCHIVE_OUTPUT_DIRECTORY_"
                    << upper_cfg << " \"" << out_dir << "\")\n";
            }
        }
    }
}

// ============================================================================
// Precompiled headers
// ============================================================================

void CMakeGenerator::write_pch_settings(std::ostream& out, const Project& project, const Solution& /*solution*/,
                                        const std::string& project_dir) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    if (config_type == "Utility") return;

    // Find PCH header from any config
    std::string pch_header;
    bool pch_enabled = false;
    for (const auto& [key, config] : project.configurations) {
        if (!config.cl_compile.pch.mode.empty() && config.cl_compile.pch.mode != "NotUsing") {
            pch_enabled = true;
            pch_header = config.cl_compile.pch.header;
            break;
        }
    }

    if (!pch_enabled || pch_header.empty()) return;

    // pch_header is the name sources #include: look for it next to the sources and in the include
    // directories. A header that is not found is included by name through the include path.
    std::string pch_arg;
    std::vector<fs::path> search_dirs;
    if (fs::path(pch_header).is_absolute()) search_dirs.emplace_back();
    for (const auto& src : project.sources) search_dirs.push_back(fs::path(src.path).parent_path());
    for (const auto& [key, config] : project.configurations) {
        for (const auto& inc : config.cl_compile.additional_include_directories) {
            for (const auto& part : split_semicolons(inc)) search_dirs.emplace_back(part);
        }
    }
    for (const auto& dir : search_dirs) {
        std::error_code ec;
        fs::path candidate = dir / pch_header;
        if (fs::is_regular_file(candidate, ec)) {
            pch_arg = cmake_arg(compute_relative_path(candidate.string(), project_dir));
            break;
        }
    }
    if (pch_arg.empty()) pch_arg = cmake_arg("<" + to_cmake_path(pch_header) + ">");

    out << "\ntarget_precompile_headers(" << project.name << " PRIVATE " << pch_arg << ")\n";

    // Files that skip PCH
    for (const auto& src : project.sources) {
        if (src.type != FileType::ClCompile && src.type != FileType::ObjCxx) continue;

        // Check if this file has NotUsing PCH or is the PCH creator
        bool skip_pch = false;
        for (const auto& [cfg_key, pch] : src.settings.pch) {
            if (pch.mode == "NotUsing" || pch.mode == "Create") {
                skip_pch = true;
                break;
            }
        }
        if (skip_pch) {
            out << "set_source_files_properties(" << cmake_arg(compute_relative_path(src.path, project_dir))
                << " PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n";
        }
    }
}

// ============================================================================
// Per-file settings
// ============================================================================

void CMakeGenerator::write_per_file_settings(std::ostream& out, const Project& project,
                                              const Solution& /*solution*/, const std::string& project_dir) {
    bool has_settings = false;

    for (const auto& src : project.sources) {
        if (src.type != FileType::ClCompile && src.type != FileType::ObjCxx) continue;

        std::string rel_path = compute_relative_path(src.path, project_dir);

        // Per-file defines
        std::vector<std::string> file_defines;
        for (const auto& [cfg_key, defs] : src.settings.preprocessor_defines) {
            const bool all_configs = cfg_key == ALL_CONFIGS || cfg_key == "ALL_CONFIGS";
            for (const auto& d : defs) {
                std::string entry = all_configs ? d : "$<$<CONFIG:" + parse_config_key(cfg_key).first + ">:" + d + ">";
                if (std::find(file_defines.begin(), file_defines.end(), entry) == file_defines.end()) {
                    file_defines.push_back(entry);
                }
            }
        }

        // Per-file excluded (in every configuration)
        auto excluded_in = [&](const std::string& config_key) {
            for (const char* key : {config_key.c_str(), ALL_CONFIGS, "ALL_CONFIGS"}) {
                auto it = src.settings.excluded.find(key);
                if (it != src.settings.excluded.end()) return it->second;
            }
            return false;
        };
        bool excluded_all = !project.configurations.empty();
        for (const auto& [cfg_key, config] : project.configurations) {
            if (!excluded_in(cfg_key)) {
                excluded_all = false;
                break;
            }
        }

        // Per-file compile_as
        std::string compile_as;
        for (const auto& [cfg_key, ca] : src.settings.compile_as) {
            compile_as = ca;
            break;
        }

        bool needs_props = !file_defines.empty() || excluded_all || !compile_as.empty();
        if (!needs_props) continue;

        if (!has_settings) {
            out << "\n# Per-file settings\n";
            has_settings = true;
        }

        out << "set_source_files_properties(" << cmake_arg(rel_path) << " PROPERTIES";

        if (!file_defines.empty()) {
            std::string joined;
            for (size_t i = 0; i < file_defines.size(); i++) {
                if (i > 0) joined += ";";
                joined += file_defines[i];
            }
            out << "\n    COMPILE_DEFINITIONS " << cmake_quote(joined);
        }

        if (excluded_all) {
            out << "\n    HEADER_FILE_ONLY TRUE";
        }

        if (compile_as == "CompileAsC") {
            out << "\n    LANGUAGE C";
        } else if (compile_as == "CompileAsCpp") {
            out << "\n    LANGUAGE CXX";
        }

        out << "\n)\n";
    }
}

// ============================================================================
// Build events
// ============================================================================

void CMakeGenerator::write_build_events(std::ostream& out, const Project& project, const Solution& solution) {
    auto config_names = get_config_names(solution);

    auto write_event = [&](BuildEvent Configuration::*member, const char* when) {
        // Commands of every configuration; one shared command list is written unconditionally
        std::vector<std::pair<std::string, const BuildEvent*>> events;
        bool all_same = true;
        for (const auto& cfg_name : config_names) {
            const Configuration* config = find_config(project, cfg_name);
            const BuildEvent* event = config ? &(config->*member) : nullptr;
            if (!events.empty() && (!event || !events.front().second ||
                                    event->command != events.front().second->command)) {
                all_same = false;
            }
            events.emplace_back(cfg_name, event);
        }
        const BuildEvent* message_source = nullptr;
        for (const auto& [cfg_name, event] : events) {
            if (event && !event->command.empty()) {
                message_source = event;
                break;
            }
        }
        if (!message_source) return;

        out << "\nadd_custom_command(TARGET " << project.name << " " << when << "\n";
        std::vector<std::pair<std::string, std::string>> commands;
        for (const auto& [cfg_name, event] : events) {
            commands.emplace_back(cfg_name, event ? event->command : std::string());
        }
        write_command_lines(out, commands, all_same);
        if (!message_source->message.empty()) {
            out << "    COMMENT " << cmake_quote(message_source->message) << "\n";
        }
        out << ")\n";
    };

    write_event(&Configuration::pre_build_event, "PRE_BUILD");
    write_event(&Configuration::pre_link_event, "PRE_LINK");
    write_event(&Configuration::post_build_event, "POST_BUILD");
}

// ============================================================================
// Custom build rules
// ============================================================================

void CMakeGenerator::write_custom_build_rules(std::ostream& out, const Project& project,
                                               const Solution& solution, const std::string& project_dir) {
    std::string config_type;
    for (const auto& [key, config] : project.configurations) {
        if (!config.config_type.empty()) {
            config_type = config.config_type;
            break;
        }
    }
    auto config_names = get_config_names(solution);
    int rule_index = 0;

    // Paths and commands of a rule are relative to the buildscript, like in the Visual Studio generator
    auto from_script = [&](const std::string& path) {
        fs::path p(path);
        if (p.is_relative() && !project.buildscript_path.empty()) p = fs::path(project.buildscript_path) / p;
        return p.string();
    };
    std::string working_dir = "${CMAKE_CURRENT_SOURCE_DIR}";
    if (!project.buildscript_path.empty()) {
        std::string rel = compute_relative_path(project.buildscript_path, project_dir);
        if (rel != ".") working_dir += "/" + rel;
    }

    for (const auto& src : project.sources) {
        if (src.type != FileType::CustomBuild) continue;

        std::string rel_path = compute_relative_path(src.path, project_dir);

        // Value of a per-configuration setting for a configuration name (ALL_CONFIGS applies to every one)
        auto value_for = [&](const std::map<std::string, std::string>& values, const std::string& cfg_name) {
            std::string fallback;
            for (const auto& [cfg_key, value] : values) {
                if (cfg_key == ALL_CONFIGS) fallback = value;
                else if (parse_config_key(cfg_key).first == cfg_name) return value;
            }
            return fallback;
        };

        std::vector<std::pair<std::string, std::string>> commands;
        std::vector<std::pair<std::string, std::string>> outputs;
        std::string description;
        bool commands_same = true;
        bool outputs_same = true;
        for (const auto& cfg_name : config_names) {
            std::string command = value_for(src.custom_command, cfg_name);
            std::string output = value_for(src.custom_outputs, cfg_name);
            if (!commands.empty() && command != commands.front().second) commands_same = false;
            if (!outputs.empty() && output != outputs.front().second) outputs_same = false;
            commands.emplace_back(cfg_name, command);
            outputs.emplace_back(cfg_name, output);
            if (description.empty()) description = value_for(src.custom_message, cfg_name);
        }
        if (commands.empty()) {
            // No solution configurations: fall back to whatever the rule declares
            for (const auto& [cfg_key, cmd] : src.custom_command) {
                commands.emplace_back(parse_config_key(cfg_key).first, cmd);
                outputs.emplace_back(parse_config_key(cfg_key).first, value_for(src.custom_outputs, parse_config_key(cfg_key).first));
                break;
            }
            if (!src.custom_message.empty()) description = src.custom_message.begin()->second;
        }
        if (std::all_of(commands.begin(), commands.end(), [](const auto& c) { return c.second.empty(); })) continue;

        // Outputs are written relative to the project directory (CMake would take them relative to the
        // build directory), one list per configuration
        auto output_list = [&](const std::string& value) {
            std::vector<std::string> list;
            for (const auto& part : split_semicolons(value)) {
                std::string t = trim(part);
                if (t.empty()) continue;
                std::string rel = compute_relative_path(from_script(t), project_dir);
                if (!fs::path(rel).is_absolute()) rel = "${CMAKE_CURRENT_SOURCE_DIR}/" + rel;
                list.push_back(rel);
            }
            return list;
        };
        std::vector<std::string> output_items;
        for (size_t i = 0; i < outputs.size(); ++i) {
            for (const auto& item : output_list(outputs[i].second)) {
                output_items.push_back(outputs_same ? item : "$<$<CONFIG:" + outputs[i].first + ">:" + item + ">");
            }
            if (outputs_same) break;
        }

        std::vector<std::string> depends{cmake_arg(rel_path)};
        const std::string inputs = value_for(src.custom_inputs, config_names.empty() ? std::string() : config_names.front());
        for (const auto& part : split_semicolons(inputs)) {
            std::string t = trim(part);
            if (!t.empty()) depends.push_back(cmake_arg(compute_relative_path(from_script(t), project_dir)));
        }

        const bool real_output = !output_items.empty();
        const std::string target_name = project.name + "_custom_" + std::to_string(rule_index++);
        if (real_output) {
            out << "\nadd_custom_command(\n    OUTPUT";
            for (const auto& item : output_items) out << " " << cmake_arg(item);
            out << "\n";
        } else {
            out << "\nadd_custom_target(" << target_name << " ALL\n";
        }
        write_command_lines(out, commands, commands_same);
        out << "    WORKING_DIRECTORY " << cmake_arg(working_dir) << "\n";
        out << "    DEPENDS";
        for (const auto& d : depends) out << " " << d;
        out << "\n";
        if (!description.empty()) {
            out << "    COMMENT " << cmake_quote(description) << "\n";
        }
        out << ")\n";

        if (!real_output) {
            out << "add_dependencies(" << project.name << " " << target_name << ")\n";
        } else if (config_type == "Utility" || config_type == "ExternalCMake" || config_type == "AndroidApk") {
            // An INTERFACE target would hand its generated sources to every consumer
            out << "add_custom_target(" << target_name << " ALL DEPENDS";
            for (const auto& item : output_items) out << " " << cmake_arg(item);
            out << ")\n";
            out << "add_dependencies(" << project.name << " " << target_name << ")\n";
        } else {
            out << "target_sources(" << project.name << " PRIVATE\n";
            for (const auto& item : output_items) out << "    " << cmake_arg(item) << "\n";
            out << ")\n";
        }
    }
}

// ============================================================================
// NASM support
// ============================================================================

void CMakeGenerator::write_nasm_support(std::ostream& out, const Project& project,
                                         const Solution& /*solution*/, const std::string& project_dir) {
    std::vector<const SourceFile*> nasm_files;
    for (const auto& src : project.sources) {
        if (src.type == FileType::NASM) {
            nasm_files.push_back(&src);
        }
    }
    if (nasm_files.empty()) return;

    // Get NASM settings from first config
    NasmSettings nasm_settings;
    for (const auto& [key, config] : project.configurations) {
        nasm_settings = config.nasm;
        break;
    }

    std::string nasm_exe = nasm_settings.path.empty() ? "nasm" : nasm_settings.path;
    std::string nasm_format = nasm_settings.format;
    if (nasm_format.empty()) {
        nasm_format = "win64"; // Default for CMake (likely MSVC)
    }

    out << "\n# NASM assembly files\n";
    out << "find_program(NASM_EXECUTABLE nasm HINTS \"" << to_cmake_path(nasm_exe) << "\")\n";
    out << "if(NOT NASM_EXECUTABLE)\n";
    out << "    message(WARNING \"NASM not found, assembly files will not be built\")\n";
    out << "endif()\n\n";

    std::vector<std::string> nasm_outputs;
    for (const auto* nasm : nasm_files) {
        std::string rel_path = compute_relative_path(nasm->path, project_dir);
        std::string stem = fs::path(nasm->path).stem().string();
        std::string obj_out = "${CMAKE_CURRENT_BINARY_DIR}/" + stem + ".obj";

        // Build NASM flags
        std::string flags = "-f " + nasm_format;
        for (const auto& inc : nasm_settings.include_directories) {
            flags += " -I" + compute_relative_path(inc, project_dir) + "/";
        }
        for (const auto& def : nasm_settings.preprocessor_definitions) {
            flags += " -D" + def;
        }
        if (!nasm_settings.additional_options.empty()) {
            flags += " " + nasm_settings.additional_options;
        }

        out << "if(NASM_EXECUTABLE)\n";
        out << "    add_custom_command(\n";
        out << "        OUTPUT " << obj_out << "\n";
        out << "        COMMAND ${NASM_EXECUTABLE} " << flags << " -o " << obj_out << " " << rel_path << "\n";
        out << "        DEPENDS " << rel_path << "\n";
        out << "        COMMENT \"Assembling " << fs::path(nasm->path).filename().string() << "\"\n";
        out << "    )\n";
        out << "endif()\n";

        nasm_outputs.push_back(obj_out);
    }

    if (!nasm_outputs.empty()) {
        out << "\ntarget_sources(" << project.name << " PRIVATE\n";
        for (const auto& obj : nasm_outputs) {
            out << "    " << obj << "\n";
        }
        out << ")\n";
    }
}

// ============================================================================
// Per-project CMakeLists.txt generation
// ============================================================================

bool CMakeGenerator::generate_project_cmakelists(const Project& project, const Solution& solution,
                                                  const std::string& project_dir,
                                                  const std::string& /*output_dir*/) {
    fs::path cmake_path = fs::path(project_dir) / "CMakeLists.txt";

    std::ofstream out(cmake_path);
    if (!out) {
        std::cerr << "Error: Failed to create " << cmake_path << "\n";
        return false;
    }
    record_generated_output(cmake_path);

    out << "# Auto-generated CMakeLists.txt for " << project.name << "\n";
    out << "# Generated by sighmake\n\n";

    // Enable MASM if needed
    if (project.has_masm_files) {
        out << "enable_language(ASM_MASM)\n\n";
    }

    // Target definition
    write_target_definition(out, project, solution);

    // Sources
    write_sources(out, project, project_dir);

    // NASM support (adds custom commands and object files to target)
    write_nasm_support(out, project, solution, project_dir);

    // Include directories
    write_include_directories(out, project, solution, project_dir);

    // Compile definitions
    write_compile_definitions(out, project, solution);

    // Compile options
    write_compile_options(out, project, solution);

    // Link libraries and link options
    write_link_libraries(out, project, solution);

    // Link directories
    write_link_directories(out, project, solution, project_dir);

    // Target properties (standard, output name, output dirs)
    write_target_properties(out, project, solution);

    // PCH
    write_pch_settings(out, project, solution, project_dir);

    // Per-file settings
    write_per_file_settings(out, project, solution, project_dir);

    // Build events
    write_build_events(out, project, solution);

    // Custom build rules
    write_custom_build_rules(out, project, solution, project_dir);

    std::cout << "  Generated: " << cmake_path << "\n";
    return true;
}

// ============================================================================
// Root CMakeLists.txt generation
// ============================================================================

bool CMakeGenerator::generate_root_cmakelists(const Solution& solution, const std::string& output_dir) {
    fs::path cmake_path = fs::path(output_dir) / "CMakeLists.txt";

    std::ofstream out(cmake_path);
    if (!out) {
        std::cerr << "Error: Failed to create " << cmake_path << "\n";
        return false;
    }
    record_generated_output(cmake_path);

    out << "# Auto-generated CMakeLists.txt for " << solution.name << "\n";
    out << "# Generated by sighmake\n\n";

    out << "cmake_minimum_required(VERSION 3.20)\n\n";

    // Android targets build through the NDK's CMake toolchain file
    bool targets_android = std::any_of(solution.platforms.begin(), solution.platforms.end(),
                                       [](const std::string& p) { return is_android_platform(p); });
    if (targets_android) {
        out << "# This solution targets Android. Configure with the NDK toolchain file, e.g.:\n";
        out << "#   cmake -B build-android \\\n";
        out << "#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \\\n";
        out << "#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24\n\n";
    }

    // Detect languages
    bool has_c = false;
    bool has_cxx = false;
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;
        std::string lang = detect_project_language(project);
        if (lang == "C") has_c = true;
        else has_cxx = true;
    }

    out << "project(" << solution.name << " LANGUAGES";
    if (has_c) out << " C";
    if (has_cxx) out << " CXX";
    if (!has_c && !has_cxx) out << " CXX"; // Default
    out << ")\n\n";

    // Find packages
    if (!solution.found_packages.empty()) {
        out << "# External packages\n";
        for (const auto& [pkg_name, pkg] : solution.found_packages) {
            out << "find_package(" << pkg_name;
            if (pkg.found) out << " REQUIRED";
            out << ")\n";
        }
        out << "\n";
    }

    // Topological sort of projects
    std::vector<const Project*> build_order;
    std::set<std::string> visited;

    std::function<void(const Project*)> visit = [&](const Project* p) {
        if (visited.count(p->name)) return;
        visited.insert(p->name);
        for (const auto& dep : p->project_references) {
            for (const auto& other : solution.projects) {
                if (other.name == dep.name && !other.is_package_project) {
                    visit(&other);
                    break;
                }
            }
        }
        build_order.push_back(p);
    };

    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;
        visit(&proj);
    }

    // Add subdirectories in dependency order
    out << "# Projects (in dependency order)\n";
    for (const auto* proj : build_order) {
        out << "add_subdirectory(" << proj->name << ")\n";
    }

    std::cout << "Generated root: " << cmake_path << "\n";
    return true;
}

// ============================================================================
// Main generate method
// ============================================================================

bool CMakeGenerator::generate(Solution& solution, const std::string& output_dir) {
    // The CMake generator does not describe Android: declarations scoped to it do not exist here.
    prune_platform_scoped_declarations(solution, [](const std::string& platform) { return !is_android_platform(platform); });
    for (const auto& project : solution.projects) {
        if (detect_project_language(project) == "C#") {
            std::cerr << "Error: C# projects require the vcxproj generator.\n";
            return false;
        }
    }
    // Create output directory if needed
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        try {
            fs::create_directories(output_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create output directory: " << e.what() << "\n";
            return false;
        }
    }

    std::cout << "Generating CMake files for solution: " << solution.name << "\n";

    // Generate per-project CMakeLists.txt files
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;

        // Create project subdirectory
        fs::path project_dir = fs::path(output_dir) / project.name;
        if (!fs::exists(project_dir)) {
            try {
                fs::create_directories(project_dir);
            } catch (const std::exception& e) {
                std::cerr << "Error: Failed to create directory for project " << project.name << ": " << e.what() << "\n";
                return false;
            }
        }

        if (!generate_project_cmakelists(project, solution, project_dir.string(), output_dir)) {
            return false;
        }
    }

    // Generate root CMakeLists.txt
    if (!generate_root_cmakelists(solution, output_dir)) {
        return false;
    }

    std::cout << "CMake generation complete!\n";

    // Write build cache
    {
        BuildCache cache;
        cache.generator = "cmake";
        cache.solution_name = solution.name;
        cache.configurations = solution.configurations;
        cache.platforms = solution.platforms;
        cache.build_dir = "build";
        cache.write(output_dir);
    }

    return true;
}

std::optional<OutputManifestScope> CMakeGenerator::output_manifest_scope(const std::string& output_dir) const {
    OutputManifestScope scope;
    scope.directory = fs::path(output_dir.empty() ? "." : output_dir);
    scope.key = "cmake " + target_os_manifest_name();
    // Project CMakeLists.txt files live one directory below the root one.
    scope.probable_search_depth = 1;
    scope.probable_output = [](const fs::path& absolute, const std::string& relative) {
        const size_t slash = relative.find('/');
        return slash != std::string::npos && relative.substr(slash + 1) == "CMakeLists.txt" &&
            has_sighmake_header(absolute);
    };
    return scope;
}

// Register this generator with the factory
REGISTER_GENERATOR(CMakeGenerator, "cmake");

} // namespace vcxproj
