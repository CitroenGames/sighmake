#include "vcxproj_reader.hpp"
#define PUGIXML_HEADER_ONLY
#include "pugixml.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace vcxproj {

std::string VcxprojReader::parse_condition(const std::string& condition) {
    // Parse condition like "'$(Configuration)|$(Platform)'=='Debug|Win32'"
    std::regex re(R"('==\s*'([^']+)')");
    std::smatch match;
    if (std::regex_search(condition, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

std::pair<std::string, std::string> VcxprojReader::parse_config_platform(const std::string& label) {
    size_t pos = label.find('|');
    if (pos != std::string::npos) {
        return {label.substr(0, pos), label.substr(pos + 1)};
    }
    return {label, "Win32"};
}

Project VcxprojReader::read_vcxproj(const std::string& filepath) {
    Project project;
    pugi::xml_document doc;

    pugi::xml_parse_result result = doc.load_file(filepath.c_str());
    if (!result) {
        throw std::runtime_error("Failed to parse vcxproj: " + std::string(result.description()));
    }

    auto root = doc.child("Project");
    if (!root) {
        throw std::runtime_error("Invalid vcxproj file: no Project root element");
    }

    // Parse Globals
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string label = prop_group.attribute("Label").as_string();
        if (label == "Globals") {
            project.name = prop_group.child_value("ProjectName");
            std::string guid = prop_group.child_value("ProjectGuid");
            // Remove braces from GUID
            if (!guid.empty() && guid.front() == '{') {
                guid = guid.substr(1, guid.length() - 2);
            }
            project.uuid = guid;
            project.root_namespace = prop_group.child_value("RootNamespace");
            project.ignore_warn_compile_duplicated_filename =
                prop_group.child("IgnoreWarnCompileDuplicatedFilename").text().as_bool();
            break;
        }
    }

    // Parse configurations
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string label = prop_group.attribute("Label").as_string();
        std::string condition = prop_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (label == "Configuration" && !config_key.empty()) {
            auto& cfg = project.configurations[config_key];
            cfg.config_type = prop_group.child_value("ConfigurationType");
            cfg.platform_toolset = prop_group.child_value("PlatformToolset");
            cfg.windows_target_platform_version = prop_group.child_value("WindowsTargetPlatformVersion");
            cfg.character_set = prop_group.child_value("CharacterSet");
            cfg.use_debug_libraries = prop_group.child("UseDebugLibraries").text().as_bool();
            cfg.use_of_mfc = prop_group.child_value("UseOfMfc");
            cfg.use_of_atl = prop_group.child_value("UseOfAtl");
        }
    }

    // Parse PropertyGroup settings (OutDir, IntDir, LinkIncremental, etc.)
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string condition = prop_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (!config_key.empty() && project.configurations.count(config_key)) {
            auto& cfg = project.configurations[config_key];

            if (auto node = prop_group.child("OutDir"))
                cfg.out_dir = node.text().as_string();
            if (auto node = prop_group.child("IntDir"))
                cfg.int_dir = node.text().as_string();
            if (auto node = prop_group.child("TargetName"))
                cfg.target_name = node.text().as_string();
            if (auto node = prop_group.child("TargetExt"))
                cfg.target_ext = node.text().as_string();
            if (auto node = prop_group.child("LinkIncremental"))
                cfg.link_incremental = node.text().as_bool();
            if (auto node = prop_group.child("ExecutablePath"))
                cfg.executable_path = node.text().as_string();
            if (auto node = prop_group.child("GenerateManifest"))
                cfg.generate_manifest = node.text().as_bool();
            if (auto node = prop_group.child("PreBuildEventUseInBuild"))
                cfg.pre_build_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PreLinkEventUseInBuild"))
                cfg.pre_link_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PostBuildEventUseInBuild"))
                cfg.post_build_event.use_in_build = node.text().as_bool();
        }
    }

    // Parse ItemDefinitionGroup (compiler, linker, etc. settings)
    for (auto item_def : root.children("ItemDefinitionGroup")) {
        std::string condition = item_def.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (config_key.empty() || !project.configurations.count(config_key)) continue;

        auto& cfg = project.configurations[config_key];

        // ClCompile settings
        if (auto cl = item_def.child("ClCompile")) {
            auto& settings = cfg.cl_compile;

            #define READ_TEXT(name, field) if (auto n = cl.child(name)) settings.field = n.text().as_string()
            #define READ_BOOL(name, field) if (auto n = cl.child(name)) settings.field = n.text().as_bool()
            #define READ_VECTOR(name, field) if (auto n = cl.child(name)) { \
                std::string val = n.text().as_string(); \
                std::istringstream ss(val); std::string item; \
                while (std::getline(ss, item, ';')) { \
                    if (!item.empty()) settings.field.push_back(item); \
                } \
            }

            READ_TEXT("Optimization", optimization);
            READ_TEXT("InlineFunctionExpansion", inline_function_expansion);
            READ_BOOL("IntrinsicFunctions", intrinsic_functions);
            READ_TEXT("FavorSizeOrSpeed", favor_size_or_speed);
            READ_VECTOR("AdditionalIncludeDirectories", additional_include_directories);
            READ_VECTOR("PreprocessorDefinitions", preprocessor_definitions);
            READ_BOOL("StringPooling", string_pooling);
            READ_BOOL("MinimalRebuild", minimal_rebuild);
            READ_TEXT("ExceptionHandling", exception_handling);
            READ_TEXT("BasicRuntimeChecks", basic_runtime_checks);
            READ_TEXT("RuntimeLibrary", runtime_library);
            READ_BOOL("BufferSecurityCheck", buffer_security_check);
            READ_BOOL("FunctionLevelLinking", function_level_linking);
            READ_TEXT("EnableEnhancedInstructionSet", enhanced_instruction_set);
            READ_TEXT("FloatingPointModel", floating_point_model);
            READ_BOOL("ForceConformanceInForLoopScope", force_conformance_in_for_loop_scope);
            READ_BOOL("RuntimeTypeInfo", runtime_type_info);
            READ_TEXT("PrecompiledHeader", pch.mode);
            READ_TEXT("PrecompiledHeaderFile", pch.header);
            READ_TEXT("PrecompiledHeaderOutputFile", pch.output);
            READ_TEXT("AssemblerListingLocation", assembler_listing_location);
            READ_TEXT("ObjectFileName", object_file_name);
            READ_TEXT("ProgramDataBaseFileName", program_database_file_name);
            READ_BOOL("GenerateXMLDocumentationFiles", generate_xml_documentation_files);
            READ_BOOL("BrowseInformation", browse_information);
            READ_TEXT("BrowseInformationFile", browse_information_file);
            READ_TEXT("WarningLevel", warning_level);
            READ_TEXT("DebugInformationFormat", debug_information_format);
            READ_TEXT("CompileAs", compile_as);
            READ_VECTOR("DisableSpecificWarnings", disable_specific_warnings);
            READ_BOOL("MultiProcessorCompilation", multi_processor_compilation);
            READ_TEXT("ErrorReporting", error_reporting);
            READ_TEXT("AdditionalOptions", additional_options);
            READ_TEXT("LanguageStandard", language_standard);

            #undef READ_TEXT
            #undef READ_BOOL
            #undef READ_VECTOR
        }

        // Link settings
        if (auto link = item_def.child("Link")) {
            auto& settings = cfg.link;

            #define READ_TEXT(name, field) if (auto n = link.child(name)) settings.field = n.text().as_string()
            #define READ_BOOL(name, field) if (auto n = link.child(name)) settings.field = n.text().as_bool()
            #define READ_VECTOR(name, field) if (auto n = link.child(name)) { \
                std::string val = n.text().as_string(); \
                std::istringstream ss(val); std::string item; \
                while (std::getline(ss, item, ';')) { \
                    if (!item.empty()) settings.field.push_back(item); \
                } \
            }

            READ_TEXT("ShowProgress", show_progress);
            READ_TEXT("OutputFile", output_file);
            READ_BOOL("SuppressStartupBanner", suppress_startup_banner);
            READ_VECTOR("AdditionalDependencies", additional_dependencies);
            READ_VECTOR("AdditionalLibraryDirectories", additional_library_directories);
            READ_VECTOR("IgnoreSpecificDefaultLibraries", ignore_specific_default_libraries);
            READ_BOOL("GenerateDebugInformation", generate_debug_info);
            READ_TEXT("ProgramDatabaseFile", program_database_file);
            READ_TEXT("SubSystem", sub_system);
            READ_BOOL("OptimizeReferences", optimize_references);
            READ_BOOL("EnableCOMDATFolding", enable_comdat_folding);
            READ_TEXT("BaseAddress", base_address);
            READ_TEXT("TargetMachine", target_machine);
            READ_TEXT("LinkErrorReporting", error_reporting);
            READ_BOOL("ImageHasSafeExceptionHandlers", image_has_safe_exception_handlers);
            READ_TEXT("AdditionalOptions", additional_options);

            #undef READ_TEXT
            #undef READ_BOOL
            #undef READ_VECTOR
        }

        // ResourceCompile settings
        if (auto rc = item_def.child("ResourceCompile")) {
            auto& settings = cfg.resource_compile;

            if (auto n = rc.child("PreprocessorDefinitions")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) settings.preprocessor_definitions.push_back(item);
                }
            }
            if (auto n = rc.child("Culture"))
                settings.culture = n.text().as_string();
            if (auto n = rc.child("AdditionalIncludeDirectories")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) settings.additional_include_directories.push_back(item);
                }
            }
        }

        // Build events
        if (auto pre_build = item_def.child("PreBuildEvent")) {
            if (auto n = pre_build.child("Command"))
                cfg.pre_build_event.command = n.text().as_string();
            if (auto n = pre_build.child("Message"))
                cfg.pre_build_event.message = n.text().as_string();
        }
        if (auto pre_link = item_def.child("PreLinkEvent")) {
            if (auto n = pre_link.child("Command"))
                cfg.pre_link_event.command = n.text().as_string();
            if (auto n = pre_link.child("Message"))
                cfg.pre_link_event.message = n.text().as_string();
        }
        if (auto post_build = item_def.child("PostBuildEvent")) {
            if (auto n = post_build.child("Command"))
                cfg.post_build_event.command = n.text().as_string();
            if (auto n = post_build.child("Message"))
                cfg.post_build_event.message = n.text().as_string();
        }
    }

    // Parse source files
    for (auto item_group : root.children("ItemGroup")) {
        for (auto file_elem : item_group.children()) {
            std::string elem_name = file_elem.name();
            if (elem_name != "ClCompile" && elem_name != "ClInclude" &&
                elem_name != "ResourceCompile" && elem_name != "CustomBuild" &&
                elem_name != "None") {
                continue;
            }

            SourceFile src;
            src.path = file_elem.attribute("Include").as_string();

            if (elem_name == "ClCompile") src.type = FileType::ClCompile;
            else if (elem_name == "ClInclude") src.type = FileType::ClInclude;
            else if (elem_name == "ResourceCompile") src.type = FileType::ResourceCompile;
            else if (elem_name == "CustomBuild") src.type = FileType::CustomBuild;
            else src.type = FileType::None;

            // Parse per-file settings
            for (auto child : file_elem.children()) {
                std::string name = child.name();
                std::string condition = child.attribute("Condition").as_string();
                std::string config_key = condition.empty() ? ALL_CONFIGS : parse_condition(condition);

                if (name == "ExcludedFromBuild") {
                    src.settings.excluded[config_key] = child.text().as_bool();
                } else if (name == "ObjectFileName") {
                    src.settings.object_file[config_key] = child.text().as_string();
                } else if (name == "AdditionalIncludeDirectories") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        if (!item.empty()) src.settings.additional_includes[config_key].push_back(item);
                    }
                } else if (name == "PreprocessorDefinitions") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        if (!item.empty()) src.settings.preprocessor_defines[config_key].push_back(item);
                    }
                } else if (name == "PrecompiledHeader") {
                    src.settings.pch[config_key].mode = child.text().as_string();
                } else if (name == "PrecompiledHeaderFile") {
                    src.settings.pch[config_key].header = child.text().as_string();
                } else if (name == "PrecompiledHeaderOutputFile") {
                    src.settings.pch[config_key].output = child.text().as_string();
                } else if (name == "CompileAs") {
                    src.settings.compile_as[config_key] = child.text().as_string();
                } else if (name == "Command") {
                    src.custom_command[config_key] = child.text().as_string();
                } else if (name == "Message") {
                    src.custom_message[config_key] = child.text().as_string();
                } else if (name == "Outputs") {
                    src.custom_outputs[config_key] = child.text().as_string();
                } else if (name == "AdditionalInputs") {
                    src.custom_inputs[config_key] = child.text().as_string();
                }
            }

            project.sources.push_back(src);
        }

        // Parse project references
        for (auto ref : item_group.children("ProjectReference")) {
            std::string include = ref.attribute("Include").as_string();
            // Extract project name from path (remove .vcxproj extension)
            fs::path p(include);
            std::string proj_name = p.stem().string();
            project.project_references.push_back(proj_name);
        }
    }

    return project;
}

// Solution parser implementation
Solution SlnReader::read_sln(const std::string& filepath) {
    Solution solution;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open solution file: " + filepath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Parse solution configurations
    std::regex config_re(R"((\w+)\|(\w+)\s*=\s*(\w+)\|(\w+))");
    std::smatch match;
    std::set<std::string> configs, platforms;

    auto it = content.cbegin();
    while (std::regex_search(it, content.cend(), match, config_re)) {
        configs.insert(match[1].str());
        platforms.insert(match[2].str());
        it = match.suffix().first;
    }

    solution.configurations = std::vector<std::string>(configs.begin(), configs.end());
    solution.platforms = std::vector<std::string>(platforms.begin(), platforms.end());

    if (solution.configurations.empty()) {
        solution.configurations = {"Debug", "Release"};
    }
    if (solution.platforms.empty()) {
        solution.platforms = {"Win32", "x64"};
    }

    // Parse projects
    auto projects = parse_projects(content);

    std::cout << "Found " << projects.size() << " project(s) in solution\n";

    fs::path sln_dir = fs::path(filepath).parent_path();

    for (const auto& proj_info : projects) {
        fs::path proj_path = sln_dir / proj_info.path;

        if (fs::exists(proj_path)) {
            try {
                VcxprojReader reader;
                Project proj = reader.read_vcxproj(proj_path.string());
                proj.name = proj_info.name;
                proj.uuid = proj_info.uuid;
                proj.vcxproj_path = proj_info.path;  // Store relative path from .sln
                solution.projects.push_back(proj);
            } catch (const std::exception& e) {
                std::cerr << "Warning: Failed to read project " << proj_info.name
                          << ": " << e.what() << "\n";
            }
        } else {
            std::cerr << "Warning: Project file not found: " << proj_path.string() << "\n";
        }
    }

    // Extract solution name from filename
    solution.name = fs::path(filepath).stem().string();
    solution.uuid = generate_uuid();

    return solution;
}

std::vector<SlnReader::SlnProject> SlnReader::parse_projects(const std::string& content) {
    std::vector<SlnProject> projects;
    int total_matches = 0;
    int vcxproj_count = 0;
    int skipped_count = 0;

    // Parse project lines: Project("{...}") = "ProjectName", "path\to\project.vcxproj", "{GUID}"
    std::regex proj_re(R"xxx(Project\s*\("[^"]+"\)\s*=\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"\{([^}]+)\}")xxx");
    std::smatch match;

    auto it = content.cbegin();
    while (std::regex_search(it, content.cend(), match, proj_re)) {
        total_matches++;
        SlnProject proj;
        proj.name = match[1].str();
        proj.path = match[2].str();
        proj.uuid = match[3].str();

        // Only include vcxproj files (filter out solution folders and other project types)
        if (proj.path.find(".vcxproj") != std::string::npos) {
            projects.push_back(proj);
            vcxproj_count++;
        } else {
            std::cout << "  Skipping: " << proj.name << " (" << proj.path << ")\n";
            skipped_count++;
        }

        it = match.suffix().first;
    }

    std::cout << "Total entries parsed: " << total_matches << " (vcxproj: " << vcxproj_count << ", skipped: " << skipped_count << ")\n";

    return projects;
}

// Buildscript writer implementation
std::string BuildscriptWriter::join_vector(const std::vector<std::string>& vec, const std::string& sep) {
    if (vec.empty()) return "";
    std::string result = vec[0];
    for (size_t i = 1; i < vec.size(); i++) {
        result += sep + vec[i];
    }
    return result;
}

std::string BuildscriptWriter::format_value(const std::string& value) {
    // Check if value contains newlines - if so, use multiline format
    if (value.find('\n') != std::string::npos) {
        std::string result = "\"\"\"\n";
        for (char c : value) {
            if (c == '\r') {
                // Skip carriage returns
            } else {
                result += c;
            }
        }
        if (!result.empty() && result.back() != '\n') {
            result += '\n';
        }
        result += "\"\"\"";
        return result;
    }
    // Single-line value, return as-is
    return value;
}

bool BuildscriptWriter::write_buildscript(const Project& project, const std::string& filepath,
                                         const std::vector<std::string>& configurations,
                                         const std::vector<std::string>& platforms) {
    // Suppress unused parameter warnings (these may be used in future enhancements)
    (void)configurations;
    (void)platforms;

    std::ofstream out(filepath);
    if (!out.is_open()) {
        return false;
    }

    out << "# Generated buildscript from " << project.name << ".vcxproj\n";
    out << "# You may need to adjust paths and settings\n\n";

    out << "[project:" << project.name << "]\n";

    // Determine project type from first configuration
    if (!project.configurations.empty()) {
        auto& first_cfg = project.configurations.begin()->second;
        out << "type = ";
        if (first_cfg.config_type == "Application") out << "exe\n";
        else if (first_cfg.config_type == "StaticLibrary") out << "lib\n";
        else if (first_cfg.config_type == "DynamicLibrary") out << "dll\n";
        else out << first_cfg.config_type << "\n";
    }

    // Source files
    std::vector<std::string> cpp_files, h_files, rc_files;
    for (const auto& src : project.sources) {
        if (src.type == FileType::ClCompile) {
            cpp_files.push_back(src.path);
        } else if (src.type == FileType::ClInclude) {
            h_files.push_back(src.path);
        } else if (src.type == FileType::ResourceCompile) {
            rc_files.push_back(src.path);
        }
    }

    if (!cpp_files.empty()) {
        out << "sources = " << join_vector(cpp_files, ", ") << "\n";
    }
    if (!h_files.empty()) {
        out << "headers = " << join_vector(h_files, ", ") << "\n";
    }
    if (!rc_files.empty()) {
        out << "resources = " << join_vector(rc_files, ", ") << "\n";
    }

    // Project references
    if (!project.project_references.empty()) {
        out << "depends = " << join_vector(project.project_references, ", ") << "\n";
    }

    // Write common settings (from first config as default)
    if (!project.configurations.empty()) {
        auto& first_cfg = project.configurations.begin()->second;
        auto& cl = first_cfg.cl_compile;
        auto& link = first_cfg.link;

        if (!first_cfg.platform_toolset.empty())
            out << "toolset = " << first_cfg.platform_toolset << "\n";
        if (!first_cfg.windows_target_platform_version.empty())
            out << "windows_sdk = " << first_cfg.windows_target_platform_version << "\n";
        if (!first_cfg.character_set.empty())
            out << "charset = " << first_cfg.character_set << "\n";
        if (!first_cfg.use_of_mfc.empty() && first_cfg.use_of_mfc != "false")
            out << "use_of_mfc = " << first_cfg.use_of_mfc << "\n";
        if (!first_cfg.use_of_atl.empty() && first_cfg.use_of_atl != "false")
            out << "use_of_atl = " << first_cfg.use_of_atl << "\n";

        // Compiler settings
        if (!cl.additional_include_directories.empty())
            out << "includes = " << join_vector(cl.additional_include_directories, ", ") << "\n";
        if (!cl.preprocessor_definitions.empty())
            out << "defines = " << join_vector(cl.preprocessor_definitions, ", ") << "\n";
        if (!cl.language_standard.empty())
            out << "std = " << cl.language_standard << "\n";
        if (!cl.warning_level.empty())
            out << "warning_level = " << cl.warning_level << "\n";
        if (!cl.exception_handling.empty())
            out << "exceptions = " << cl.exception_handling << "\n";
        if (!cl.runtime_type_info)
            out << "rtti = false\n";
        if (cl.multi_processor_compilation)
            out << "multiprocessor = true\n";
        if (!cl.enhanced_instruction_set.empty())
            out << "sse = " << cl.enhanced_instruction_set << "\n";
        if (!cl.floating_point_model.empty())
            out << "floating_point = " << cl.floating_point_model << "\n";
        if (cl.string_pooling)
            out << "string_pooling = true\n";
        if (!cl.favor_size_or_speed.empty())
            out << "favor = " << cl.favor_size_or_speed << "\n";
        if (!cl.inline_function_expansion.empty())
            out << "inline_expansion = " << cl.inline_function_expansion << "\n";
        if (!cl.buffer_security_check)
            out << "buffer_security = false\n";
        if (!cl.compile_as.empty())
            out << "compile_as = " << cl.compile_as << "\n";

        // Linker settings
        if (!link.sub_system.empty())
            out << "subsystem = " << link.sub_system << "\n";
        if (!link.additional_dependencies.empty())
            out << "libs = " << join_vector(link.additional_dependencies, ", ") << "\n";
        if (!link.additional_library_directories.empty())
            out << "libdirs = " << join_vector(link.additional_library_directories, ", ") << "\n";
    }

    // Write config-specific settings
    out << "\n# Configuration-specific settings\n";
    for (const auto& [config_key, cfg] : project.configurations) {
        out << "\n[config:" << config_key << "]\n";

        if (!cfg.out_dir.empty())
            out << "outdir = " << cfg.out_dir << "\n";
        if (!cfg.int_dir.empty())
            out << "intdir = " << cfg.int_dir << "\n";
        if (!cfg.cl_compile.optimization.empty())
            out << "optimization = " << cfg.cl_compile.optimization << "\n";
        if (!cfg.cl_compile.runtime_library.empty())
            out << "runtime_library = " << cfg.cl_compile.runtime_library << "\n";
        if (!cfg.cl_compile.debug_information_format.empty())
            out << "debug_info = " << cfg.cl_compile.debug_information_format << "\n";
        if (cfg.link.generate_debug_info)
            out << "generate_debug_info = true\n";
        if (cfg.link_incremental)
            out << "link_incremental = true\n";

        // Build events
        if (!cfg.pre_build_event.command.empty()) {
            out << "prebuild = " << format_value(cfg.pre_build_event.command) << "\n";
            if (!cfg.pre_build_event.message.empty())
                out << "prebuild_message = " << format_value(cfg.pre_build_event.message) << "\n";
        }
        if (!cfg.post_build_event.command.empty()) {
            out << "postbuild = " << format_value(cfg.post_build_event.command) << "\n";
            if (!cfg.post_build_event.message.empty())
                out << "postbuild_message = " << format_value(cfg.post_build_event.message) << "\n";
        }
    }

    // Write per-file settings
    for (const auto& src : project.sources) {
        bool has_settings = !src.settings.pch.empty() ||
                           !src.settings.additional_includes.empty() ||
                           !src.settings.preprocessor_defines.empty() ||
                           !src.custom_command.empty();

        if (has_settings) {
            out << "\n[file:" << src.path << "]\n";

            for (const auto& [config_key, pch] : src.settings.pch) {
                if (!pch.mode.empty())
                    out << "pch[" << config_key << "] = " << pch.mode << "\n";
                if (!pch.header.empty())
                    out << "pch_header[" << config_key << "] = " << pch.header << "\n";
            }

            for (const auto& [config_key, includes] : src.settings.additional_includes) {
                if (!includes.empty())
                    out << "includes[" << config_key << "] = " << join_vector(includes, ", ") << "\n";
            }

            for (const auto& [config_key, defines] : src.settings.preprocessor_defines) {
                if (!defines.empty())
                    out << "defines[" << config_key << "] = " << join_vector(defines, ", ") << "\n";
            }

            for (const auto& [config_key, cmd] : src.custom_command) {
                if (!cmd.empty()) {
                    out << "custom_command[" << config_key << "] = " << cmd << "\n";
                    if (src.custom_outputs.count(config_key))
                        out << "custom_outputs[" << config_key << "] = " << src.custom_outputs.at(config_key) << "\n";
                    if (src.custom_message.count(config_key))
                        out << "custom_message[" << config_key << "] = " << src.custom_message.at(config_key) << "\n";
                }
            }
        }
    }

    out.close();
    return true;
}

bool BuildscriptWriter::write_solution_buildscripts(const Solution& solution, const std::string& base_dir) {
    fs::path sln_base = fs::path(base_dir);
    std::vector<std::string> buildscript_paths;

    // Generate individual buildscripts next to each vcxproj
    for (const auto& project : solution.projects) {
        // Determine output path: same directory as the vcxproj file
        fs::path vcxproj_rel_path(project.vcxproj_path);
        fs::path buildscript_path = sln_base / vcxproj_rel_path.parent_path() / (project.name + ".buildscript");

        std::cout << "  Generating: " << buildscript_path.string() << "\n";

        if (!write_buildscript(project, buildscript_path.string(),
                              solution.configurations, solution.platforms)) {
            std::cerr << "Error: Failed to write " << buildscript_path.string() << "\n";
            return false;
        }

        // Store relative path from solution directory for include directive
        fs::path rel_include_path = vcxproj_rel_path.parent_path() / (project.name + ".buildscript");
        buildscript_paths.push_back(rel_include_path.string());
    }

    // Generate root buildscript that includes all individual buildscripts
    fs::path root_buildscript = sln_base / (solution.name + ".buildscript");
    std::ofstream root_out(root_buildscript);
    if (!root_out.is_open()) {
        std::cerr << "Error: Failed to create root buildscript: " << root_buildscript.string() << "\n";
        return false;
    }

    std::cout << "  Generating root: " << root_buildscript.string() << "\n";

    root_out << "# Generated root buildscript for solution: " << solution.name << "\n";
    root_out << "# This file includes all project buildscripts\n\n";
    root_out << "solution_name = " << solution.name << "\n";
    root_out << "solution_uuid = " << solution.uuid << "\n\n";

    // Write include directives for all project buildscripts
    for (const auto& include_path : buildscript_paths) {
        // Convert backslashes to forward slashes for consistency
        std::string normalized_path = include_path;
        std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
        root_out << "include = " << normalized_path << "\n";
    }

    root_out.close();

    return true;
}

} // namespace vcxproj
