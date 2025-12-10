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

// Helper function to filter out VPC-related commands from build events
static std::string filter_vpc_commands(const std::string& command) {
    if (command.empty()) return command;

    std::string result;
    std::istringstream stream(command);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip lines that contain vpc.exe commands
        if (line.find("vpc.exe") != std::string::npos) {
            continue;
        }
        // Skip lines that reference .vpc_crc files
        if (line.find(".vpc_crc") != std::string::npos) {
            continue;
        }

        // Keep other lines
        if (!result.empty()) {
            result += "\n";
        }
        result += line;
    }

    // Trim trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
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
            cfg.whole_program_optimization = prop_group.child("WholeProgramOptimization").text().as_bool();
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

        // Manifest settings
        if (auto manifest = item_def.child("Manifest")) {
            if (auto n = manifest.child("SuppressStartupBanner"))
                cfg.manifest.suppress_startup_banner = n.text().as_bool();
        }

        // Xdcmake settings
        if (auto xdcmake = item_def.child("Xdcmake")) {
            if (auto n = xdcmake.child("SuppressStartupBanner"))
                cfg.xdcmake.suppress_startup_banner = n.text().as_bool();
        }

        // Bscmake settings
        if (auto bscmake = item_def.child("Bscmake")) {
            if (auto n = bscmake.child("SuppressStartupBanner"))
                cfg.bscmake.suppress_startup_banner = n.text().as_bool();
            if (auto n = bscmake.child("OutputFile"))
                cfg.bscmake.output_file = n.text().as_string();
        }

        // Build events (filter out VPC-related commands)
        if (auto pre_build = item_def.child("PreBuildEvent")) {
            if (auto n = pre_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cfg.pre_build_event.command = filter_vpc_commands(cmd);
            }
            if (auto n = pre_build.child("Message"))
                cfg.pre_build_event.message = n.text().as_string();
        }
        if (auto pre_link = item_def.child("PreLinkEvent")) {
            if (auto n = pre_link.child("Command")) {
                std::string cmd = n.text().as_string();
                cfg.pre_link_event.command = filter_vpc_commands(cmd);
            }
            if (auto n = pre_link.child("Message"))
                cfg.pre_link_event.message = n.text().as_string();
        }
        if (auto post_build = item_def.child("PostBuildEvent")) {
            if (auto n = post_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cfg.post_build_event.command = filter_vpc_commands(cmd);
            }
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
                } else if (name == "AdditionalOptions") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ' ')) {
                        if (!item.empty()) src.settings.additional_options[config_key].push_back(item);
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

        // Parse library references
        for (auto lib : item_group.children("Library")) {
            std::string include = lib.attribute("Include").as_string();
            if (!include.empty()) {
                LibraryFile lf;
                lf.path = include;

                // Parse per-library settings (like ExcludedFromBuild)
                for (auto child : lib.children()) {
                    std::string name = child.name();
                    if (name == "ExcludedFromBuild") {
                        std::string condition = child.attribute("Condition").as_string();
                        std::string config_key = condition.empty() ? ALL_CONFIGS : parse_condition(condition);
                        lf.excluded[config_key] = child.text().as_bool();
                    }
                }

                project.libraries.push_back(lf);
            }
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

    // Analyze libraries to separate those with exclusions from those without
    std::vector<const LibraryFile*> excluded_libs;
    for (const auto& lib : project.libraries) {
        if (!lib.excluded.empty()) {
            excluded_libs.push_back(&lib);
        }
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
        if (!cl.language_standard.empty()) {
            // Strip "stdcpp" prefix if present (e.g., "stdcpp17" -> "17")
            std::string std_value = cl.language_standard;
            if (std_value.find("stdcpp") == 0) {
                std_value = std_value.substr(6);  // Remove "stdcpp" prefix
            }
            out << "std = " << std_value << "\n";
        }
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

        // Combine Library elements and additional dependencies
        // Only include libraries without config-specific exclusions at project level
        std::vector<std::string> all_libs;

        for (const auto& lib : project.libraries) {
            // Check if library has any config-specific exclusions
            if (lib.excluded.empty()) {
                all_libs.push_back(lib.path);
            }
        }
        // Don't add config-specific additional_dependencies here
        // They will be written per-config
        if (!all_libs.empty())
            out << "libs = " << join_vector(all_libs, ", ") << "\n";

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
        if (cfg.whole_program_optimization)
            out << "whole_program_optimization = true\n";

        // Write config-specific libraries (excluded libs + additional dependencies)
        std::vector<std::string> config_libs;

        // Add libraries with exclusions that are NOT excluded for this config
        for (const auto* lib : excluded_libs) {
            bool is_excluded = false;
            for (const auto& [excl_config_key, excluded] : lib->excluded) {
                if ((excl_config_key == ALL_CONFIGS || excl_config_key == config_key) && excluded) {
                    is_excluded = true;
                    break;
                }
            }
            if (!is_excluded) {
                config_libs.push_back(lib->path);
            }
        }

        // Add config-specific additional dependencies
        for (const auto& dep : cfg.link.additional_dependencies) {
            config_libs.push_back(dep);
        }

        if (!config_libs.empty()) {
            out << "libs = " << join_vector(config_libs, ", ") << "\n";
        }

        // Linker ignore settings
        if (!cfg.link.ignore_specific_default_libraries.empty()) {
            out << "ignore_libs = " << join_vector(cfg.link.ignore_specific_default_libraries, ", ") << "\n";
        }

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

    // Use project-level PCH settings from ItemDefinitionGroup as defaults
    std::map<std::string, std::string> default_pch_mode;
    std::map<std::string, std::string> default_pch_header;
    std::map<std::string, std::string> default_pch_output;

    // Extract project-level PCH settings from configurations
    for (const auto& [config_key, cfg] : project.configurations) {
        if (!cfg.cl_compile.pch.mode.empty()) {
            default_pch_mode[config_key] = cfg.cl_compile.pch.mode;
        }
        if (!cfg.cl_compile.pch.header.empty()) {
            default_pch_header[config_key] = cfg.cl_compile.pch.header;
        }
        if (!cfg.cl_compile.pch.output.empty()) {
            default_pch_output[config_key] = cfg.cl_compile.pch.output;
        }
    }

    // Write project-level PCH defaults if they exist and are not "NotUsing"
    bool wrote_pch_defaults = false;
    for (const auto& [config_key, mode] : default_pch_mode) {
        // Only write non-NotUsing modes as defaults (NotUsing is the implicit default)
        if (mode != "NotUsing") {
            if (!wrote_pch_defaults) {
                out << "\n# Precompiled header defaults\n";
                wrote_pch_defaults = true;
            }
            out << "pch[" << config_key << "] = " << mode << "\n";
            if (default_pch_header.count(config_key)) {
                out << "pch_header[" << config_key << "] = " << default_pch_header[config_key] << "\n";
            }
            if (default_pch_output.count(config_key)) {
                out << "pch_output[" << config_key << "] = " << default_pch_output[config_key] << "\n";
            }
        }
    }

    // Write per-file settings (only for exceptions)
    for (const auto& src : project.sources) {
        bool has_pch_exception = false;
        bool has_other_settings = !src.settings.additional_includes.empty() ||
                                 !src.settings.preprocessor_defines.empty() ||
                                 !src.settings.additional_options.empty() ||
                                 !src.settings.excluded.empty() ||
                                 !src.settings.compile_as.empty() ||
                                 !src.settings.object_file.empty() ||
                                 !src.custom_command.empty();

        // Check if this file's PCH settings differ from defaults
        for (const auto& [config_key, pch] : src.settings.pch) {
            bool mode_differs = (!pch.mode.empty() &&
                                default_pch_mode.count(config_key) &&
                                pch.mode != default_pch_mode[config_key]);
            bool header_differs = (!pch.header.empty() &&
                                  default_pch_header.count(config_key) &&
                                  pch.header != default_pch_header[config_key]);
            bool no_default = (!pch.mode.empty() && !default_pch_mode.count(config_key));

            if (mode_differs || header_differs || no_default || pch.mode == "NotUsing") {
                has_pch_exception = true;
                break;
            }
        }

        if (has_pch_exception || has_other_settings) {
            out << "\n[file:" << src.path << "]\n";

            // Write PCH settings only if they differ from defaults
            if (has_pch_exception) {
                for (const auto& [config_key, pch] : src.settings.pch) {
                    bool mode_differs = (!pch.mode.empty() &&
                                        (!default_pch_mode.count(config_key) ||
                                         pch.mode != default_pch_mode[config_key]));
                    bool header_differs = (!pch.header.empty() &&
                                          (!default_pch_header.count(config_key) ||
                                           pch.header != default_pch_header[config_key]));

                    if (mode_differs || pch.mode == "NotUsing")
                        out << "pch[" << config_key << "] = " << pch.mode << "\n";
                    if (header_differs)
                        out << "pch_header[" << config_key << "] = " << pch.header << "\n";
                }
            }

            for (const auto& [config_key, includes] : src.settings.additional_includes) {
                if (!includes.empty())
                    out << "includes[" << config_key << "] = " << join_vector(includes, ", ") << "\n";
            }

            for (const auto& [config_key, defines] : src.settings.preprocessor_defines) {
                if (!defines.empty())
                    out << "defines[" << config_key << "] = " << join_vector(defines, ", ") << "\n";
            }

            for (const auto& [config_key, options] : src.settings.additional_options) {
                if (!options.empty())
                    out << "flags[" << config_key << "] = " << join_vector(options, ", ") << "\n";
            }

            for (const auto& [config_key, excluded] : src.settings.excluded) {
                if (excluded)
                    out << "excluded[" << config_key << "] = true\n";
            }

            for (const auto& [config_key, compile_as] : src.settings.compile_as) {
                if (!compile_as.empty())
                    out << "compile_as[" << config_key << "] = " << compile_as << "\n";
            }

            for (const auto& [config_key, obj_file] : src.settings.object_file) {
                if (!obj_file.empty())
                    out << "object_file[" << config_key << "] = " << obj_file << "\n";
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
    root_out << "[solution]\n";
    root_out << "name = " << solution.name << "\n";
    root_out << "uuid = " << solution.uuid << "\n\n";

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
