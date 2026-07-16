#include "pch.h"
#include "cmake_generator.hpp"
#include "common/build_cache.hpp"
#include "common/string_utils.hpp"
#include "common/compiler_flags.hpp"
#include "common/language_standards.hpp"

namespace vcxproj {

namespace fs = std::filesystem;

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

    if (config_type == "StaticLibrary") {
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
        out << "    " << s << "\n";
    }
    for (const auto& s : header_sources) {
        out << "    " << s << "\n";
    }
    for (const auto& s : resource_sources) {
        out << "    " << s << "\n";
    }
    for (const auto& s : masm_sources) {
        out << "    " << s << "\n";
    }
    for (const auto& s : def_sources) {
        out << "    " << s << "\n";
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

    if (common_includes.empty() && public_incs.empty()) return;

    out << "\ntarget_include_directories(" << project.name << "\n";

    if (!public_incs.empty()) {
        out << "    PUBLIC\n";
        for (const auto& inc : public_incs) {
            out << "        " << inc << "\n";
        }
    }

    if (!common_includes.empty()) {
        out << "    " << visibility << "\n";
        for (const auto& inc : common_includes) {
            out << "        " << compute_relative_path(inc, project_dir) << "\n";
        }
    }

    out << ")\n";

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
                    extra.push_back(compute_relative_path(normalized, project_dir));
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
                out << "    $<$<CONFIG:" << cfg_name << ">:" << e << ">\n";
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
    for (const auto& [key, config] : project.configurations) {
        std::set<std::string> cfg_defines(config.cl_compile.preprocessor_definitions.begin(),
                                           config.cl_compile.preprocessor_definitions.end());
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
        for (const auto& def : config->cl_compile.preprocessor_definitions) {
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
            out << "        " << d << "\n";
        }
    }

    if (!common_defines.empty() || !per_config.empty()) {
        out << "    " << visibility << "\n";
        for (const auto& d : common_defines) {
            out << "        " << d << "\n";
        }
        for (const auto& pc : per_config) {
            for (const auto& d : pc.defines) {
                out << "        $<$<CONFIG:" << pc.name << ">:" << d << ">\n";
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
    if (first_config->cl_compile.utf8_source) {
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
            out << "    $<$<CXX_COMPILER_ID:MSVC>:" << opt << ">\n";
        }
    }

    // Shared GCC/Clang options
    if (!shared_gcc.empty()) {
        for (const auto& opt : shared_gcc) {
            out << "    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:" << opt << ">\n";
        }
    }

    // Per-config options
    for (const auto& pc : per_config) {
        for (const auto& opt : pc.msvc_opts) {
            out << "    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:" << pc.name << ">>:" << opt << ">\n";
        }
        for (const auto& opt : pc.gcc_opts) {
            out << "    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:" << pc.name << ">>:" << opt << ">\n";
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
                out << "        " << to_cmake_path(lib) << "\n";
            }
            for (const auto& pc : per_config) {
                for (const auto& lib : pc.libs) {
                    out << "        $<$<CONFIG:" << pc.name << ">:" << to_cmake_path(lib) << ">\n";
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

            // Link-time code generation (per-config)
            for (const auto& cfg_name : config_names) {
                const Configuration* cfg = find_config(project, cfg_name);
                if (cfg && (cfg->link.optimize_references.value_or(false) || cfg->link.enable_comdat_folding.value_or(false))) {
                    // GCC equivalent per-config
                    // We'll handle this outside the opts collection
                }
            }
        }
    }

    if (!msvc_link_opts.empty() || !gcc_link_opts.empty()) {
        out << "\ntarget_link_options(" << project.name << " " << default_vis << "\n";
        for (const auto& opt : msvc_link_opts) {
            out << "    $<$<CXX_COMPILER_ID:MSVC>:" << opt << ">\n";
        }
        for (const auto& opt : gcc_link_opts) {
            out << "    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:" << opt << ">\n";
        }

        // Per-config gc-sections for release-type configs
        for (const auto& cfg_name : config_names) {
            const Configuration* cfg = find_config(project, cfg_name);
            if (cfg && (cfg->link.optimize_references.value_or(false) || cfg->link.enable_comdat_folding.value_or(false))) {
                out << "    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:" << cfg_name << ">>:-Wl,--gc-sections>\n";
                out << "    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:" << cfg_name << ">>:/OPT:REF>\n";
                out << "    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:" << cfg_name << ">>:/OPT:ICF>\n";
            }
        }

        out << ")\n";
    }
}

// ============================================================================
// Link directories
// ============================================================================

void CMakeGenerator::write_link_directories(std::ostream& out, const Project& project,
                                             const Solution& /*solution*/, const std::string& project_dir) {
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

    if (common_libdirs.empty() && public_dirs.empty()) return;

    out << "\ntarget_link_directories(" << project.name << "\n";

    if (!public_dirs.empty()) {
        out << "    PUBLIC\n";
        for (const auto& d : public_dirs) {
            out << "        " << d << "\n";
        }
    }

    if (!common_libdirs.empty()) {
        out << "    PRIVATE\n";
        for (const auto& d : common_libdirs) {
            out << "        " << compute_relative_path(d, project_dir) << "\n";
        }
    }

    out << ")\n";
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

    // Position independent code for shared libs
    if (config_type == "DynamicLibrary") {
        props.push_back("POSITION_INDEPENDENT_CODE ON");
    }

    if (props.empty()) return;

    out << "\nset_target_properties(" << project.name << " PROPERTIES\n";
    for (const auto& p : props) {
        out << "    " << p << "\n";
    }
    out << ")\n";

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

void CMakeGenerator::write_pch_settings(std::ostream& out, const Project& project, const Solution& /*solution*/) {
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

    out << "\ntarget_precompile_headers(" << project.name << " PRIVATE " << pch_header << ")\n";

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
            std::string filename = fs::path(src.path).filename().string();
            out << "set_source_files_properties(" << filename
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
            for (const auto& d : defs) {
                file_defines.push_back(d);
            }
        }

        // Per-file excluded
        bool excluded_all = false;
        for (const auto& [cfg_key, excluded] : src.settings.excluded) {
            if (excluded && cfg_key == "ALL_CONFIGS") {
                excluded_all = true;
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

        out << "set_source_files_properties(" << rel_path << " PROPERTIES";

        if (!file_defines.empty()) {
            out << "\n    COMPILE_DEFINITIONS \"";
            for (size_t i = 0; i < file_defines.size(); i++) {
                if (i > 0) out << ";";
                out << file_defines[i];
            }
            out << "\"";
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

void CMakeGenerator::write_build_events(std::ostream& out, const Project& project, const Solution& /*solution*/) {
    // Use the first config's build events (they're typically the same across configs)
    const Configuration* config = nullptr;
    for (const auto& [key, cfg] : project.configurations) {
        config = &cfg;
        break;
    }
    if (!config) return;

    // Pre-build event
    if (!config->pre_build_event.command.empty()) {
        std::string cmd = unescape_newlines(config->pre_build_event.command);
        out << "\nadd_custom_command(TARGET " << project.name << " PRE_BUILD\n";
        out << "    COMMAND " << to_cmake_path(cmd) << "\n";
        if (!config->pre_build_event.message.empty()) {
            out << "    COMMENT \"" << config->pre_build_event.message << "\"\n";
        }
        out << ")\n";
    }

    // Pre-link event
    if (!config->pre_link_event.command.empty()) {
        std::string cmd = unescape_newlines(config->pre_link_event.command);
        out << "\nadd_custom_command(TARGET " << project.name << " PRE_LINK\n";
        out << "    COMMAND " << to_cmake_path(cmd) << "\n";
        if (!config->pre_link_event.message.empty()) {
            out << "    COMMENT \"" << config->pre_link_event.message << "\"\n";
        }
        out << ")\n";
    }

    // Post-build event
    if (!config->post_build_event.command.empty()) {
        std::string cmd = unescape_newlines(config->post_build_event.command);
        out << "\nadd_custom_command(TARGET " << project.name << " POST_BUILD\n";
        out << "    COMMAND " << to_cmake_path(cmd) << "\n";
        if (!config->post_build_event.message.empty()) {
            out << "    COMMENT \"" << config->post_build_event.message << "\"\n";
        }
        out << ")\n";
    }
}

// ============================================================================
// Custom build rules
// ============================================================================

void CMakeGenerator::write_custom_build_rules(std::ostream& out, const Project& project,
                                               const std::string& project_dir) {
    for (const auto& src : project.sources) {
        if (src.type != FileType::CustomBuild) continue;

        std::string rel_path = compute_relative_path(src.path, project_dir);

        // Use the first config's custom build settings
        std::string command, outputs, description;
        for (const auto& [cfg_key, cmd] : src.custom_command) {
            command = unescape_newlines(cmd);
            break;
        }
        for (const auto& [cfg_key, out_val] : src.custom_outputs) {
            outputs = out_val;
            break;
        }
        for (const auto& [cfg_key, msg] : src.custom_message) {
            description = msg;
            break;
        }

        if (command.empty()) continue;

        out << "\nadd_custom_command(\n";
        if (!outputs.empty()) {
            out << "    OUTPUT " << to_cmake_path(outputs) << "\n";
        }
        out << "    COMMAND " << to_cmake_path(command) << "\n";
        out << "    DEPENDS " << rel_path << "\n";
        if (!description.empty()) {
            out << "    COMMENT \"" << description << "\"\n";
        }
        out << ")\n";
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
    write_pch_settings(out, project, solution);

    // Per-file settings
    write_per_file_settings(out, project, solution, project_dir);

    // Build events
    write_build_events(out, project, solution);

    // Custom build rules
    write_custom_build_rules(out, project, project_dir);

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

// Register this generator with the factory
REGISTER_GENERATOR(CMakeGenerator, "cmake");

} // namespace vcxproj
