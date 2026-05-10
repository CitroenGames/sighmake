#include "pch.h"
#include "config.hpp"
#include "vcxproj_generator.hpp"
#include "common/vs_detector.hpp"
#include "common/toolset_registry.hpp"
#include "common/build_cache.hpp"
#define PUGIXML_HEADER_ONLY
#include "pugixml.hpp"

#if PROJ_SEPERATOR
#define GENERATED_VCXPROJ "_.vcxproj"
#define GENERATED_SLNX "_.slnx"
#define GENERATED_SLN "_.sln"
#else
#define GENERATED_VCXPROJ ".vcxproj"
#define GENERATED_SLNX ".slnx"
#define GENERATED_SLN ".sln"
#endif

namespace fs = std::filesystem;

namespace vcxproj {

// Helper to unescape escaped newlines in strings
static std::string unescape_newlines(const std::string& str) {
    std::string result;
    result.reserve(str.length());

    for (size_t i = 0; i < str.length(); ++i) {
        if (i + 1 < str.length() && str[i] == '\x01') {
            if (str[i + 1] == 'n') {
                result += '\n';
                ++i;
            } else if (str[i + 1] == '\\') {
                result += '\\';
                ++i;
            } else {
                result += str[i];
            }
        } else {
            result += str[i];
        }
    }

    return result;
}

// Map buildscript warning_level values to MSBuild WarningLevel enum values
static std::string map_warning_level(const std::string& level) {
    if (level == "Level0") return "TurnOffAllWarnings";
    return level; // Level1-Level4 are already valid MSBuild values
}

// Adjust relative file paths in a custom build command
static std::string adjust_command_paths(const std::string& command,
                                       const std::string&,
                                       const std::string&) {
    // Return command unchanged to preserve exact formatting, paths, and command structure
    return command;
}

// Helper function to make a path relative to the output directory
static std::string make_relative_path(const std::string& file_path, const std::string& base_path) {
    if (file_path.find("$(") != std::string::npos || file_path.find("%(") != std::string::npos) {
        return file_path;
    }

    try {
        // Check if original path has trailing slash/backslash
        bool has_trailing_slash = !file_path.empty() &&
            (file_path.back() == '/' || file_path.back() == '\\');

        fs::path file(file_path);
        fs::path base(base_path);

        // Make both paths absolute
        if (!file.is_absolute()) {
            file = fs::absolute(file);
        }
        if (!base.is_absolute()) {
            base = fs::absolute(base);
        }

        // Get the directory containing the base file
        base = base.parent_path();

        // Check for different drives (Windows-specific)
        // If paths are on different drives, we cannot create a relative path
        // Return the absolute path instead
        if (file.root_name() != base.root_name()) {
            std::string result = file.string();
            // Preserve trailing slash if original had one
            if (has_trailing_slash && !result.empty() && result.back() != '\\') {
                result += '\\';
            }
            return result;
        }

        // Calculate relative path without touching the filesystem. Some output
        // directories do not exist yet, and fs::relative() can fail on them.
        fs::path relative = file.lexically_relative(base);
        if (relative.empty() && file != base) {
            relative = file;
        }

        // Convert to string with backslashes for Windows
        std::string result = relative.string();
        std::replace(result.begin(), result.end(), '/', '\\');

        // Preserve trailing slash if original had one
        if (has_trailing_slash && !result.empty() && result.back() != '\\') {
            result += '\\';
        }

        return result;
    } catch (...) {
        // If anything fails, just return the original path
        return file_path;
    }
}

static bool project_matches_dependency_name(const Project& project, const std::string& dependency_name) {
    std::string key = to_lower(dependency_name);
    if (key.empty()) return false;

    if (to_lower(project.name) == key || to_lower(project.project_name) == key) {
        return true;
    }

    for (const auto& [_, cfg] : project.configurations) {
        if (!cfg.target_name.empty() && to_lower(cfg.target_name) == key) {
            return true;
        }
    }

    return false;
}

static const Project* find_dependency_project(const Solution& solution, const std::string& dependency_name) {
    for (const auto& project : solution.projects) {
        if (project_matches_dependency_name(project, dependency_name)) {
            return &project;
        }
    }
    return nullptr;
}

static std::string normalize_filter_path(std::string filter) {
    std::replace(filter.begin(), filter.end(), '\\', '/');
    while (!filter.empty() && filter.front() == '/') {
        filter.erase(filter.begin());
    }
    while (!filter.empty() && filter.back() == '/') {
        filter.pop_back();
    }
    return filter;
}

static std::string to_msvc_filter_path(std::string filter) {
    filter = normalize_filter_path(filter);
    std::replace(filter.begin(), filter.end(), '/', '\\');
    return filter;
}

static uint64_t fnv1a64(const std::string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

static std::string make_stable_filter_guid(const std::string& seed) {
    uint64_t high = fnv1a64(seed);
    uint64_t low = fnv1a64(seed + "#filter");
    std::stringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0');
    ss << std::setw(8) << static_cast<uint32_t>(high >> 32) << "-";
    ss << std::setw(4) << static_cast<uint16_t>(high >> 16) << "-";
    ss << std::setw(4) << static_cast<uint16_t>(high) << "-";
    ss << std::setw(4) << static_cast<uint16_t>(low >> 48) << "-";
    ss << std::setw(12) << (low & 0x0000FFFFFFFFFFFFull);
    return ss.str();
}

static void collect_filter_and_parents(const std::string& filter, std::set<std::string>& filters) {
    std::string path = normalize_filter_path(filter);
    while (!path.empty()) {
        filters.insert(path);
        size_t slash = path.rfind('/');
        if (slash == std::string::npos) {
            break;
        }
        path = path.substr(0, slash);
    }
}

std::string VcxprojGenerator::escape_xml(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string VcxprojGenerator::join_vector(const std::vector<std::string>& vec,
                                           const std::string& separator) {
    if (vec.empty()) return "";
    std::string result = vec[0];
    for (size_t i = 1; i < vec.size(); i++) {
        result += separator + vec[i];
    }
    return result;
}

std::string VcxprojGenerator::map_c_standard(const std::string& std) {
    // Valid MSVC C standards: stdc89, stdc11 only
    if (std == "89" || std == "90") {
        return "stdc89";
    } else if (std == "11") {
        return "stdc11";
    }

    // Handle unsupported standards with warnings
    if (std == "99") {
        std::cerr << "Warning: C99 (stdc99) is not fully supported by MSVC. "
                  << "Falling back to stdc11.\n";
        return "stdc11";
    } else if (std == "17" || std == "23") {
        std::cerr << "Warning: C" << std << " is not supported by MSVC. "
                  << "Falling back to stdc11.\n";
        return "stdc11";
    }

    return "";  // Empty = compiler default
}

std::string VcxprojGenerator::map_cpp_standard(const std::string& std) {
    // Handle already-prefixed standards (e.g., "stdcpp17")
    if (std.find("stdcpp") == 0) {
        std::string num = std.substr(6);  // Extract number after "stdcpp"
        return map_cpp_standard(num);  // Recursive call to validate
    }

    // Map numeric values to valid MSVC C++ standards
    if (std == "14") {
        return "stdcpp14";
    } else if (std == "17") {
        return "stdcpp17";
    } else if (std == "20") {
        return "stdcpp20";
    } else if (std == "23") {
        return "stdcpp23";
    } else if (std == "latest") {
        return "stdcpplatest";
    }

    // Handle invalid values - warn and fallback
    if (std == "11" || std == "03" || std == "98") {
        std::cerr << "Warning: C++" << std << " is not supported by VS 2022+ (minimum is C++14). "
                  << "Falling back to stdcpp14.\n";
        return "stdcpp14";
    }

    // Unknown/future standards - default to C++17
    if (!std.empty()) {
        std::cerr << "Warning: Unknown C++ standard '" << std << "'. Falling back to stdcpp17.\n";
    }
    return "stdcpp17";  // Safe default
}

std::string VcxprojGenerator::get_file_type_name(FileType type) {
    switch (type) {
        case FileType::ClCompile: return "ClCompile";
        case FileType::ClInclude: return "ClInclude";
        case FileType::CustomBuild: return "CustomBuild";
        case FileType::MASM: return "MASM";
        case FileType::MessageCompile: return "MessageCompile";
        case FileType::Midl: return "Midl";
        case FileType::NASM: return "CustomBuild";   // NASM uses custom build rules
        case FileType::ObjCxx: return "ClCompile";  // Emit as ClCompile for cross-platform project listing
        case FileType::ResourceCompile: return "ResourceCompile";
        default: return "None";
    }
}

bool VcxprojGenerator::generate_vcxproj(const Project& project, const Solution& solution,
                                         const std::string& output_path) {
    pugi::xml_document doc;

    // XML declaration
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    // Root Project element
    auto root = doc.append_child("Project");
    root.append_attribute("DefaultTargets") = "Build";

    // Determine ToolsVersion based on project's toolsets
    std::string tools_version = "4.0"; // Default legacy
    for (const auto& [config_key, cfg] : project.configurations) {
        std::string ts_version = get_tools_version(cfg.platform_toolset);
#ifndef NDEBUG
        std::cout << "[DEBUG] .vcxproj generation for '" << project.name
                  << "': Config '" << config_key
                  << "' has toolset '" << cfg.platform_toolset
                  << "' -> ToolsVersion '" << ts_version << "'\n";
#endif
        if (ts_version == "18.0") {
            tools_version = "18.0";
            break; // MSVC 2026 takes precedence
        }
    }

#ifndef NDEBUG
    std::cout << "[DEBUG] Final ToolsVersion for " << project.name << ": " << tools_version << "\n";
#endif

    root.append_attribute("ToolsVersion") = tools_version.c_str();
    root.append_attribute("xmlns") = "http://schemas.microsoft.com/developer/msbuild/2003";

    // Add VCProjectUpgraderObjectName for MSVC 2026 to prevent auto-upgrade prompts
    if (tools_version == "18.0") {
#ifndef NDEBUG
        std::cout << "[DEBUG] Adding VCProjectUpgraderObjectName=NoUpgrade for MSVC 2026\n";
#endif
        root.append_attribute("VCProjectUpgraderObjectName") = "NoUpgrade";
    }

    // ProjectConfigurations
    auto configs_group = root.append_child("ItemGroup");
    configs_group.append_attribute("Label") = "ProjectConfigurations";
    for (const auto& config_key : solution.get_config_keys()) {
        auto [config, platform] = parse_config_key(config_key);
        if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
        auto proj_config = configs_group.append_child("ProjectConfiguration");
        proj_config.append_attribute("Include") = config_key.c_str();
        proj_config.append_child("Configuration").text() = config.c_str();
        proj_config.append_child("Platform").text() = platform.c_str();
    }

    // Globals
    auto globals = root.append_child("PropertyGroup");
    globals.append_attribute("Label") = "Globals";
    // Use project_name if available, otherwise use name
    std::string display_name = !project.project_name.empty() ? project.project_name : project.name;
    globals.append_child("ProjectName").text() = display_name.c_str();
    globals.append_child("ProjectGuid").text() = ("{" + project.uuid + "}").c_str();
    if (!project.root_namespace.empty()) {
        globals.append_child("RootNamespace").text() = project.root_namespace.c_str();
    }
    if (project.ignore_warn_compile_duplicated_filename) {
        globals.append_child("IgnoreWarnCompileDuplicatedFilename").text() = "true";
    }
    // Add WindowsTargetPlatformVersion to Globals if it exists in any configuration
    if (!project.configurations.empty()) {
        auto& first_cfg = project.configurations.begin()->second;
        if (!first_cfg.windows_target_platform_version.empty()) {
            globals.append_child("WindowsTargetPlatformVersion").text() =
                first_cfg.windows_target_platform_version.c_str();
        }
    }

    // Import default props
    auto import1 = root.append_child("Import");
    import1.append_attribute("Project") = "$(VCTargetsPath)\\Microsoft.Cpp.Default.props";

    // Configuration properties
    for (const auto& [config_key, cfg] : project.configurations) {
        auto [config, platform] = parse_config_key(config_key);
        if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";

        auto cfg_props = root.append_child("PropertyGroup");
        cfg_props.append_attribute("Condition") = condition.c_str();
        cfg_props.append_attribute("Label") = "Configuration";

        if (!cfg.config_type.empty())
            cfg_props.append_child("ConfigurationType").text() = cfg.config_type.c_str();
        if (!cfg.character_set.empty())
            cfg_props.append_child("CharacterSet").text() = cfg.character_set.c_str();
        if (!cfg.target_name.empty())
            cfg_props.append_child("TargetName").text() = cfg.target_name.c_str();
        if (!cfg.platform_toolset.empty())
            cfg_props.append_child("PlatformToolset").text() = cfg.platform_toolset.c_str();
        if (cfg.use_debug_libraries)
            cfg_props.append_child("UseDebugLibraries").text() = "true";
        if (cfg.whole_program_optimization)
            cfg_props.append_child("WholeProgramOptimization").text() = "true";
    }

    // Import Cpp props
    auto import2 = root.append_child("Import");
    import2.append_attribute("Project") = "$(VCTargetsPath)\\Microsoft.Cpp.props";

    // Extension settings - conditionally import MASM props if project has MASM files
    auto ext_settings = root.append_child("ImportGroup");
    ext_settings.append_attribute("Label") = "ExtensionSettings";
    if (project.has_masm_files) {
        auto masm_import = ext_settings.append_child("Import");
        masm_import.append_attribute("Project") = "$(VCTargetsPath)\\BuildCustomizations\\masm.props";
    }

    // Property sheets
    for (const auto& config_key : solution.get_config_keys()) {
        auto [config, platform] = parse_config_key(config_key);
        if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
        auto sheets = root.append_child("ImportGroup");
        sheets.append_attribute("Condition") = condition.c_str();
        sheets.append_attribute("Label") = "PropertySheets";

        auto import = sheets.append_child("Import");
        import.append_attribute("Project") = "$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props";
        import.append_attribute("Condition") = "exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')";
        import.append_attribute("Label") = "LocalAppDataPlatform";
    }

    // User macros
    root.append_child("PropertyGroup").append_attribute("Label") = "UserMacros";

    // Output directories and other properties
    auto props = root.append_child("PropertyGroup");
    props.append_child("_ProjectFileVersion").text() = "10.0.30319.1";
    for (const auto& [config_key, cfg] : project.configurations) {
        auto [config_name, platform_name] = parse_config_key(config_key);
        if (is_unix_platform(platform_name)) continue;  // Skip Unix configs for vcxproj
        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";

        // Relative outdir/intdir in a buildscript are interpreted relative to
        // the buildscript file itself. Since the .vcxproj now lives in a build
        // subdirectory (not next to the buildscript), we must first anchor a
        // relative path against project.buildscript_path, then relativize from
        // the vcxproj location. Absolute paths and MSBuild $() variables are
        // passed through unchanged (the latter) or only relativized (the former).
        auto resolve_dir = [&](const std::string& raw) -> std::string {
            if (raw.find("$(") != std::string::npos) return raw;
            fs::path p(raw);
            if (!p.is_absolute() && !project.buildscript_path.empty()) {
                p = fs::path(project.buildscript_path) / p;
            }
            return make_relative_path(p.string(), output_path);
        };

        {
            auto node = props.append_child("OutDir");
            node.append_attribute("Condition") = condition.c_str();
            std::string out_value;
            if (cfg.out_dir.empty()) {
                out_value = default_vcxproj_out_dir(platform_name, config_name);
            } else {
                out_value = resolve_dir(cfg.out_dir);
            }
            // Ensure trailing slash for MSBuild
            if (!out_value.empty() && out_value.back() != '\\') {
                out_value += '\\';
            }
            node.text() = out_value.c_str();
        }
        {
            auto node = props.append_child("IntDir");
            node.append_attribute("Condition") = condition.c_str();
            std::string int_value;
            if (cfg.int_dir.empty()) {
                int_value = default_vcxproj_int_dir(platform_name, config_name, project.name);
            } else {
                int_value = resolve_dir(cfg.int_dir);
            }
            // Ensure trailing slash for MSBuild
            if (!int_value.empty() && int_value.back() != '\\') {
                int_value += '\\';
            }
            node.text() = int_value.c_str();
        }
        // Note: TargetName is written in the Configuration PropertyGroup, not here
        if (!cfg.target_ext.empty()) {
            auto node = props.append_child("TargetExt");
            node.append_attribute("Condition") = condition.c_str();
            node.text() = cfg.target_ext.c_str();
        }

        // For DLL projects, set the import library location
        if (cfg.config_type == "DynamicLibrary") {
            auto node = props.append_child("ImportLibrary");
            node.append_attribute("Condition") = condition.c_str();
            std::string target_name = cfg.target_name.empty() ? project.name : cfg.target_name;
            node.text() = ("$(OutDir)" + target_name + ".lib").c_str();
        }

        auto node = props.append_child("LinkIncremental");
        node.append_attribute("Condition") = condition.c_str();
        node.text() = cfg.link_incremental ? "true" : "false";

        // ExecutablePath
        if (!cfg.executable_path.empty()) {
            auto exec_path = props.append_child("ExecutablePath");
            exec_path.append_attribute("Condition") = condition.c_str();
            exec_path.text() = cfg.executable_path.c_str();
        }

        // GenerateManifest
        if (!cfg.generate_manifest) {
            auto gen_manifest = props.append_child("GenerateManifest");
            gen_manifest.append_attribute("Condition") = condition.c_str();
            gen_manifest.text() = "false";
        }

        // IgnoreImportLibrary
        if (cfg.ignore_import_library) {
            auto ignore_lib = props.append_child("IgnoreImportLibrary");
            ignore_lib.append_attribute("Condition") = condition.c_str();
            ignore_lib.text() = "true";
        }

        // ImportLibrary
        if (!cfg.import_library.empty()) {
            auto import_lib = props.append_child("ImportLibrary");
            import_lib.append_attribute("Condition") = condition.c_str();
            import_lib.text() = cfg.import_library.c_str();
        }

        // Build event use in build flags - always write them
        if (!cfg.pre_build_event.command.empty()) {
            auto pre_use = props.append_child("PreBuildEventUseInBuild");
            pre_use.append_attribute("Condition") = condition.c_str();
            pre_use.text() = cfg.pre_build_event.use_in_build ? "true" : "false";
        }
        if (!cfg.pre_link_event.command.empty()) {
            auto pre_link_use = props.append_child("PreLinkEventUseInBuild");
            pre_link_use.append_attribute("Condition") = condition.c_str();
            pre_link_use.text() = cfg.pre_link_event.use_in_build ? "true" : "false";
        }
        if (!cfg.post_build_event.command.empty()) {
            auto post_use = props.append_child("PostBuildEventUseInBuild");
            post_use.append_attribute("Condition") = condition.c_str();
            post_use.text() = cfg.post_build_event.use_in_build ? "true" : "false";
        }
    }

    // ItemDefinitionGroup for each configuration
    for (const auto& [config_key, cfg] : project.configurations) {
        auto [config_name, platform_name] = parse_config_key(config_key);
        if (is_unix_platform(platform_name)) continue;  // Skip Unix configs for vcxproj
        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
        auto item_def = root.append_child("ItemDefinitionGroup");
        item_def.append_attribute("Condition") = condition.c_str();

        // ClCompile settings
        auto cl = item_def.append_child("ClCompile");
        if (!cfg.cl_compile.optimization.empty())
            cl.append_child("Optimization").text() = cfg.cl_compile.optimization.c_str();
        if (!cfg.cl_compile.additional_include_directories.empty()) {
            // Make include directories relative to the output path
            std::vector<std::string> relative_includes;
            for (const auto& inc : cfg.cl_compile.additional_include_directories) {
                relative_includes.push_back(make_relative_path(inc, output_path));
            }
            cl.append_child("AdditionalIncludeDirectories").text() =
                join_vector(relative_includes, ";").c_str();
        }
        std::vector<std::string> current_defines = cfg.cl_compile.preprocessor_definitions;

        // Automatically add _DEBUG for Debug configurations and NDEBUG for Release configurations
        if (config_name == "Debug") {
            if (std::find(current_defines.begin(), current_defines.end(), "_DEBUG") == current_defines.end()) {
                current_defines.push_back("_DEBUG");
            }
        } else if (config_name == "Release") {
            if (std::find(current_defines.begin(), current_defines.end(), "NDEBUG") == current_defines.end()) {
                current_defines.push_back("NDEBUG");
            }
        }

        if (!current_defines.empty()) {
            cl.append_child("PreprocessorDefinitions").text() =
                join_vector(current_defines, ";").c_str();
        }
        if (!cfg.cl_compile.forced_include_files.empty())
            cl.append_child("ForcedIncludeFiles").text() =
                join_vector(cfg.cl_compile.forced_include_files, ";").c_str();
        if (!cfg.cl_compile.runtime_library.empty())
            cl.append_child("RuntimeLibrary").text() = cfg.cl_compile.runtime_library.c_str();
        if (!cfg.cl_compile.debug_information_format.empty())
            cl.append_child("DebugInformationFormat").text() = cfg.cl_compile.debug_information_format.c_str();
        if (!cfg.cl_compile.warning_level.empty())
            cl.append_child("WarningLevel").text() = map_warning_level(cfg.cl_compile.warning_level).c_str();
        if (!cfg.cl_compile.disable_specific_warnings.empty())
            cl.append_child("DisableSpecificWarnings").text() =
                join_vector(cfg.cl_compile.disable_specific_warnings, ";").c_str();
        if (!cfg.cl_compile.language_standard.empty()) {
            // Validate and map C++ standard
            std::string validated_std = map_cpp_standard(cfg.cl_compile.language_standard);
            cl.append_child("LanguageStandard").text() = validated_std.c_str();
        }
        // C standard (emitted whenever c_standard is set; MSBuild applies it only to C compilations)
        if (!project.c_standard.empty()) {
            std::string c_std_mapped = map_c_standard(project.c_standard);
            if (!c_std_mapped.empty()) {
                cl.append_child("LanguageStandard_C").text() = c_std_mapped.c_str();
            }
        }
        if (!cfg.cl_compile.exception_handling.empty())
            cl.append_child("ExceptionHandling").text() = cfg.cl_compile.exception_handling.c_str();
        if (!cfg.cl_compile.enhanced_instruction_set.empty())
            cl.append_child("EnableEnhancedInstructionSet").text() = cfg.cl_compile.enhanced_instruction_set.c_str();
        if (!cfg.cl_compile.floating_point_model.empty())
            cl.append_child("FloatingPointModel").text() = cfg.cl_compile.floating_point_model.c_str();

        // Build AdditionalOptions with UTF-8 flag if needed
        std::string additional_opts = cfg.cl_compile.additional_options;
        if (cfg.cl_compile.utf8_source) {
            if (!additional_opts.empty()) {
                additional_opts += " /utf-8";
            } else {
                additional_opts = "/utf-8";
            }
        }
        if (!additional_opts.empty())
            cl.append_child("AdditionalOptions").text() = additional_opts.c_str();

        if (cfg.cl_compile.function_level_linking.value_or(false))
            cl.append_child("FunctionLevelLinking").text() = "true";
        if (cfg.cl_compile.intrinsic_functions.value_or(false))
            cl.append_child("IntrinsicFunctions").text() = "true";
        // Always write RuntimeTypeInfo explicitly
        if (cfg.cl_compile.runtime_type_info)
            cl.append_child("RuntimeTypeInfo").text() = "true";
        if (cfg.cl_compile.multi_processor_compilation)
            cl.append_child("MultiProcessorCompilation").text() = "true";

        // New compiler settings
        if (!cfg.cl_compile.inline_function_expansion.empty())
            cl.append_child("InlineFunctionExpansion").text() = cfg.cl_compile.inline_function_expansion.c_str();
        if (!cfg.cl_compile.favor_size_or_speed.empty())
            cl.append_child("FavorSizeOrSpeed").text() = cfg.cl_compile.favor_size_or_speed.c_str();
        if (cfg.cl_compile.string_pooling)
            cl.append_child("StringPooling").text() = "true";
        // Always write MinimalRebuild explicitly
        cl.append_child("MinimalRebuild").text() = cfg.cl_compile.minimal_rebuild ? "true" : "false";
        if (!cfg.cl_compile.basic_runtime_checks.empty())
            cl.append_child("BasicRuntimeChecks").text() = cfg.cl_compile.basic_runtime_checks.c_str();
        if (!cfg.cl_compile.buffer_security_check)
            cl.append_child("BufferSecurityCheck").text() = "false";
        // Always write ForceConformanceInForLoopScope explicitly
        if (cfg.cl_compile.force_conformance_in_for_loop_scope)
            cl.append_child("ForceConformanceInForLoopScope").text() = "true";
        if (!cfg.cl_compile.assembler_listing_location.empty())
            cl.append_child("AssemblerListingLocation").text() = cfg.cl_compile.assembler_listing_location.c_str();
        if (!cfg.cl_compile.object_file_name.empty())
            cl.append_child("ObjectFileName").text() = cfg.cl_compile.object_file_name.c_str();
        if (!cfg.cl_compile.program_database_file_name.empty())
            cl.append_child("ProgramDataBaseFileName").text() = cfg.cl_compile.program_database_file_name.c_str();
        // Always write GenerateXMLDocumentationFiles explicitly
        if (!cfg.cl_compile.generate_xml_documentation_files)
            cl.append_child("GenerateXMLDocumentationFiles").text() = "false";
        // Always write BrowseInformation explicitly
        if (!cfg.cl_compile.browse_information)
            cl.append_child("BrowseInformation").text() = "false";
        if (!cfg.cl_compile.browse_information_file.empty())
            cl.append_child("BrowseInformationFile").text() = cfg.cl_compile.browse_information_file.c_str();
        if (!cfg.cl_compile.compile_as.empty())
            cl.append_child("CompileAs").text() = cfg.cl_compile.compile_as.c_str();
        if (!cfg.cl_compile.error_reporting.empty())
            cl.append_child("ErrorReporting").text() = cfg.cl_compile.error_reporting.c_str();
        if (!cfg.cl_compile.treat_wchar_t_as_built_in_type)
            cl.append_child("TreatWChar_tAsBuiltInType").text() = "false";
        if (!cfg.cl_compile.assembler_output.empty())
            cl.append_child("AssemblerOutput").text() = cfg.cl_compile.assembler_output.c_str();
        if (cfg.cl_compile.expand_attributed_source)
            cl.append_child("ExpandAttributedSource").text() = "true";
        if (cfg.cl_compile.openmp_support)
            cl.append_child("OpenMPSupport").text() = "true";
        if (cfg.cl_compile.treat_warning_as_error)
            cl.append_child("TreatWarningAsError").text() = "true";

        // PCH - always write, defaulting to NotUsing
        std::string pch_mode = cfg.cl_compile.pch.mode.empty() ? "NotUsing" : cfg.cl_compile.pch.mode;
        cl.append_child("PrecompiledHeader").text() = pch_mode.c_str();
        // Always write PrecompiledHeaderFile if specified, even if mode is "NotUsing"
        // This is needed for files that have Create mode - they inherit this header
        // Extract filename only to match makefile_generator behavior
        if (!cfg.cl_compile.pch.header.empty()) {
            std::string pch_filename = fs::path(cfg.cl_compile.pch.header).filename().string();
            cl.append_child("PrecompiledHeaderFile").text() = pch_filename.c_str();
        }
        // Only write PrecompiledHeaderOutputFile if mode is not "NotUsing"
        if (pch_mode != "NotUsing" && !cfg.cl_compile.pch.output.empty())
            cl.append_child("PrecompiledHeaderOutputFile").text() = cfg.cl_compile.pch.output.c_str();

        // Link settings
        if (cfg.config_type == "Application" || cfg.config_type == "DynamicLibrary" || cfg.config_type == "Driver") {
            auto link = item_def.append_child("Link");
            if (!cfg.link.sub_system.empty())
                link.append_child("SubSystem").text() = cfg.link.sub_system.c_str();
            if (cfg.link.generate_debug_info)
                link.append_child("GenerateDebugInformation").text() = "true";

            // For DLL projects, ensure import library is generated
            if (cfg.config_type == "DynamicLibrary") {
                std::string target_name = cfg.target_name.empty() ? project.name : cfg.target_name;
                link.append_child("ImportLibrary").text() = ("$(OutDir)" + target_name + ".lib").c_str();
            }

            if (!cfg.link.additional_dependencies.empty()) {
                // Add leading semicolon if there are Library elements to inherit from
                std::string deps_str;
                if (!project.libraries.empty()) {
                    deps_str = ";";
                }

                // Make absolute library paths relative to vcxproj location
                std::vector<std::string> relative_deps;
                for (const auto& dep : cfg.link.additional_dependencies) {
                    // Only make relative if it's an absolute file path
                    if (fs::path(dep).is_absolute()) {
                        relative_deps.push_back(make_relative_path(dep, output_path));
                    } else {
                        // Keep system libraries (e.g., shell32.lib, opengl32.lib) as-is
                        relative_deps.push_back(dep);
                    }
                }

                deps_str += join_vector(relative_deps, ";");
                if (deps_str.find("%(AdditionalDependencies)") == std::string::npos) {
                    if (!deps_str.empty() && deps_str.back() != ';') {
                        deps_str += ";";
                    }
                    deps_str += "%(AdditionalDependencies)";
                }
                link.append_child("AdditionalDependencies").text() = deps_str.c_str();
            }
            if (!cfg.link.additional_library_directories.empty()) {
                // Make library directories relative to the output path
                std::vector<std::string> relative_libdirs;
                for (const auto& libdir : cfg.link.additional_library_directories) {
                    relative_libdirs.push_back(make_relative_path(libdir, output_path));
                }
                link.append_child("AdditionalLibraryDirectories").text() =
                    join_vector(relative_libdirs, ";").c_str();
            }
            if (!cfg.link.ignore_specific_default_libraries.empty())
                link.append_child("IgnoreSpecificDefaultLibraries").text() =
                    join_vector(cfg.link.ignore_specific_default_libraries, ";").c_str();
            if (cfg.link.ignore_all_default_libraries)
                link.append_child("IgnoreAllDefaultLibraries").text() = "true";
            if (!cfg.link.module_definition_file.empty())
                link.append_child("ModuleDefinitionFile").text() =
                    make_relative_path(cfg.link.module_definition_file, output_path).c_str();
            {
                // Build whole-archive linker flags for dependencies marked WHOLE_ARCHIVE
                std::string whole_archive_opts;
                for (const auto& dep : project.project_references) {
                    if (!dep.whole_archive) continue;
                    // Find the dependency project to determine its output library name
                    for (const auto& sol_proj : solution.projects) {
                        if (sol_proj.name == dep.name) {
                            std::string lib_name;
                            auto dep_cfg_it = sol_proj.configurations.find(config_key);
                            if (dep_cfg_it != sol_proj.configurations.end() && !dep_cfg_it->second.target_name.empty()) {
                                lib_name = dep_cfg_it->second.target_name;
                            } else {
                                lib_name = sol_proj.name;
                            }
                            if (!whole_archive_opts.empty()) whole_archive_opts += " ";
                            whole_archive_opts += "/WHOLEARCHIVE:" + lib_name;
                            break;
                        }
                    }
                }
                // Combine existing additional_options with whole-archive flags
                std::string combined_options = cfg.link.additional_options;
                if (!whole_archive_opts.empty()) {
                    if (!combined_options.empty()) combined_options += " ";
                    combined_options += whole_archive_opts;
                }
                if (!combined_options.empty())
                    link.append_child("AdditionalOptions").text() = combined_options.c_str();
            }
            if (cfg.link.enable_comdat_folding.value_or(false))
                link.append_child("EnableCOMDATFolding").text() = "true";
            if (cfg.link.optimize_references.value_or(false))
                link.append_child("OptimizeReferences").text() = "true";

            // New linker settings
            if (!cfg.link.show_progress.empty())
                link.append_child("ShowProgress").text() = cfg.link.show_progress.c_str();
            if (!cfg.link.output_file.empty())
                link.append_child("OutputFile").text() = cfg.link.output_file.c_str();
            if (cfg.link.suppress_startup_banner)
                link.append_child("SuppressStartupBanner").text() = "true";
            if (!cfg.link.program_database_file.empty())
                link.append_child("ProgramDatabaseFile").text() = cfg.link.program_database_file.c_str();
            if (cfg.link.generate_map_file)
                link.append_child("GenerateMapFile").text() = "true";
            if (!cfg.link.map_file_name.empty())
                link.append_child("MapFileName").text() = cfg.link.map_file_name.c_str();
            if (cfg.link.fixed_base_address)
                link.append_child("FixedBaseAddress").text() = "true";
            // Write RandomizedBaseAddress if explicitly set, or auto-suppress when FIXED is requested
            if (cfg.link.randomized_base_address.has_value()) {
                link.append_child("RandomizedBaseAddress").text() =
                    cfg.link.randomized_base_address.value() ? "true" : "false";
            } else if (cfg.link.fixed_base_address ||
                       cfg.link.additional_options.find("/FIXED") != std::string::npos) {
                link.append_child("RandomizedBaseAddress").text() = "false";
            }
            if (cfg.link.large_address_aware)
                link.append_child("LargeAddressAware").text() = "true";
            if (!cfg.link.base_address.empty())
                link.append_child("BaseAddress").text() = cfg.link.base_address.c_str();
            if (!cfg.link.target_machine.empty())
                link.append_child("TargetMachine").text() = cfg.link.target_machine.c_str();
            if (!cfg.link.error_reporting.empty())
                link.append_child("LinkErrorReporting").text() = cfg.link.error_reporting.c_str();
            if (!cfg.link.entry_point_symbol.empty())
                link.append_child("EntryPointSymbol").text() = cfg.link.entry_point_symbol.c_str();
            if (!cfg.link.version.empty())
                link.append_child("Version").text() = cfg.link.version.c_str();
            // Always write ImageHasSafeExceptionHandlers to avoid linker errors with libs that lack safe exception handlers
            link.append_child("ImageHasSafeExceptionHandlers").text() = cfg.link.image_has_safe_exception_handlers ? "true" : "false";
        }

        // Lib settings (for static libraries)
        if (cfg.config_type == "StaticLibrary") {
            auto lib = item_def.append_child("Lib");
            if (cfg.lib.use_unicode_response_files)
                lib.append_child("UseUnicodeResponseFiles").text() = "true";
            if (!cfg.lib.additional_dependencies.empty())
                lib.append_child("AdditionalDependencies").text() = join_vector(cfg.lib.additional_dependencies, ";").c_str();
            if (!cfg.lib.output_file.empty())
                lib.append_child("OutputFile").text() = cfg.lib.output_file.c_str();
            if (cfg.lib.suppress_startup_banner)
                lib.append_child("SuppressStartupBanner").text() = "true";
            if (!cfg.lib.additional_options.empty())
                lib.append_child("AdditionalOptions").text() = cfg.lib.additional_options.c_str();
        }

        // ResourceCompile settings
        if (!cfg.resource_compile.preprocessor_definitions.empty() ||
            !cfg.resource_compile.culture.empty() ||
            !cfg.resource_compile.additional_include_directories.empty()) {
            auto rc = item_def.append_child("ResourceCompile");
            if (!cfg.resource_compile.preprocessor_definitions.empty())
                rc.append_child("PreprocessorDefinitions").text() =
                    join_vector(cfg.resource_compile.preprocessor_definitions, ";").c_str();
            if (!cfg.resource_compile.culture.empty())
                rc.append_child("Culture").text() = cfg.resource_compile.culture.c_str();
            if (!cfg.resource_compile.additional_include_directories.empty())
                rc.append_child("AdditionalIncludeDirectories").text() =
                    join_vector(cfg.resource_compile.additional_include_directories, ";").c_str();
        }

        // Manifest settings - always write
        auto manifest = item_def.append_child("Manifest");
        if (cfg.manifest.suppress_startup_banner) {
            manifest.append_child("SuppressStartupBanner").text() = "true";
        }
        if (!cfg.manifest.additional_manifest_files.empty()) {
            manifest.append_child("AdditionalManifestFiles").text() = cfg.manifest.additional_manifest_files.c_str();
        }

        // Xdcmake settings - always write
        auto xdcmake = item_def.append_child("Xdcmake");
        if (cfg.xdcmake.suppress_startup_banner) {
            xdcmake.append_child("SuppressStartupBanner").text() = "true";
        }

        // Bscmake settings - always write
        auto bscmake = item_def.append_child("Bscmake");
        if (cfg.bscmake.suppress_startup_banner)
            bscmake.append_child("SuppressStartupBanner").text() = "true";
        if (!cfg.bscmake.output_file.empty())
            bscmake.append_child("OutputFile").text() = cfg.bscmake.output_file.c_str();

        // Message Compiler settings
        if (project.has_mc_files) {
            auto mc = item_def.append_child("MessageCompile");
            if (!cfg.mc.header_file_path.empty())
                mc.append_child("HeaderFilePath").text() =
                    make_relative_path(cfg.mc.header_file_path, output_path).c_str();
            if (!cfg.mc.rc_file_path.empty())
                mc.append_child("RCFilePath").text() =
                    make_relative_path(cfg.mc.rc_file_path, output_path).c_str();
            if (!cfg.mc.additional_options.empty())
                mc.append_child("AdditionalOptions").text() = cfg.mc.additional_options.c_str();
        }

        // MIDL compiler settings
        if (project.has_idl_files) {
            auto midl = item_def.append_child("Midl");
            if (!cfg.midl.output_directory.empty())
                midl.append_child("OutputDirectory").text() =
                    make_relative_path(cfg.midl.output_directory, output_path).c_str();
            if (!cfg.midl.header_file_name.empty())
                midl.append_child("HeaderFileName").text() = cfg.midl.header_file_name.c_str();
            if (!cfg.midl.type_library_name.empty())
                midl.append_child("TypeLibraryName").text() = cfg.midl.type_library_name.c_str();
            if (!cfg.midl.dlldata_file_name.empty())
                midl.append_child("DllDataFileName").text() = cfg.midl.dlldata_file_name.c_str();
            if (!cfg.midl.interface_identifier_file_name.empty())
                midl.append_child("InterfaceIdentifierFileName").text() = cfg.midl.interface_identifier_file_name.c_str();
            if (!cfg.midl.proxy_file_name.empty())
                midl.append_child("ProxyFileName").text() = cfg.midl.proxy_file_name.c_str();
            if (!cfg.midl.preprocessor_definitions.empty())
                midl.append_child("PreprocessorDefinitions").text() =
                    join_vector(cfg.midl.preprocessor_definitions, ";").c_str();
            if (!cfg.midl.additional_options.empty())
                midl.append_child("AdditionalOptions").text() = cfg.midl.additional_options.c_str();
            if (!cfg.midl.default_char_type.empty())
                midl.append_child("DefaultCharType").text() = cfg.midl.default_char_type.c_str();
            if (!cfg.midl.target_environment.empty())
                midl.append_child("TargetEnvironment").text() = cfg.midl.target_environment.c_str();
        }

        // Build events (don't call unescape_newlines - commands already have real newlines from buildscript_parser)
        if (!cfg.pre_build_event.command.empty()) {
            auto pre_build = item_def.append_child("PreBuildEvent");
            pre_build.append_child("Command").text() = cfg.pre_build_event.command.c_str();
            if (!cfg.pre_build_event.message.empty())
                pre_build.append_child("Message").text() = cfg.pre_build_event.message.c_str();
        }
        if (!cfg.pre_link_event.command.empty()) {
            auto pre_link = item_def.append_child("PreLinkEvent");
            pre_link.append_child("Command").text() = cfg.pre_link_event.command.c_str();
            if (!cfg.pre_link_event.message.empty())
                pre_link.append_child("Message").text() = cfg.pre_link_event.message.c_str();
        } else {
            // Add empty PreLinkEvent if no command
            item_def.append_child("PreLinkEvent");
        }
        if (!cfg.post_build_event.command.empty()) {
            auto post_build = item_def.append_child("PostBuildEvent");
            post_build.append_child("Command").text() = cfg.post_build_event.command.c_str();
            if (!cfg.post_build_event.message.empty())
                post_build.append_child("Message").text() = cfg.post_build_event.message.c_str();
        }
        // Always add empty CustomBuildStep
        item_def.append_child("CustomBuildStep");
    }

    // Source files
    std::map<FileType, std::vector<const SourceFile*>> files_by_type;
    for (const auto& src : project.sources) {
        files_by_type[src.type].push_back(&src);
    }

    for (const auto& [type, files] : files_by_type) {
        if (files.empty()) continue;

        auto item_group = root.append_child("ItemGroup");
        std::string type_name = get_file_type_name(type);

        for (const auto* src : files) {
            auto file_elem = item_group.append_child(type_name.c_str());
            std::string relative_path = make_relative_path(src->path, output_path);
            file_elem.append_attribute("Include") = relative_path.c_str();

            // File-specific settings
            for (const auto& [config_key, excluded] : src->settings.excluded) {
                if (excluded) {
                    // If config_key is "*" (ALL_CONFIGS), expand to all configurations
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg_name + "'";
                            auto node = file_elem.append_child("ExcludedFromBuild");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = "true";
                        }
                    } else {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                        auto node = file_elem.append_child("ExcludedFromBuild");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = "true";
                    }
                }
            }

            for (const auto& [config_key, obj_file] : src->settings.object_file) {
                if (!obj_file.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("ObjectFileName");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = obj_file.c_str();
                    }
                }
            }

            // Per-file, per-config AdditionalIncludeDirectories
            for (const auto& [config_key, includes] : src->settings.additional_includes) {
                if (!includes.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("AdditionalIncludeDirectories");
                        node.append_attribute("Condition") = condition.c_str();
                        // Make include directories relative to the output path
                        std::vector<std::string> relative_includes;
                        for (const auto& inc : includes) {
                            relative_includes.push_back(make_relative_path(inc, output_path));
                        }
                        node.text() = join_vector(relative_includes, ";").c_str();
                    }
                }
            }

            // Per-file, per-config PreprocessorDefinitions
            for (const auto& [config_key, defines] : src->settings.preprocessor_defines) {
                if (!defines.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("PreprocessorDefinitions");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = join_vector(defines, ";").c_str();
                    }
                }
            }

            // Per-file, per-config AdditionalOptions
            for (const auto& [config_key, options] : src->settings.additional_options) {
                if (!options.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("AdditionalOptions");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = (join_vector(options, " ") + " %(AdditionalOptions)").c_str();
                    }
                }
            }

            // Per-file, per-config PrecompiledHeader settings
            for (const auto& [config_key, pch] : src->settings.pch) {
                if (!pch.mode.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        if (!pch.mode.empty()) {
                            auto node = file_elem.append_child("PrecompiledHeader");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = pch.mode.c_str();
                        }

                        // If the file is not using PCH, don't write header or output file
                        if (pch.mode == "NotUsing") {
                            continue;
                        }

                        // Use file-level header if specified, otherwise inherit from project-level
                        // (but only if project-level is not "NotUsing")
                        std::string header_to_use = pch.header;
                        std::string output_to_use = pch.output;
                        if (header_to_use.empty() && project.configurations.count(cfg)) {
                            auto& proj_pch = project.configurations.at(cfg).cl_compile.pch;
                            if (proj_pch.mode != "NotUsing") {
                                header_to_use = proj_pch.header;
                                output_to_use = proj_pch.output;
                            }
                        }

                        // Only write PrecompiledHeaderFile if it was explicitly specified
                        // Extract filename only to match makefile_generator behavior
                        if (!header_to_use.empty()) {
                            std::string pch_filename = fs::path(header_to_use).filename().string();
                            auto node = file_elem.append_child("PrecompiledHeaderFile");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = pch_filename.c_str();
                        }
                        // Only write PrecompiledHeaderOutputFile if it was explicitly specified
                        // Don't auto-generate - let MSBuild use its defaults
                        if (!output_to_use.empty()) {
                            auto node = file_elem.append_child("PrecompiledHeaderOutputFile");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = output_to_use.c_str();
                        }
                    }
                }
            }

            // Auto-set CompileAs based on file extension if not explicitly set per-file
            if (src->settings.compile_as.empty()) {
                // Check if project-level compile_as is set in any configuration.
                // If so, skip per-file auto-detection — the ItemDefinitionGroup setting
                // applies to all files, and the user overrides specific files via
                // set_file_properties().
                bool has_project_level_compile_as = false;
                for (const auto& [cfg_name, cfg] : project.configurations) {
                    if (!cfg.cl_compile.compile_as.empty()) {
                        has_project_level_compile_as = true;
                        break;
                    }
                }

                if (!has_project_level_compile_as) {
                    // No project-level compile_as: auto-detect per file based on extension
                    fs::path file_path(src->path);
                    std::string ext = file_path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    std::string auto_compile_as;
                    if (ext == ".c") {
                        auto_compile_as = "CompileAsC";
                    } else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                        auto_compile_as = "CompileAsCpp";
                    }

                    if (!auto_compile_as.empty()) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg_name + "'";
                            auto node = file_elem.append_child("CompileAs");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = auto_compile_as.c_str();
                        }
                    }
                }
            }

            // Per-file, per-config CompileAs (overrides auto-detection)
            for (const auto& [config_key, compile_as] : src->settings.compile_as) {
                if (!compile_as.empty()) {
                    // Expand ALL_CONFIGS wildcard to individual configs
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("CompileAs");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = compile_as.c_str();
                    }
                }
            }

            // Per-file, per-config Optimization
            for (const auto& [config_key, opt] : src->settings.optimization) {
                if (!opt.empty()) {
                    std::vector<std::string> configs_to_write;
                    if (config_key == ALL_CONFIGS) {
                        for (const auto& [cfg_name, cfg] : project.configurations) {
                            configs_to_write.push_back(cfg_name);
                        }
                    } else {
                        configs_to_write.push_back(config_key);
                    }

                    for (const auto& cfg : configs_to_write) {
                        std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                        auto node = file_elem.append_child("Optimization");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = opt.c_str();
                    }
                }
            }

            // NASM custom build rules (auto-generated)
            if (type == FileType::NASM && src->custom_command.empty()) {
                for (const auto& [cfg_key, cfg] : project.configurations) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg_key + "'";

                    // Determine output format
                    std::string fmt = cfg.nasm.format;
                    if (fmt.empty()) {
                        // Default based on platform
                        auto [cfg_name, platform] = parse_config_key(cfg_key);
                        if (is_windows_platform(platform)) {
                            fmt = (to_lower(platform) == "win32" || to_lower(platform) == "x86") ? "win32" : "win64";
                        } else {
                            fmt = "elf64";
                        }
                    }

                    // Determine output extension based on format
                    std::string out_ext = (fmt == "bin") ? ".bin" : ".obj";

                    // Build NASM command line
                    std::string nasm_exe = cfg.nasm.path.empty() ? "nasm" : "\"" + cfg.nasm.path + "\"";
                    std::string nasm_cmd = nasm_exe + " -f " + fmt;

                    // Add include directories
                    for (const auto& inc : cfg.nasm.include_directories) {
                        std::string rel_inc = make_relative_path(inc, output_path);
                        nasm_cmd += " -I\"" + rel_inc + "/\"";
                    }

                    // Add defines
                    for (const auto& def : cfg.nasm.preprocessor_definitions) {
                        nasm_cmd += " -D" + def;
                    }

                    // Add additional flags
                    if (!cfg.nasm.additional_options.empty()) {
                        nasm_cmd += " " + cfg.nasm.additional_options;
                    }

                    nasm_cmd += " -o \"$(IntDir)%(Filename)" + out_ext + "\" \"%(FullPath)\"";

                    auto cmd_node = file_elem.append_child("Command");
                    cmd_node.append_attribute("Condition") = condition.c_str();
                    cmd_node.text() = nasm_cmd.c_str();

                    auto msg_node = file_elem.append_child("Message");
                    msg_node.append_attribute("Condition") = condition.c_str();
                    std::string msg = "Assembling %(Filename)%(Extension) with NASM (-f " + fmt + ")";
                    msg_node.text() = msg.c_str();

                    auto out_node = file_elem.append_child("Outputs");
                    out_node.append_attribute("Condition") = condition.c_str();
                    std::string output = "$(IntDir)%(Filename)" + out_ext;
                    out_node.text() = output.c_str();
                }
            }

            // Custom build tool
            if (type == FileType::CustomBuild) {
                // Get directories for path adjustment
                std::string from_dir = project.buildscript_path.empty() ?
                    fs::absolute(output_path).parent_path().string() :
                    project.buildscript_path;
                std::string to_dir = fs::absolute(output_path).parent_path().string();

                for (const auto& [config_key, command] : src->custom_command) {
                    if (!command.empty()) {
                        // Expand ALL_CONFIGS wildcard to individual configs
                        std::vector<std::string> configs_to_write;
                        if (config_key == ALL_CONFIGS) {
                            for (const auto& [cfg_name, cfg] : project.configurations) {
                                configs_to_write.push_back(cfg_name);
                            }
                        } else {
                            configs_to_write.push_back(config_key);
                        }

                        for (const auto& cfg : configs_to_write) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                            // Unescape newlines first (from buildscript's \n to actual newlines)
                            std::string unescaped_command = unescape_newlines(command);
                            // Adjust paths in the command from buildscript location to vcxproj location
                            std::string adjusted_command = adjust_command_paths(unescaped_command, from_dir, to_dir);
                            auto node = file_elem.append_child("Command");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = adjusted_command.c_str();
                        }
                    }
                }
                for (const auto& [config_key, message] : src->custom_message) {
                    if (!message.empty()) {
                        // Expand ALL_CONFIGS wildcard to individual configs
                        std::vector<std::string> configs_to_write;
                        if (config_key == ALL_CONFIGS) {
                            for (const auto& [cfg_name, cfg] : project.configurations) {
                                configs_to_write.push_back(cfg_name);
                            }
                        } else {
                            configs_to_write.push_back(config_key);
                        }

                        for (const auto& cfg : configs_to_write) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                            std::string unescaped_message = unescape_newlines(message);
                            auto node = file_elem.append_child("Message");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = unescaped_message.c_str();
                        }
                    }
                }
                for (const auto& [config_key, outputs] : src->custom_outputs) {
                    if (!outputs.empty()) {
                        // Expand ALL_CONFIGS wildcard to individual configs
                        std::vector<std::string> configs_to_write;
                        if (config_key == ALL_CONFIGS) {
                            for (const auto& [cfg_name, cfg] : project.configurations) {
                                configs_to_write.push_back(cfg_name);
                            }
                        } else {
                            configs_to_write.push_back(config_key);
                        }

                        for (const auto& cfg : configs_to_write) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                            auto node = file_elem.append_child("Outputs");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = outputs.c_str();
                        }
                    }
                }
                for (const auto& [config_key, inputs] : src->custom_inputs) {
                    if (!inputs.empty()) {
                        // Expand ALL_CONFIGS wildcard to individual configs
                        std::vector<std::string> configs_to_write;
                        if (config_key == ALL_CONFIGS) {
                            for (const auto& [cfg_name, cfg] : project.configurations) {
                                configs_to_write.push_back(cfg_name);
                            }
                        } else {
                            configs_to_write.push_back(config_key);
                        }

                        for (const auto& cfg : configs_to_write) {
                            std::string condition = "'$(Configuration)|$(Platform)'=='" + cfg + "'";
                            auto node = file_elem.append_child("AdditionalInputs");
                            node.append_attribute("Condition") = condition.c_str();
                            node.text() = inputs.c_str();
                        }
                    }
                }
            }
        }
    }

    // Project references
    if (!project.project_references.empty()) {
        auto ref_group = root.append_child("ItemGroup");
        for (const auto& dep : project.project_references) {
            // Skip INTERFACE dependencies - they don't get linked, only their includes propagate
            if (dep.visibility == DependencyVisibility::INTERFACE) {
                continue;
            }

            auto ref_elem = ref_group.append_child("ProjectReference");

            // Find the referenced project in the solution to get its GUID and actual path
            std::string ref_path;
            bool found = false;
            if (const Project* sol_proj = find_dependency_project(solution, dep.name)) {
                found = true;

                // All .vcxproj files for a solution live side-by-side in the
                // effective build root, so the reference is always a bare filename.
                ref_path = sol_proj->name + GENERATED_VCXPROJ;

                ref_elem.append_attribute("Include") = ref_path.c_str();

                // Add the Project GUID element
                ref_elem.append_child("Project").text() = ("{" + sol_proj->uuid + "}").c_str();

                // Check if the referenced project actually produces a library to link.
                // If it has no linkable sources (only headers or nothing), disable automatic linking.
                // This is crucial for wrapper projects that only propagate public_libs.
                bool has_linkable_content = false;
                for (const auto& src : sol_proj->sources) {
                    if (src.type == FileType::ClCompile ||
                        src.type == FileType::ObjCxx ||
                        src.type == FileType::ResourceCompile ||
                        src.type == FileType::CustomBuild ||
                        src.type == FileType::NASM) {
                        has_linkable_content = true;
                        break;
                    }
                }
                if (!has_linkable_content) {
                    ref_elem.append_child("LinkLibraryDependencies").text() = "false";
                }
            }

            // Fallback if project not found in solution (shouldn't happen)
            if (!found) {
                ref_path = dep.name + GENERATED_VCXPROJ;
                ref_elem.append_attribute("Include") = ref_path.c_str();
            }
        }
    }

    // Library references
    if (!project.libraries.empty()) {
        auto lib_group = root.append_child("ItemGroup");
        for (const auto& lib : project.libraries) {
            auto lib_elem = lib_group.append_child("Library");
            // Only make library paths relative if they're absolute file paths
            // System libraries (e.g., shell32.lib) should be kept as-is
            std::string lib_path;
            if (fs::path(lib.path).is_absolute()) {
                lib_path = make_relative_path(lib.path, output_path);
            } else {
                lib_path = lib.path;
            }
            lib_elem.append_attribute("Include") = lib_path.c_str();

            // Write per-config exclusions
            for (const auto& [config_key, excluded] : lib.excluded) {
                if (excluded) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = lib_elem.append_child("ExcludedFromBuild");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = "true";
                }
            }
        }
    }

    // Import Cpp targets
    auto import3 = root.append_child("Import");
    import3.append_attribute("Project") = "$(VCTargetsPath)\\Microsoft.Cpp.targets";

    // Extension targets - conditionally import MASM targets if project has MASM files
    auto ext_targets = root.append_child("ImportGroup");
    ext_targets.append_attribute("Label") = "ExtensionTargets";
    if (project.has_masm_files) {
        auto masm_import = ext_targets.append_child("Import");
        masm_import.append_attribute("Project") = "$(VCTargetsPath)\\BuildCustomizations\\masm.targets";
    }

    // Save to file
    bool saved = doc.save_file(output_path.c_str(), "  ", pugi::format_default | pugi::format_write_bom);
    if (!saved) {
        return false;
    }
    return generate_vcxproj_filters(project, output_path);
}

bool VcxprojGenerator::generate_vcxproj_filters(const Project& project, const std::string& vcxproj_path) {
    pugi::xml_document doc;

    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    auto root = doc.append_child("Project");
    root.append_attribute("ToolsVersion") = "4.0";

    std::set<std::string> filters;
    for (const auto& src : project.sources) {
        if (!src.filter.empty()) {
            collect_filter_and_parents(src.filter, filters);
        }
    }

    if (!filters.empty()) {
        auto filter_group = root.append_child("ItemGroup");
        for (const auto& filter : filters) {
            auto filter_elem = filter_group.append_child("Filter");
            std::string msvc_filter = to_msvc_filter_path(filter);
            filter_elem.append_attribute("Include") = msvc_filter.c_str();
            std::string guid = "{" + make_stable_filter_guid(project.uuid + "|" + filter) + "}";
            filter_elem.append_child("UniqueIdentifier").text() = guid.c_str();
        }
    }

    std::map<FileType, std::vector<const SourceFile*>> files_by_type;
    for (const auto& src : project.sources) {
        if (!src.filter.empty()) {
            files_by_type[src.type].push_back(&src);
        }
    }

    for (const auto& [type, files] : files_by_type) {
        if (files.empty()) continue;

        auto item_group = root.append_child("ItemGroup");
        std::string type_name = get_file_type_name(type);
        for (const auto* src : files) {
            auto file_elem = item_group.append_child(type_name.c_str());
            std::string relative_path = make_relative_path(src->path, vcxproj_path);
            file_elem.append_attribute("Include") = relative_path.c_str();
            std::string msvc_filter = to_msvc_filter_path(src->filter);
            file_elem.append_child("Filter").text() = msvc_filter.c_str();
        }
    }

    fs::path filters_path = vcxproj_path;
    filters_path += ".filters";
    return doc.save_file(filters_path.string().c_str(), "  ", pugi::format_default | pugi::format_write_bom);
}

bool VcxprojGenerator::generate_sln(const Solution& solution, const std::string& output_path) {
    std::ofstream file(output_path);
    if (!file.is_open()) {
        return false;
    }

    // Header - version-appropriate for the target toolset
    std::string toolset = resolve_solution_toolset(solution);
    auto ver = get_sln_version_info(toolset);

    file << "\xEF\xBB\xBF\n"; // UTF-8 BOM
    file << "Microsoft Visual Studio Solution File, Format Version " << ver.format_version << "\n";
    file << ver.comment << "\n";
    if (!ver.visual_studio_version.empty()) {
        file << "VisualStudioVersion = " << ver.visual_studio_version << "\n";
        file << "MinimumVisualStudioVersion = " << ver.min_vs_version << "\n";
    }

    // Projects
    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;  // Skip synthetic find_package projects
        // .vcxproj is co-located with this .sln in the effective build root,
        // so the reference is always a bare filename.
        std::string vcxproj_path = proj.name + GENERATED_VCXPROJ;
        file << "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \""
             << proj.name << "\", \"" << vcxproj_path << "\", \"{"
             << proj.uuid << "}\"\n";

        // Write ProjectSection(ProjectDependencies) if the project has dependencies
        if (!proj.project_references.empty()) {
            file << "\tProjectSection(ProjectDependencies) = postProject\n";
            for (const auto& dep : proj.project_references) {
                // Skip INTERFACE dependencies - they don't get linked
                if (dep.visibility == DependencyVisibility::INTERFACE) {
                    continue;
                }

                // Look up the UUID for the dependency
                if (const Project* dep_project = find_dependency_project(solution, dep.name)) {
                    file << "\t\t{" << dep_project->uuid << "} = {" << dep_project->uuid << "}\n";
                }
            }
            file << "\tEndProjectSection\n";
        }

        file << "EndProject\n";
    }

    // Solution folders
    for (const auto& folder : solution.folders) {
        file << "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \""
             << folder.name << "\", \"" << folder.name << "\", \"{"
             << folder.uuid << "}\"\n";
        file << "EndProject\n";
    }

    // Global section
    file << "Global\n";

    // Solution configuration platforms
    file << "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
    for (const auto& config : solution.configurations) {
        for (const auto& platform : solution.platforms) {
            if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
            std::string key = config + "|" + platform;
            file << "\t\t" << key << " = " << key << "\n";
        }
    }
    file << "\tEndGlobalSection\n";

    // Project configuration platforms
    file << "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;  // Skip synthetic find_package projects
        for (const auto& config : solution.configurations) {
            for (const auto& platform : solution.platforms) {
                if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
                std::string key = config + "|" + platform;
                std::string guid = "{" + proj.uuid + "}";
                file << "\t\t" << guid << "." << key << ".ActiveCfg = " << key << "\n";
                file << "\t\t" << guid << "." << key << ".Build.0 = " << key << "\n";
            }
        }
    }
    file << "\tEndGlobalSection\n";

    // Solution properties
    file << "\tGlobalSection(SolutionProperties) = preSolution\n";
    file << "\t\tHideSolutionNode = FALSE\n";
    file << "\tEndGlobalSection\n";

    // Nested projects (solution folder membership and folder-to-folder nesting)
    if (!solution.folders.empty()) {
        std::map<std::string, std::string> folder_uuid_map;
        for (const auto& f : solution.folders)
            folder_uuid_map[f.path] = f.uuid;

        file << "\tGlobalSection(NestedProjects) = preSolution\n";

        // Folder-to-folder nesting (child folder -> parent folder)
        for (const auto& f : solution.folders) {
            if (!f.parent.empty()) {
                auto parent_it = folder_uuid_map.find(f.parent);
                if (parent_it != folder_uuid_map.end())
                    file << "\t\t{" << f.uuid << "} = {" << parent_it->second << "}\n";
            }
        }

        // Project-to-folder nesting
        for (const auto& proj : solution.projects) {
            if (proj.is_package_project) continue;  // Skip synthetic find_package projects
            if (!proj.solution_folder.empty()) {
                auto it = folder_uuid_map.find(proj.solution_folder);
                if (it != folder_uuid_map.end())
                    file << "\t\t{" << proj.uuid << "} = {" << it->second << "}\n";
            }
        }
        file << "\tEndGlobalSection\n";
    }

    file << "EndGlobal\n";
    file.close();
    return true;
}

bool VcxprojGenerator::generate_slnx(const Solution& solution, const std::string& output_path) {
    pugi::xml_document doc;

    // XML declaration
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    // Root Solution element
    auto root = doc.append_child("Solution");

    // Configurations section
    auto configs = root.append_child("Configurations");

    // Build types (configurations like Debug, Release)
    for (const auto& config : solution.configurations) {
        auto build_type = configs.append_child("BuildType");
        build_type.append_attribute("Name") = config.c_str();
    }

    // Platforms (Win32, x64, etc.)
    for (const auto& platform : solution.platforms) {
        if (is_unix_platform(platform)) continue;  // Skip Unix configs for vcxproj
        auto plat_elem = configs.append_child("Platform");
        plat_elem.append_attribute("Name") = platform.c_str();
    }

    // Helper: emit a Project element under a given parent node
    auto emit_project = [&](pugi::xml_node parent, const Project& proj) {
        // .vcxproj is co-located with this .slnx in the effective build root.
        std::string vcxproj_path = proj.name + GENERATED_VCXPROJ;

        auto project = parent.append_child("Project");
        project.append_attribute("Path") = vcxproj_path.c_str();
        project.append_attribute("Type") = "8bc9ceb8-8b4a-11d0-8d11-00a0c91bc942"; // C++ GUID
        project.append_attribute("Id") = proj.uuid.c_str();

        // Add build dependencies
        for (const auto& dep : proj.project_references) {
            if (dep.visibility == DependencyVisibility::INTERFACE) {
                continue;
            }

            std::string dep_path;
            if (const Project* dep_proj = find_dependency_project(solution, dep.name)) {
                // All vcxproj files live next to this .slnx in the build root.
                dep_path = dep_proj->name + GENERATED_VCXPROJ;
            }

            if (!dep_path.empty()) {
                auto dep_elem = project.append_child("BuildDependency");
                dep_elem.append_attribute("Project") = dep_path.c_str();
            }
        }
    };

    // Solution folders — .slnx requires ALL folders as direct children of <Solution>
    // with full path names (e.g. "/Engine/ThirdParty/"), not nested XML elements.
    std::vector<SolutionFolder> sorted_folders = solution.folders;
    std::sort(sorted_folders.begin(), sorted_folders.end(),
        [](const SolutionFolder& a, const SolutionFolder& b) { return a.path < b.path; });

    std::map<std::string, pugi::xml_node> folder_nodes;
    for (const auto& folder : sorted_folders) {
        auto folder_node = root.append_child("Folder");
        folder_node.append_attribute("Name") = ("/" + folder.path + "/").c_str();
        folder_nodes[folder.path] = folder_node;
    }

    // Projects
    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;  // Skip synthetic find_package projects
        if (!proj.solution_folder.empty()) {
            auto it = folder_nodes.find(proj.solution_folder);
            if (it != folder_nodes.end()) {
                emit_project(it->second, proj);
                continue;
            }
        }
        emit_project(root, proj);
    }

    // Save to file with tab indentation
    return doc.save_file(output_path.c_str(), "\t", pugi::format_default);
}

bool VcxprojGenerator::generate(Solution& solution, const std::string& output_dir) {
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

    // .vcxproj / .sln / .slnx all land in this directory. Default is
    // "<output_dir>/build/"; users override via -B / --build-dir. Empty
    // build_dir_ means "write directly into output_dir" (back-compat escape hatch
    // for callers that explicitly opted out).
    fs::path effective_build_root = build_dir_.empty()
        ? fs::path(output_dir.empty() ? "." : output_dir)
        : fs::path(output_dir.empty() ? "." : output_dir) / build_dir_;
    if (!fs::exists(effective_build_root)) {
        try {
            fs::create_directories(effective_build_root);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create build directory "
                      << effective_build_root << ": " << e.what() << "\n";
            return false;
        }
    }

    // 1. FIRST: Detect Visual Studio installation
    auto vs_info = VSDetector::detect_latest_vs();

    if (!vs_info.has_value()) {
        std::cerr << "Error: No Visual Studio installation detected.\n";
        std::cerr << "Please install Visual Studio 2017 or later.\n";
        return false;
    }

    std::cout << "Detected: Visual Studio " << vs_info->year
              << " (toolset " << vs_info->platform_toolset << ")\n";

    // 2. SECOND: Set default toolsets and validate
    auto& toolset_registry = ToolsetRegistry::instance();
    std::string registry_default = toolset_registry.get_default();

    // Validate CLI default against detected VS
    if (!registry_default.empty()) {
        int default_year = toolset_registry.get_toolset_year(registry_default);
        if (default_year > vs_info->year) {
            std::cerr << "Warning: CLI default toolset " << registry_default
                      << " (Visual Studio " << default_year << ")"
                      << " is newer than detected Visual Studio " << vs_info->year
                      << " (toolset " << vs_info->platform_toolset << ").\n";
            std::cerr << "         Falling back to detected toolset "
                      << vs_info->platform_toolset << ".\n";
            registry_default = vs_info->platform_toolset;
            toolset_registry.set_default(registry_default);
        }
    }

    bool already_logged = false;

    for (auto& proj : solution.projects) {
        if (proj.is_package_project) continue;  // Skip synthetic find_package projects
#ifndef NDEBUG
        std::cout << "[DEBUG] Processing project: " << proj.name << "\n";
#endif
        for (auto& config_pair : proj.configurations) {
            auto& cfg = config_pair.second;

#ifndef NDEBUG
            std::cout << "[DEBUG]   Config: " << config_pair.first
                      << ", current toolset: '" << cfg.platform_toolset << "'\n";
#endif

            // If no toolset specified, use this priority:
            // 1. CLI default (from -t flag)
            // 2. Detected VS toolset (fallback)
            if (cfg.platform_toolset.empty()) {
                // Check if a default was set via CLI (-t flag) or environment variable
                if (!registry_default.empty()) {
                    cfg.platform_toolset = registry_default;
#ifndef NDEBUG
                    std::cout << "[DEBUG]   -> Set to CLI default toolset: " << cfg.platform_toolset << "\n";
#endif
                    if (!already_logged) {
                        std::cout << "Using CLI default toolset " << cfg.platform_toolset
                                  << " for projects without explicit toolset\n";
                        already_logged = true;
                    }
                } else {
                    // No CLI default, fall back to detected VS toolset
                    cfg.platform_toolset = vs_info->platform_toolset;
#ifndef NDEBUG
                    std::cout << "[DEBUG]   -> Set to detected toolset: " << cfg.platform_toolset << "\n";
#endif
                    if (!already_logged) {
                        std::cout << "Using detected toolset " << cfg.platform_toolset
                                  << " for projects without explicit toolset\n";
                        already_logged = true;
                    }
                }
            } else {
#ifndef NDEBUG
                std::cout << "[DEBUG]   -> Keeping explicit toolset: " << cfg.platform_toolset << "\n";
#endif
                // Validate explicitly specified toolset
                int specified_year = toolset_registry.get_toolset_year(cfg.platform_toolset);

#ifndef NDEBUG
                std::cout << "[DEBUG]   -> Toolset year: " << specified_year
                          << ", detected VS year: " << vs_info->year << "\n";
#endif

                if (specified_year > vs_info->year) {
                    std::cerr << "Error: Project '" << proj.name << "' requires toolset "
                              << cfg.platform_toolset << " (Visual Studio " << specified_year << ")\n";
                    std::cerr << "       but only Visual Studio " << vs_info->year
                              << " is installed.\n";
                    std::cerr << "       Please install Visual Studio " << specified_year
                              << " or newer, or change the toolset in the buildscript.\n";
                    return false;
                }
            }
        }
    }

    // 3. THIRD: Generate project files (now with correct toolsets)
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;  // Skip synthetic find_package projects
        std::string filename = project.name + GENERATED_VCXPROJ;
        fs::path vcxproj_path = effective_build_root / filename;

        if (!generate_vcxproj(project, solution, vcxproj_path.string())) {
            std::cerr << "Error: Failed to generate " << vcxproj_path << "\n";
            return false;
        }
    }

    // 4. FOURTH: Generate solution file
    if (!solution.projects.empty()) {
#ifndef NDEBUG
        std::cout << "[DEBUG] ========== Solution Generation Start ==========\n";
#endif

        std::string sln_name = solution.name.empty() ? solution.projects[0].name : solution.name;

        // Determine solution format based on detected VS installation
        bool use_slnx = (vs_info->year >= 2026);

#ifndef NDEBUG
        std::cout << "[DEBUG] Solution format decision: VS year " << vs_info->year
                  << " -> " << (use_slnx ? ".slnx" : ".sln") << "\n";
        std::cout << "[DEBUG] ========== Solution Generation End ==========\n";
#endif

        if (use_slnx) {
            // Generate .slnx for MSVC 2026
            fs::path slnx_path = effective_build_root / (sln_name + GENERATED_SLNX);

            std::cout << "Generating .slnx format for Visual Studio " << vs_info->year << "...\n";

            if (!generate_slnx(solution, slnx_path.string())) {
                std::cerr << "Error: Failed to generate " << slnx_path << "\n";
                return false;
            }
        } else {
            // Generate traditional .sln for older toolsets
            fs::path sln_path = effective_build_root / (sln_name + GENERATED_SLN);

            if (!generate_sln(solution, sln_path.string())) {
                std::cerr << "Error: Failed to generate " << sln_path << "\n";
                return false;
            }
        }
    }

    // Write build cache for --build support
    {
        BuildCache cache;
        cache.generator = "vcxproj";
        cache.solution_name = solution.name.empty() ? solution.projects[0].name : solution.name;
        std::string sln_filename = (vs_info->year >= 2026)
            ? cache.solution_name + GENERATED_SLNX
            : cache.solution_name + GENERATED_SLN;
        // Cache stores solution_file relative to the cache directory (output_dir),
        // and the runner resolves it as <cache_dir>/<solution_file>. Since the
        // .sln/.slnx now lives in build_dir_, prefix the filename accordingly.
        cache.solution_file = build_dir_.empty()
            ? sln_filename
            : (fs::path(build_dir_) / sln_filename).string();
        cache.vs_installation_path = vs_info->installation_path;
        cache.vs_year = vs_info->year;
        cache.platform_toolset = vs_info->platform_toolset;
        cache.configurations = solution.configurations;
        cache.platforms = solution.platforms;
        for (const auto& project : solution.projects) {
            if (project.is_package_project) continue;

            const std::string filename = project.name + GENERATED_VCXPROJ;
            const std::string project_file = build_dir_.empty()
                ? filename
                : (fs::path(build_dir_) / filename).string();

            std::set<std::string> names;
            names.insert(project.name);
            for (const auto& [_, config] : project.configurations) {
                if (!config.target_name.empty()) {
                    names.insert(config.target_name);
                }
            }

            for (const auto& name : names) {
                BuildProjectEntry entry;
                entry.name = name;
                entry.file = project_file;
                cache.projects.push_back(std::move(entry));
            }
        }
        cache.build_dir = build_dir_;
        cache.write(output_dir);
    }

    return true;
}

// Determine if solution should use .slnx format based on detected VS installation
bool VcxprojGenerator::should_use_slnx_format() const {
    // Detect latest VS installation
    auto vs_info = VSDetector::detect_latest_vs();

    if (!vs_info.has_value()) {
        // No VS detected - this will cause error later
        return false;
    }

    // Use .slnx for VS 2026+
    return vs_info->year >= 2026;
}

// Determine ToolsVersion based on toolset
std::string VcxprojGenerator::get_tools_version(const std::string& toolset) const {
    if (toolset == "v145" || toolset == "v144") return "18.0";
    if (toolset == "v143") return "Current";
    if (toolset == "v142") return "Current";
    if (toolset == "v141" || toolset == "v141_xp") return "15.0";
    if (toolset == "v140" || toolset == "v140_xp") return "14.0";
    if (toolset == "v120" || toolset == "v120_xp") return "12.0";
    // v110, v100 and other legacy toolsets use MSBuild 4.0
    return "4.0";
}

// Get .sln header version info for a given toolset
VcxprojGenerator::SlnVersionInfo VcxprojGenerator::get_sln_version_info(const std::string& toolset) const {
    if (toolset == "v100" || toolset == "v100_xp") {
        return {"11.00", "# Visual Studio 2010", "", "10.0.40219.1"};
    }
    if (toolset == "v110" || toolset == "v110_xp") {
        return {"12.00", "# Visual Studio 2012", "11.0.61030.0", "10.0.40219.1"};
    }
    if (toolset == "v120" || toolset == "v120_xp") {
        return {"12.00", "# Visual Studio 2013", "12.0.21005.1", "10.0.40219.1"};
    }
    if (toolset == "v140" || toolset == "v140_xp") {
        return {"12.00", "# Visual Studio 14", "14.0.25420.1", "10.0.40219.1"};
    }
    if (toolset == "v141" || toolset == "v141_xp") {
        return {"12.00", "# Visual Studio 15", "15.0.26228.4", "10.0.40219.1"};
    }
    if (toolset == "v142") {
        return {"12.00", "# Visual Studio Version 16", "16.0.28729.10", "10.0.40219.1"};
    }
    if (toolset == "v145" || toolset == "v144") {
        return {"12.00", "# Visual Studio Version 18", "18.0.00000.0", "10.0.40219.1"};
    }
    // Default: VS 2022 (v143)
    return {"12.00", "# Visual Studio Version 17", "17.0.31903.59", "10.0.40219.1"};
}

// Resolve the effective toolset from a Solution
std::string VcxprojGenerator::resolve_solution_toolset(const Solution& solution) const {
    if (!solution.target_toolset.empty()) {
        return solution.target_toolset;
    }
    // Fall back to first project's first configuration toolset
    for (const auto& proj : solution.projects) {
        for (const auto& [key, cfg] : proj.configurations) {
            if (!cfg.platform_toolset.empty()) {
                return cfg.platform_toolset;
            }
        }
    }
    return "v143"; // Default to VS 2022
}

// Register the vcxproj generator
REGISTER_GENERATOR(VcxprojGenerator, "vcxproj");

} // namespace vcxproj
