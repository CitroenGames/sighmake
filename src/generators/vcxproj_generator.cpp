#include "pch.h"
#include "config.hpp"
#include "vcxproj_generator.hpp"
#include "common/vs_detector.hpp"
#include "common/toolset_registry.hpp"
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
        if (i + 1 < str.length() && str[i] == '\\' && str[i + 1] == 'n') {
            result += '\n';
            ++i; // Skip the 'n'
        } else {
            result += str[i];
        }
    }

    return result;
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

        // Calculate relative path
        fs::path relative = fs::relative(file, base);

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

std::string VcxprojGenerator::get_file_type_name(FileType type) {
    switch (type) {
        case FileType::ClCompile: return "ClCompile";
        case FileType::ClInclude: return "ClInclude";
        case FileType::CustomBuild: return "CustomBuild";
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

    // Extension settings
    root.append_child("ImportGroup").append_attribute("Label") = "ExtensionSettings";

    // Property sheets
    for (const auto& config_key : solution.get_config_keys()) {
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
        std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";

        if (!cfg.out_dir.empty()) {
            // Validate that out_dir is an absolute path (as expected from buildscript_parser)
            try {
                fs::path out_path(cfg.out_dir);
                if (!out_path.is_absolute()) {
                    std::cerr << "Warning: OutDir '" << cfg.out_dir
                             << "' is not absolute for configuration " << config_key
                             << " in project " << project.name << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid OutDir path '" << cfg.out_dir
                         << "' for configuration " << config_key
                         << " in project " << project.name << ": " << e.what() << "\n";
                return false;
            }

            auto node = props.append_child("OutDir");
            node.append_attribute("Condition") = condition.c_str();
            // Make relative to vcxproj output location
            std::string relative_out = make_relative_path(cfg.out_dir, output_path);
            // Ensure trailing slash for MSBuild
            if (!relative_out.empty() && relative_out.back() != '\\') {
                relative_out += '\\';
            }
            node.text() = relative_out.c_str();
        }
        if (!cfg.int_dir.empty()) {
            // Validate that int_dir is an absolute path (as expected from buildscript_parser)
            try {
                fs::path int_path(cfg.int_dir);
                if (!int_path.is_absolute()) {
                    std::cerr << "Warning: IntDir '" << cfg.int_dir
                             << "' is not absolute for configuration " << config_key
                             << " in project " << project.name << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid IntDir path '" << cfg.int_dir
                         << "' for configuration " << config_key
                         << " in project " << project.name << ": " << e.what() << "\n";
                return false;
            }

            auto node = props.append_child("IntDir");
            node.append_attribute("Condition") = condition.c_str();
            // Make relative to vcxproj output location
            std::string relative_int = make_relative_path(cfg.int_dir, output_path);
            // Ensure trailing slash for MSBuild
            if (!relative_int.empty() && relative_int.back() != '\\') {
                relative_int += '\\';
            }
            node.text() = relative_int.c_str();
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
        auto [config_name, platform_name] = parse_config_key(config_key); // Use the existing helper

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
            cl.append_child("WarningLevel").text() = cfg.cl_compile.warning_level.c_str();
        if (!cfg.cl_compile.disable_specific_warnings.empty())
            cl.append_child("DisableSpecificWarnings").text() =
                join_vector(cfg.cl_compile.disable_specific_warnings, ";").c_str();
        if (!cfg.cl_compile.language_standard.empty())
            cl.append_child("LanguageStandard").text() = cfg.cl_compile.language_standard.c_str();
        if (!cfg.cl_compile.exception_handling.empty())
            cl.append_child("ExceptionHandling").text() = cfg.cl_compile.exception_handling.c_str();
        if (!cfg.cl_compile.enhanced_instruction_set.empty())
            cl.append_child("EnableEnhancedInstructionSet").text() = cfg.cl_compile.enhanced_instruction_set.c_str();
        if (!cfg.cl_compile.floating_point_model.empty())
            cl.append_child("FloatingPointModel").text() = cfg.cl_compile.floating_point_model.c_str();
        if (!cfg.cl_compile.additional_options.empty())
            cl.append_child("AdditionalOptions").text() = cfg.cl_compile.additional_options.c_str();
        if (cfg.cl_compile.function_level_linking)
            cl.append_child("FunctionLevelLinking").text() = "true";
        if (cfg.cl_compile.intrinsic_functions)
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
            deps_str += join_vector(cfg.link.additional_dependencies, ";");
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
        if (!cfg.link.additional_options.empty())
            link.append_child("AdditionalOptions").text() = cfg.link.additional_options.c_str();
        if (cfg.link.enable_comdat_folding)
            link.append_child("EnableCOMDATFolding").text() = "true";
        if (cfg.link.optimize_references)
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
                        node.text() = join_vector(options, " ").c_str();
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

            // Per-file, per-config CompileAs
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
            for (const auto& sol_proj : solution.projects) {
                if (sol_proj.name == dep.name) {
                    found = true;

                    // Calculate correct relative path using buildscript_path
                    if (!sol_proj.buildscript_path.empty()) {
                        fs::path dep_vcxproj_path = fs::path(sol_proj.buildscript_path) / (sol_proj.name + GENERATED_VCXPROJ);
                        ref_path = make_relative_path(dep_vcxproj_path.string(), output_path);
                    } else {
                        ref_path = dep.name + GENERATED_VCXPROJ;
                    }

                    ref_elem.append_attribute("Include") = ref_path.c_str();

                    // Add the Project GUID element
                    ref_elem.append_child("Project").text() = ("{" + sol_proj.uuid + "}").c_str();
                    break;
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

    // Extension targets
    root.append_child("ImportGroup").append_attribute("Label") = "ExtensionTargets";

    // Save to file
    return doc.save_file(output_path.c_str(), "  ", pugi::format_default | pugi::format_write_bom);
}

bool VcxprojGenerator::generate_sln(const Solution& solution, const std::string& output_path) {
    std::ofstream file(output_path);
    if (!file.is_open()) {
        return false;
    }

    // Header
    file << "\xEF\xBB\xBF\n"; // UTF-8 BOM
    file << "Microsoft Visual Studio Solution File, Format Version 12.00\n";
    file << "# Visual Studio Version 17\n";
    file << "VisualStudioVersion = 17.0.31903.59\n";
    file << "MinimumVisualStudioVersion = 10.0.40219.1\n";

    // Build a map from project name to UUID for dependency resolution
    std::map<std::string, std::string> name_to_uuid;
    for (const auto& proj : solution.projects) {
        name_to_uuid[proj.name] = proj.uuid;
    }

    // Projects
    for (const auto& proj : solution.projects) {
#if PROJ_SEPERATOR
        // Compute relative path to vcxproj
        std::string vcxproj_path;
        if (!proj.buildscript_path.empty()) {
            // vcxproj is at buildscript_path/name_.vcxproj
            fs::path proj_path = fs::path(proj.buildscript_path) / (proj.name + GENERATED_VCXPROJ);
            fs::path sln_path(output_path);
            try {
                vcxproj_path = fs::relative(proj_path, sln_path.parent_path()).string();
                // Convert to backslashes for Windows
                std::replace(vcxproj_path.begin(), vcxproj_path.end(), '/', '\\');
            } catch (...) {
                vcxproj_path = proj.name + GENERATED_VCXPROJ;
            }
        } else {
            vcxproj_path = proj.name + GENERATED_VCXPROJ;
        }
#else
        std::string vcxproj_path = proj.name + GENERATED_VCXPROJ;
#endif
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
                if (name_to_uuid.count(dep.name)) {
                    std::string dep_uuid = name_to_uuid[dep.name];
                    file << "\t\t{" << dep_uuid << "} = {" << dep_uuid << "}\n";
                }
            }
            file << "\tEndProjectSection\n";
        }

        file << "EndProject\n";
    }

    // Global section
    file << "Global\n";

    // Solution configuration platforms
    file << "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
    for (const auto& config : solution.configurations) {
        for (const auto& platform : solution.platforms) {
            std::string key = config + "|" + platform;
            file << "\t\t" << key << " = " << key << "\n";
        }
    }
    file << "\tEndGlobalSection\n";

    // Project configuration platforms
    file << "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
    for (const auto& proj : solution.projects) {
        for (const auto& config : solution.configurations) {
            for (const auto& platform : solution.platforms) {
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
        auto plat_elem = configs.append_child("Platform");
        plat_elem.append_attribute("Name") = platform.c_str();
    }

    // Projects
    for (const auto& proj : solution.projects) {
        // Compute relative path to vcxproj
        std::string vcxproj_path;
#if PROJ_SEPERATOR
        if (!proj.buildscript_path.empty()) {
            fs::path proj_path = fs::path(proj.buildscript_path) / (proj.name + GENERATED_VCXPROJ);
            fs::path sln_path(output_path);
            try {
                vcxproj_path = fs::relative(proj_path, sln_path.parent_path()).string();
                std::replace(vcxproj_path.begin(), vcxproj_path.end(), '/', '\\');
            } catch (...) {
                vcxproj_path = proj.name + GENERATED_VCXPROJ;
            }
        } else {
            vcxproj_path = proj.name + GENERATED_VCXPROJ;
        }
#else
        vcxproj_path = proj.name + GENERATED_VCXPROJ;
#endif

        auto project = root.append_child("Project");
        project.append_attribute("Path") = vcxproj_path.c_str();
        project.append_attribute("Type") = "8bc9ceb8-8b4a-11d0-8d11-00a0c91bc942"; // C++ GUID
        project.append_attribute("Id") = proj.uuid.c_str();

        // Add build dependencies
        for (const auto& dep : proj.project_references) {
            // Skip INTERFACE dependencies - they don't get linked
            if (dep.visibility == DependencyVisibility::INTERFACE) {
                continue;
            }

            // Find the referenced project path
            std::string dep_path;
            for (const auto& dep_proj : solution.projects) {
                if (dep_proj.name == dep.name) {
#if PROJ_SEPERATOR
                    if (!dep_proj.buildscript_path.empty()) {
                        fs::path dep_proj_path = fs::path(dep_proj.buildscript_path) / (dep_proj.name + GENERATED_VCXPROJ);
                        fs::path sln_path(output_path);
                        try {
                            dep_path = fs::relative(dep_proj_path, sln_path.parent_path()).string();
                            std::replace(dep_path.begin(), dep_path.end(), '/', '\\');
                        } catch (...) {
                            dep_path = dep_proj.name + GENERATED_VCXPROJ;
                        }
                    } else {
                        dep_path = dep_proj.name + GENERATED_VCXPROJ;
                    }
#else
                    dep_path = dep_proj.name + GENERATED_VCXPROJ;
#endif
                    break;
                }
            }

            if (!dep_path.empty()) {
                auto dep_elem = project.append_child("BuildDependency");
                dep_elem.append_attribute("Project") = dep_path.c_str();
            }
        }
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
    bool already_logged = false;

    for (auto& proj : solution.projects) {
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
        fs::path vcxproj_path;

#if PROJ_SEPERATOR
        // Generate at buildscript location with underscore suffix
        std::string filename = project.name + GENERATED_VCXPROJ;
        if (!project.buildscript_path.empty()) {
            vcxproj_path = fs::path(project.buildscript_path) / filename;
        } else {
            vcxproj_path = fs::path(output_dir) / filename;
        }
#else
        // Generate without underscore at buildscript location
        std::string filename = project.name + GENERATED_VCXPROJ;
        if (!project.buildscript_path.empty()) {
            vcxproj_path = fs::path(project.buildscript_path) / filename;
        } else {
            vcxproj_path = fs::path(output_dir) / filename;
        }
#endif

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
            fs::path slnx_path;
            slnx_path = fs::path(output_dir) / (sln_name + GENERATED_SLNX);

            std::cout << "Generating .slnx format for Visual Studio " << vs_info->year << "...\n";

            if (!generate_slnx(solution, slnx_path.string())) {
                std::cerr << "Error: Failed to generate " << slnx_path << "\n";
                return false;
            }
        } else {
            // Generate traditional .sln for older toolsets
            fs::path sln_path;
            sln_path = fs::path(output_dir) / (sln_name + GENERATED_SLN);

            if (!generate_sln(solution, sln_path.string())) {
                std::cerr << "Error: Failed to generate " << sln_path << "\n";
                return false;
            }
        }
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
    // MSVC 2026 uses ToolsVersion 18.0
    if (toolset == "v145" || toolset == "v144") {
        return "18.0";
    }
    // Legacy toolsets use 4.0
    return "4.0";
}

// Register the vcxproj generator
REGISTER_GENERATOR(VcxprojGenerator, "vcxproj");

} // namespace vcxproj
