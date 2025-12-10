#include "vcxproj_generator.hpp"
#define PUGIXML_HEADER_ONLY
#include "pugixml.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <map>

namespace fs = std::filesystem;

namespace vcxproj {

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

bool VcxprojGenerator::generate_vcxproj(const Project& proj_input, const Solution& solution,
                                         const std::string& output_path) {
    // Create mutable copy to preprocess config-level libs into Library elements
    Project project = proj_input;

    // Analyze config-level additional_dependencies and convert library files to Library elements
    // Collect all lib paths from all configs
    std::map<std::string, std::map<std::string, std::string>> lib_basename_to_configs;  // basename -> {config -> full_path}

    for (auto& [config_key, cfg] : project.configurations) {
        std::vector<std::string> remaining_deps;

        for (const auto& dep : cfg.link.additional_dependencies) {
            // Check if this is a library file (has path separators)
            if (dep.find('/') != std::string::npos || dep.find('\\') != std::string::npos) {
                // Extract basename
                std::filesystem::path p(dep);
                std::string basename = p.filename().string();
                lib_basename_to_configs[basename][config_key] = dep;
            } else {
                // System library - keep in additional_dependencies
                remaining_deps.push_back(dep);
            }
        }

        cfg.link.additional_dependencies = remaining_deps;
    }

    // Now create Library elements for libs that appear in multiple configs with different paths
    for (const auto& [basename, config_paths] : lib_basename_to_configs) {
        if (config_paths.size() > 1) {
            // Multiple configs use this library - check if paths differ
            std::set<std::string> unique_paths;
            for (const auto& [cfg_key, path] : config_paths) {
                unique_paths.insert(path);
            }

            if (unique_paths.size() > 1) {
                // Different paths for different configs - create Library elements with ExcludedFromBuild
                for (const auto& path : unique_paths) {
                    LibraryFile lf;
                    lf.path = path;

                    // Mark as excluded for configs that don't use this path
                    for (const auto& [cfg_key, cfg] : project.configurations) {
                        auto it = config_paths.find(cfg_key);
                        if (it == config_paths.end() || it->second != path) {
                            lf.excluded[cfg_key] = true;
                        }
                    }

                    project.libraries.push_back(lf);
                }
            } else {
                // Same path in all configs - just add as Library without exclusions
                LibraryFile lf;
                lf.path = *unique_paths.begin();
                project.libraries.push_back(lf);
            }
        } else {
            // Only one config uses this - add as Library element excluded from other configs
            for (const auto& [cfg_key, path] : config_paths) {
                LibraryFile lf;
                lf.path = path;

                // Mark as excluded for all other configs
                for (const auto& [other_cfg_key, other_cfg] : project.configurations) {
                    if (other_cfg_key != cfg_key) {
                        lf.excluded[other_cfg_key] = true;
                    }
                }

                project.libraries.push_back(lf);
            }
        }
    }

    pugi::xml_document doc;

    // XML declaration
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    // Root Project element
    auto root = doc.append_child("Project");
    root.append_attribute("DefaultTargets") = "Build";
    root.append_attribute("ToolsVersion") = "4.0";
    root.append_attribute("xmlns") = "http://schemas.microsoft.com/developer/msbuild/2003";

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
    globals.append_child("ProjectName").text() = project.name.c_str();
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
            auto node = props.append_child("OutDir");
            node.append_attribute("Condition") = condition.c_str();
            std::string relative_out = make_relative_path(cfg.out_dir, output_path);
            node.text() = relative_out.c_str();
        }
        if (!cfg.int_dir.empty()) {
            auto node = props.append_child("IntDir");
            node.append_attribute("Condition") = condition.c_str();
            std::string relative_int = make_relative_path(cfg.int_dir, output_path);
            node.text() = relative_int.c_str();
        }
        if (!cfg.target_name.empty()) {
            auto node = props.append_child("TargetName");
            node.append_attribute("Condition") = condition.c_str();
            node.text() = cfg.target_name.c_str();
        }
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
        if (!cfg.cl_compile.preprocessor_definitions.empty())
            cl.append_child("PreprocessorDefinitions").text() =
                join_vector(cfg.cl_compile.preprocessor_definitions, ";").c_str();
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

        // PCH - always write, defaulting to NotUsing
        std::string pch_mode = cfg.cl_compile.pch.mode.empty() ? "NotUsing" : cfg.cl_compile.pch.mode;
        cl.append_child("PrecompiledHeader").text() = pch_mode.c_str();
        // Only write header/output if mode is not "NotUsing"
        if (pch_mode != "NotUsing") {
            if (!cfg.cl_compile.pch.header.empty())
                cl.append_child("PrecompiledHeaderFile").text() = cfg.cl_compile.pch.header.c_str();
            if (!cfg.cl_compile.pch.output.empty())
                cl.append_child("PrecompiledHeaderOutputFile").text() = cfg.cl_compile.pch.output.c_str();
        }

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
        if (!cfg.link.base_address.empty())
            link.append_child("BaseAddress").text() = cfg.link.base_address.c_str();
        if (!cfg.link.target_machine.empty())
            link.append_child("TargetMachine").text() = cfg.link.target_machine.c_str();
        if (!cfg.link.error_reporting.empty())
            link.append_child("LinkErrorReporting").text() = cfg.link.error_reporting.c_str();
        if (cfg.link.image_has_safe_exception_handlers)
            link.append_child("ImageHasSafeExceptionHandlers").text() = "true";

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

        // Build events
        if (!cfg.pre_build_event.command.empty()) {
            auto pre_build = item_def.append_child("PreBuildEvent");
            pre_build.append_child("Command").text() = cfg.pre_build_event.command.c_str();
            if (!cfg.pre_build_event.message.empty())
                pre_build.append_child("Message").text() = cfg.pre_build_event.message.c_str();
        }
        // Always add empty PreLinkEvent
        item_def.append_child("PreLinkEvent");
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
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = file_elem.append_child("ExcludedFromBuild");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = "true";
                }
            }

            for (const auto& [config_key, obj_file] : src->settings.object_file) {
                if (!obj_file.empty()) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = file_elem.append_child("ObjectFileName");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = obj_file.c_str();
                }
            }

            // Per-file, per-config AdditionalIncludeDirectories
            for (const auto& [config_key, includes] : src->settings.additional_includes) {
                if (!includes.empty() && config_key != ALL_CONFIGS) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
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

            // Per-file, per-config PreprocessorDefinitions
            for (const auto& [config_key, defines] : src->settings.preprocessor_defines) {
                if (!defines.empty() && config_key != ALL_CONFIGS) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = file_elem.append_child("PreprocessorDefinitions");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = join_vector(defines, ";").c_str();
                }
            }

            // Per-file, per-config AdditionalOptions
            for (const auto& [config_key, options] : src->settings.additional_options) {
                if (!options.empty() && config_key != ALL_CONFIGS) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = file_elem.append_child("AdditionalOptions");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = join_vector(options, " ").c_str();
                }
            }

            // Per-file, per-config PrecompiledHeader settings
            for (const auto& [config_key, pch] : src->settings.pch) {
                if (!pch.mode.empty() && config_key != ALL_CONFIGS) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    if (!pch.mode.empty()) {
                        auto node = file_elem.append_child("PrecompiledHeader");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = pch.mode.c_str();
                    }

                    // Use file-level header if specified, otherwise inherit from project-level
                    // (but only if project-level is not "NotUsing")
                    std::string header_to_use = pch.header;
                    std::string output_to_use = pch.output;
                    if (header_to_use.empty() && project.configurations.count(config_key)) {
                        auto& proj_pch = project.configurations.at(config_key).cl_compile.pch;
                        if (proj_pch.mode != "NotUsing") {
                            header_to_use = proj_pch.header;
                            output_to_use = proj_pch.output;
                        }
                    }

                    // Auto-generate output path if not specified
                    if (output_to_use.empty() && !header_to_use.empty()) {
                        // Convert audio_pch.h -> audio_pch.pch
                        fs::path header_path(header_to_use);
                        std::string pch_name = header_path.stem().string() + ".pch";
                        output_to_use = "$(IntDir)/" + pch_name;
                    }

                    if (!header_to_use.empty()) {
                        auto node = file_elem.append_child("PrecompiledHeaderFile");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = header_to_use.c_str();
                    }
                    if (!output_to_use.empty()) {
                        auto node = file_elem.append_child("PrecompiledHeaderOutputFile");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = output_to_use.c_str();
                    }
                }
            }

            // Per-file, per-config CompileAs
            for (const auto& [config_key, compile_as] : src->settings.compile_as) {
                if (!compile_as.empty() && config_key != ALL_CONFIGS) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    auto node = file_elem.append_child("CompileAs");
                    node.append_attribute("Condition") = condition.c_str();
                    node.text() = compile_as.c_str();
                }
            }

            // Custom build tool
            if (type == FileType::CustomBuild) {
                for (const auto& [config_key, command] : src->custom_command) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    if (!command.empty()) {
                        auto node = file_elem.append_child("Command");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = command.c_str();
                    }
                }
                for (const auto& [config_key, message] : src->custom_message) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    if (!message.empty()) {
                        auto node = file_elem.append_child("Message");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = message.c_str();
                    }
                }
                for (const auto& [config_key, outputs] : src->custom_outputs) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + config_key + "'";
                    if (!outputs.empty()) {
                        auto node = file_elem.append_child("Outputs");
                        node.append_attribute("Condition") = condition.c_str();
                        node.text() = outputs.c_str();
                    }
                }
            }
        }
    }

    // Project references
    if (!project.project_references.empty()) {
        auto ref_group = root.append_child("ItemGroup");
        for (const auto& ref : project.project_references) {
            auto ref_elem = ref_group.append_child("ProjectReference");
            // If the reference is just a project name (no path separators),
            // assume it's in the same directory as this project
            std::string ref_path;
            if (ref.find('/') == std::string::npos && ref.find('\\') == std::string::npos) {
                ref_path = ref + ".vcxproj";
            } else {
                ref_path = make_relative_path(ref + ".vcxproj", output_path);
            }
            ref_elem.append_attribute("Include") = ref_path.c_str();

            // Find the referenced project in the solution to get its GUID
            for (const auto& sol_proj : solution.projects) {
                if (sol_proj.name == ref) {
                    // Add the Project GUID element
                    ref_elem.append_child("Project").text() = ("{" + sol_proj.uuid + "}").c_str();
                    break;
                }
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

    // Projects
    for (const auto& proj : solution.projects) {
        file << "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \""
             << proj.name << "\", \"" << proj.name << ".vcxproj\", \"{"
             << proj.uuid << "}\"\n";
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

bool VcxprojGenerator::generate(const Solution& solution, const std::string& output_dir) {
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

    // Generate project files
    for (const auto& project : solution.projects) {
        fs::path vcxproj_path = fs::path(output_dir) / (project.name + ".vcxproj");

        if (!generate_vcxproj(project, solution, vcxproj_path.string())) {
            std::cerr << "Error: Failed to generate " << vcxproj_path << "\n";
            return false;
        }
    }

    // Generate solution file
    if (!solution.projects.empty()) {
        std::string sln_name = solution.name.empty() ? solution.projects[0].name : solution.name;
        fs::path sln_path = fs::path(output_dir) / (sln_name + ".sln");

        if (!generate_sln(solution, sln_path.string())) {
            std::cerr << "Error: Failed to generate " << sln_path << "\n";
            return false;
        }
    }

    return true;
}

// Register the vcxproj generator
REGISTER_GENERATOR(VcxprojGenerator, "vcxproj");

} // namespace vcxproj
