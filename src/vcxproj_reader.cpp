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
    // Return command unchanged to preserve VPC commands
    return command;
}

// Helper function to normalize paths and remove redundant \.\ segments
// For 100% accuracy, we preserve paths exactly as they appear in the original vcxproj
static std::string normalize_path(const std::string& path) {
    // Return path unchanged to preserve exact formatting including \.\ segments
    return path;
}

// Helper function to normalize paths within build event commands
// For 100% accuracy, we preserve paths exactly as they appear
static std::string normalize_command_paths(const std::string& command) {
    // Return command unchanged to preserve exact path formatting
    return command;
}

// Helper to check if a path looks like a file path (not a macro or argument)
static bool looks_like_file_path(const std::string& token) {
    // Skip MSBuild macros
    if (token.find("$(") != std::string::npos || token.find("%(") != std::string::npos)
        return false;
    // Skip shell operators
    if (token == ">" || token == "<" || token == "|" || token == "||" || token == "&&")
        return false;
    // Check if it has path separators or file extension
    return (token.find('\\') != std::string::npos ||
            token.find('/') != std::string::npos ||
            (token.find('.') != std::string::npos && token.find(".exe") != std::string::npos) ||
            (token.find('.') != std::string::npos && token.find(".pl") != std::string::npos) ||
            (token.find('.') != std::string::npos && token.find(".py") != std::string::npos));
}

// Adjust relative file paths in a custom build command
static std::string adjust_command_paths(const std::string& command,
                                       const std::string& from_dir,
                                       const std::string& to_dir) {
    namespace fs = std::filesystem;

    if (command.empty()) return command;

    // Split command by spaces (simple tokenization)
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;

    for (char c : command) {
        if (c == '"') {
            in_quotes = !in_quotes;
            current_token += c;
        } else if (c == ' ' && !in_quotes) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token += c;
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    // Process each token
    std::string result;
    for (const auto& token : tokens) {
        if (!result.empty()) result += " ";

        // Check if this looks like a relative file path
        if (looks_like_file_path(token)) {
            try {
                // Remove quotes if present
                std::string clean_token = token;
                bool had_quotes = false;
                if (clean_token.size() >= 2 && clean_token.front() == '"' && clean_token.back() == '"') {
                    clean_token = clean_token.substr(1, clean_token.size() - 2);
                    had_quotes = true;
                }

                fs::path p(clean_token);
                // Only process relative paths
                if (!p.is_absolute()) {
                    // Make absolute relative to from_dir
                    fs::path abs_path = fs::absolute(fs::path(from_dir) / p);
                    // Make relative to to_dir
                    fs::path rel_path = fs::relative(abs_path, to_dir);

                    std::string new_path = rel_path.string();
                    std::replace(new_path.begin(), new_path.end(), '/', '\\');

                    if (had_quotes) {
                        result += "\"" + new_path + "\"";
                    } else {
                        result += new_path;
                    }
                } else {
                    result += token;
                }
            } catch (...) {
                // If path processing fails, keep original
                result += token;
            }
        } else {
            result += token;
        }
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
            project.project_name = prop_group.child_value("ProjectName");
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
            // Read TargetName from Configuration PropertyGroup
            if (auto node = prop_group.child("TargetName"))
                cfg.target_name = node.text().as_string();
        }
    }

    // Parse PropertyGroup settings (OutDir, IntDir, LinkIncremental, etc.)
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string condition = prop_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (!config_key.empty() && project.configurations.count(config_key)) {
            auto& cfg = project.configurations[config_key];

            if (auto node = prop_group.child("OutDir"))
                cfg.out_dir = normalize_path(node.text().as_string());
            if (auto node = prop_group.child("IntDir"))
                cfg.int_dir = normalize_path(node.text().as_string());
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
            if (auto node = prop_group.child("IgnoreImportLibrary"))
                cfg.ignore_import_library = node.text().as_bool();
            if (auto node = prop_group.child("ImportLibrary"))
                cfg.import_library = node.text().as_string();
            if (auto node = prop_group.child("PreBuildEventUseInBuild"))
                cfg.pre_build_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PreLinkEventUseInBuild"))
                cfg.pre_link_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PostBuildEventUseInBuild"))
                cfg.post_build_event.use_in_build = node.text().as_bool();
        }

        // Also handle PropertyGroup elements without a Condition, where individual
        // child elements have their own Condition attributes
        if (config_key.empty()) {
            for (auto node : prop_group.children()) {
                std::string node_condition = node.attribute("Condition").as_string();
                std::string node_config_key = parse_condition(node_condition);

                if (!node_config_key.empty() && project.configurations.count(node_config_key)) {
                    auto& cfg = project.configurations[node_config_key];
                    std::string node_name = node.name();

                    if (node_name == "OutDir")
                        cfg.out_dir = normalize_path(node.text().as_string());
                    else if (node_name == "IntDir")
                        cfg.int_dir = normalize_path(node.text().as_string());
                    else if (node_name == "TargetName")
                        cfg.target_name = node.text().as_string();
                    else if (node_name == "TargetExt")
                        cfg.target_ext = node.text().as_string();
                    else if (node_name == "LinkIncremental")
                        cfg.link_incremental = node.text().as_bool();
                    else if (node_name == "ExecutablePath")
                        cfg.executable_path = node.text().as_string();
                    else if (node_name == "GenerateManifest")
                        cfg.generate_manifest = node.text().as_bool();
                    else if (node_name == "PreBuildEventUseInBuild")
                        cfg.pre_build_event.use_in_build = node.text().as_bool();
                    else if (node_name == "PreLinkEventUseInBuild")
                        cfg.pre_link_event.use_in_build = node.text().as_bool();
                    else if (node_name == "PostBuildEventUseInBuild")
                        cfg.post_build_event.use_in_build = node.text().as_bool();
                }
            }
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
            READ_VECTOR("ForcedIncludeFiles", forced_include_files);
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
            READ_BOOL("TreatWChar_tAsBuiltInType", treat_wchar_t_as_built_in_type);
            READ_TEXT("AssemblerOutput", assembler_output);
            READ_BOOL("ExpandAttributedSource", expand_attributed_source);
            READ_BOOL("OpenMPSupport", openmp_support);
            READ_BOOL("TreatWarningAsError", treat_warning_as_error);

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
            // Normalize the output file path to remove redundant \.\ segments
            if (!settings.output_file.empty()) {
                settings.output_file = normalize_path(settings.output_file);
            }
            READ_BOOL("SuppressStartupBanner", suppress_startup_banner);
            READ_VECTOR("AdditionalDependencies", additional_dependencies);
            READ_VECTOR("AdditionalLibraryDirectories", additional_library_directories);
            READ_VECTOR("IgnoreSpecificDefaultLibraries", ignore_specific_default_libraries);
            READ_BOOL("GenerateDebugInformation", generate_debug_info);
            READ_TEXT("ProgramDatabaseFile", program_database_file);
            READ_TEXT("SubSystem", sub_system);
            READ_BOOL("OptimizeReferences", optimize_references);
            READ_BOOL("EnableCOMDATFolding", enable_comdat_folding);
            READ_BOOL("GenerateMapFile", generate_map_file);
            READ_TEXT("MapFileName", map_file_name);
            READ_TEXT("BaseAddress", base_address);
            READ_TEXT("TargetMachine", target_machine);
            READ_TEXT("LinkErrorReporting", error_reporting);
            READ_BOOL("ImageHasSafeExceptionHandlers", image_has_safe_exception_handlers);
            READ_TEXT("AdditionalOptions", additional_options);
            READ_TEXT("EntryPointSymbol", entry_point_symbol);
            READ_TEXT("Version", version);
            READ_BOOL("FixedBaseAddress", fixed_base_address);
            READ_BOOL("LargeAddressAware", large_address_aware);

            #undef READ_TEXT
            #undef READ_BOOL
            #undef READ_VECTOR
        }

        // Lib settings (for static libraries)
        if (auto lib = item_def.child("Lib")) {
            auto& settings = cfg.lib;

            if (auto n = lib.child("OutputFile"))
                settings.output_file = normalize_path(n.text().as_string());
            if (auto n = lib.child("SuppressStartupBanner"))
                settings.suppress_startup_banner = n.text().as_bool();
            if (auto n = lib.child("UseUnicodeResponseFiles"))
                settings.use_unicode_response_files = n.text().as_bool();
            if (auto n = lib.child("AdditionalOptions"))
                settings.additional_options = n.text().as_string();
            if (auto n = lib.child("AdditionalDependencies")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) settings.additional_dependencies.push_back(item);
                }
            }
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
            if (auto n = manifest.child("AdditionalManifestFiles"))
                cfg.manifest.additional_manifest_files = n.text().as_string();
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
                cfg.bscmake.output_file = normalize_path(n.text().as_string());
        }

        // Build events (filter out VPC-related commands and normalize paths)
        if (auto pre_build = item_def.child("PreBuildEvent")) {
            if (auto n = pre_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.pre_build_event.command = normalize_command_paths(cmd);
            }
            if (auto n = pre_build.child("Message"))
                cfg.pre_build_event.message = n.text().as_string();
        }
        if (auto pre_link = item_def.child("PreLinkEvent")) {
            if (auto n = pre_link.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.pre_link_event.command = normalize_command_paths(cmd);
            }
            if (auto n = pre_link.child("Message"))
                cfg.pre_link_event.message = n.text().as_string();
        }
        if (auto post_build = item_def.child("PostBuildEvent")) {
            if (auto n = post_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.post_build_event.command = normalize_command_paths(cmd);
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

    // Parse project dependencies (UUID -> list of dependent UUIDs)
    auto dependencies = parse_project_dependencies(content);

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

    // Build UUID -> project name mapping
    std::map<std::string, std::string> uuid_to_name;
    for (const auto& proj : solution.projects) {
        uuid_to_name[proj.uuid] = proj.name;
    }

    // Convert dependencies from UUIDs to project names
    for (auto& proj : solution.projects) {
        if (dependencies.count(proj.uuid)) {
            for (const auto& dep_uuid : dependencies[proj.uuid]) {
                if (uuid_to_name.count(dep_uuid)) {
                    proj.project_references.push_back(uuid_to_name[dep_uuid]);
                }
            }
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

std::map<std::string, std::vector<std::string>> SlnReader::parse_project_dependencies(const std::string& content) {
    std::map<std::string, std::vector<std::string>> dependencies;

    // Use string searching instead of complex regex to avoid stack overflow
    // Parse by finding Project lines, then looking for ProjectSection(ProjectDependencies) blocks

    size_t pos = 0;
    std::regex proj_line_re(R"(Project\s*\("[^"]+"\)\s*=\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"\{([A-Fa-f0-9\-]+)\}")");

    while (pos < content.length()) {
        // Find next "Project(" line
        size_t proj_start = content.find("Project(", pos);
        if (proj_start == std::string::npos) break;

        // Find the matching EndProject
        size_t end_project = content.find("EndProject", proj_start);
        if (end_project == std::string::npos) break;

        // Extract the project block
        std::string block = content.substr(proj_start, end_project - proj_start);

        // Extract UUID from Project line using regex
        std::smatch match;
        if (std::regex_search(block, match, proj_line_re)) {
            std::string project_uuid = match[1].str();

            // Check if this block has ProjectSection(ProjectDependencies)
            size_t dep_section = block.find("ProjectSection(ProjectDependencies)");
            if (dep_section != std::string::npos) {
                // Find EndProjectSection
                size_t end_section = block.find("EndProjectSection", dep_section);
                if (end_section != std::string::npos) {
                    std::string dep_section_content = block.substr(dep_section, end_section - dep_section);

                    // Extract dependency UUIDs
                    std::regex dep_re(R"(\{([A-Fa-f0-9\-]+)\}\s*=\s*\{[A-Fa-f0-9\-]+\})");
                    std::smatch dep_match;

                    auto it = dep_section_content.cbegin();
                    while (std::regex_search(it, dep_section_content.cend(), dep_match, dep_re)) {
                        dependencies[project_uuid].push_back(dep_match[1].str());
                        it = dep_match.suffix().first;
                    }
                }
            }
        }

        pos = end_project + 1;
    }

    std::cout << "Found " << dependencies.size() << " project(s) with dependencies\n";

    return dependencies;
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

    // Write global project properties
    if (!project.project_name.empty())
        out << "project_name = " << project.project_name << "\n";
    if (!project.uuid.empty())
        out << "uuid = " << project.uuid << "\n";
    if (!project.root_namespace.empty())
        out << "root_namespace = " << project.root_namespace << "\n";
    if (project.ignore_warn_compile_duplicated_filename)
        out << "ignore_warn_duplicated_filename = true\n";

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
        auto& libsettings = first_cfg.lib;

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
        if (!cl.forced_include_files.empty())
            out << "forced_includes = " << join_vector(cl.forced_include_files, ", ") << "\n";

        // Find common preprocessor definitions across all configurations
        // Configuration-specific defines like _DEBUG, DEBUG, NDEBUG will be written per-config
        if (!project.configurations.empty()) {
            std::set<std::string> common_defines;
            bool first = true;

            for (const auto& [config_key, config] : project.configurations) {
                std::set<std::string> config_defines(
                    config.cl_compile.preprocessor_definitions.begin(),
                    config.cl_compile.preprocessor_definitions.end()
                );

                if (first) {
                    common_defines = config_defines;
                    first = false;
                } else {
                    // Keep only defines that exist in all configurations
                    std::set<std::string> intersection;
                    std::set_intersection(
                        common_defines.begin(), common_defines.end(),
                        config_defines.begin(), config_defines.end(),
                        std::inserter(intersection, intersection.begin())
                    );
                    common_defines = intersection;
                }
            }

            if (!common_defines.empty()) {
                std::vector<std::string> common_defines_vec(common_defines.begin(), common_defines.end());
                out << "defines = " << join_vector(common_defines_vec, ", ") << "\n";
            }
        }
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
        if (!cl.disable_specific_warnings.empty())
            out << "disable_warnings = " << join_vector(cl.disable_specific_warnings, ", ") << "\n";
        if (!cl.additional_options.empty())
            out << "cflags = " << cl.additional_options << "\n";
        if (!cl.error_reporting.empty())
            out << "error_reporting = " << cl.error_reporting << "\n";
        if (!cl.treat_wchar_t_as_built_in_type)
            out << "treat_wchar_t_as_builtin = false\n";
        if (!cl.assembler_output.empty())
            out << "assembler_output = " << cl.assembler_output << "\n";
        if (cl.expand_attributed_source)
            out << "expand_attributed_source = true\n";
        if (cl.openmp_support)
            out << "openmp = true\n";
        if (cl.treat_warning_as_error)
            out << "treat_warning_as_error = true\n";
        if (!cl.assembler_listing_location.empty())
            out << "assembler_listing = " << cl.assembler_listing_location << "\n";
        if (!cl.object_file_name.empty())
            out << "object_file_name = " << cl.object_file_name << "\n";
        if (!cl.program_database_file_name.empty())
            out << "program_database_file = " << cl.program_database_file_name << "\n";
        if (!cl.browse_information_file.empty())
            out << "browse_information_file = " << cl.browse_information_file << "\n";
        if (!cl.basic_runtime_checks.empty())
            out << "basic_runtime_checks = " << cl.basic_runtime_checks << "\n";

        // Linker settings
        if (!link.sub_system.empty())
            out << "subsystem = " << link.sub_system << "\n";
        if (!link.image_has_safe_exception_handlers)
            out << "safe_seh = false\n";
        if (!link.additional_options.empty())
            out << "ldflags = " << link.additional_options << "\n";
        if (!link.error_reporting.empty())
            out << "link_error_reporting = " << link.error_reporting << "\n";
        if (!link.target_machine.empty())
            out << "target_machine = " << link.target_machine << "\n";
        if (link.suppress_startup_banner)
            out << "suppress_startup_banner = true\n";
        if (!link.show_progress.empty())
            out << "show_progress = " << link.show_progress << "\n";
        if (!link.entry_point_symbol.empty())
            out << "entry_point = " << link.entry_point_symbol << "\n";
        if (!link.version.empty())
            out << "link_version = " << link.version << "\n";
        if (!link.output_file.empty())
            out << "link_output_file = " << link.output_file << "\n";
        if (!link.program_database_file.empty())
            out << "link_program_database_file = " << link.program_database_file << "\n";
        if (link.generate_map_file)
            out << "generate_map_file = true\n";
        if (!link.map_file_name.empty())
            out << "map_file_name = " << link.map_file_name << "\n";
        if (link.fixed_base_address)
            out << "fixed_base_address = true\n";
        if (link.large_address_aware)
            out << "large_address_aware = true\n";

        // Librarian settings (for static libraries)
        if (!libsettings.output_file.empty())
            out << "lib_output_file = " << libsettings.output_file << "\n";
        if (libsettings.suppress_startup_banner)
            out << "lib_suppress_startup_banner = true\n";
        if (libsettings.use_unicode_response_files)
            out << "lib_use_unicode_response_files = true\n";
        if (!libsettings.additional_options.empty())
            out << "libflags = " << libsettings.additional_options << "\n";
        if (!libsettings.additional_dependencies.empty())
            out << "lib_additional_dependencies = " << join_vector(libsettings.additional_dependencies, ", ") << "\n";

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

        // Write Library elements with ExcludedFromBuild as excluded_library entries
        for (const auto* lib : excluded_libs) {
            // For each configuration, write the library if NOT excluded
            for (const auto& [config_key, cfg] : project.configurations) {
                bool is_excluded = false;
                for (const auto& [excl_config_key, excluded] : lib->excluded) {
                    if ((excl_config_key == ALL_CONFIGS || excl_config_key == config_key) && excluded) {
                        is_excluded = true;
                        break;
                    }
                }
                if (!is_excluded) {
                    out << "excluded_library[" << config_key << "] = " << lib->path << "\n";
                }
            }
        }
    }

    // Write config-specific settings
    out << "\n# Configuration-specific settings\n";

    // First, calculate common defines across all configurations
    std::set<std::string> common_defines;
    if (!project.configurations.empty()) {
        bool first = true;
        for (const auto& [config_key, config] : project.configurations) {
            std::set<std::string> config_defines(
                config.cl_compile.preprocessor_definitions.begin(),
                config.cl_compile.preprocessor_definitions.end()
            );

            if (first) {
                common_defines = config_defines;
                first = false;
            } else {
                std::set<std::string> intersection;
                std::set_intersection(
                    common_defines.begin(), common_defines.end(),
                    config_defines.begin(), config_defines.end(),
                    std::inserter(intersection, intersection.begin())
                );
                common_defines = intersection;
            }
        }
    }

    for (const auto& [config_key, cfg] : project.configurations) {
        out << "\n[config:" << config_key << "]\n";

        // Write config-specific preprocessor definitions (those not common to all configs)
        std::vector<std::string> config_specific_defines;
        for (const auto& def : cfg.cl_compile.preprocessor_definitions) {
            if (common_defines.find(def) == common_defines.end()) {
                config_specific_defines.push_back(def);
            }
        }
        if (!config_specific_defines.empty()) {
            out << "defines = " << join_vector(config_specific_defines, ", ") << "\n";
        }

        // Resolve out_dir and int_dir to actual paths relative to buildscript location
        // This ensures the buildscript is location-independent
        if (!cfg.out_dir.empty()) {
            std::string converted_out_dir = cfg.out_dir;
            if (!project.vcxproj_path.empty()) {
                namespace fs = std::filesystem;
                try {
                    // Resolve out_dir to absolute path based on vcxproj location
                    fs::path vcxproj_dir = fs::path(project.vcxproj_path).parent_path();
                    fs::path abs_out_dir = fs::absolute(vcxproj_dir / cfg.out_dir);

                    // Normalize the path (removes .\ and \.\ etc)
                    abs_out_dir = abs_out_dir.lexically_normal();

                    // Make it relative to buildscript location
                    fs::path buildscript_dir = fs::path(filepath).parent_path();
                    fs::path relative_out = fs::relative(abs_out_dir, buildscript_dir);

                    converted_out_dir = relative_out.string();
                    std::replace(converted_out_dir.begin(), converted_out_dir.end(), '/', '\\');

                    // Add trailing backslash if original had one
                    if (!cfg.out_dir.empty() && (cfg.out_dir.back() == '\\' || cfg.out_dir.back() == '/')) {
                        if (!converted_out_dir.empty() && converted_out_dir.back() != '\\') {
                            converted_out_dir += '\\';
                        }
                    }
                } catch (...) {
                    // If conversion fails, use original path
                }
            }
            out << "outdir = " << converted_out_dir << "\n";
        }
        if (!cfg.int_dir.empty()) {
            std::string converted_int_dir = cfg.int_dir;
            if (!project.vcxproj_path.empty()) {
                namespace fs = std::filesystem;
                try {
                    // Resolve int_dir to absolute path based on vcxproj location
                    fs::path vcxproj_dir = fs::path(project.vcxproj_path).parent_path();
                    fs::path abs_int_dir = fs::absolute(vcxproj_dir / cfg.int_dir);

                    // Normalize the path (removes .\ and \.\ etc)
                    abs_int_dir = abs_int_dir.lexically_normal();

                    // Make it relative to buildscript location
                    fs::path buildscript_dir = fs::path(filepath).parent_path();
                    fs::path relative_int = fs::relative(abs_int_dir, buildscript_dir);

                    converted_int_dir = relative_int.string();
                    std::replace(converted_int_dir.begin(), converted_int_dir.end(), '/', '\\');

                    // Add trailing backslash if original had one
                    if (!cfg.int_dir.empty() && (cfg.int_dir.back() == '\\' || cfg.int_dir.back() == '/')) {
                        if (!converted_int_dir.empty() && converted_int_dir.back() != '\\') {
                            converted_int_dir += '\\';
                        }
                    }
                } catch (...) {
                    // If conversion fails, use original path
                }
            }
            out << "intdir = " << converted_int_dir << "\n";
        }
        if (!cfg.target_name.empty())
            out << "target_name = " << cfg.target_name << "\n";
        if (!cfg.target_ext.empty())
            out << "target_ext = " << cfg.target_ext << "\n";
        if (!cfg.executable_path.empty())
            out << "executable_path = " << cfg.executable_path << "\n";
        if (!cfg.generate_manifest)
            out << "generate_manifest = false\n";
        if (cfg.ignore_import_library)
            out << "ignore_import_library = true\n";
        if (!cfg.import_library.empty())
            out << "import_library = " << cfg.import_library << "\n";
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
        // Only write per-config cflags if different from global (first config)
        if (!cfg.cl_compile.additional_options.empty() &&
            cfg.cl_compile.additional_options != project.configurations.begin()->second.cl_compile.additional_options)
            out << "cflags = " << cfg.cl_compile.additional_options << "\n";
        if (!cfg.cl_compile.favor_size_or_speed.empty())
            out << "favor = " << cfg.cl_compile.favor_size_or_speed << "\n";
        if (!cfg.cl_compile.inline_function_expansion.empty())
            out << "inline_expansion = " << cfg.cl_compile.inline_function_expansion << "\n";
        if (cfg.cl_compile.intrinsic_functions)
            out << "intrinsic_functions = true\n";
        if (cfg.cl_compile.function_level_linking)
            out << "function_level_linking = true\n";

        // Write config-specific libraries (only additional dependencies, not excluded libs)
        // Excluded libs are now written as excluded_library entries at project level
        std::vector<std::string> config_libs;

        // Add config-specific additional dependencies only
        for (const auto& dep : cfg.link.additional_dependencies) {
            config_libs.push_back(dep);
        }

        if (!config_libs.empty()) {
            out << "libs = " << join_vector(config_libs, ", ") << "\n";
        }

        // Linker ignore settings
        // Note: Common runtime libraries (libc, libcmt, libcmtd, etc.) are automatically
        // added based on the runtime_library setting, so you typically don't need to
        // specify them manually unless you have custom requirements
        if (!cfg.link.ignore_specific_default_libraries.empty()) {
            out << "ignore_libs = " << join_vector(cfg.link.ignore_specific_default_libraries, ", ") << "\n";
        }

        // Xdcmake settings
        if (cfg.xdcmake.suppress_startup_banner)
            out << "xdcmake_suppress_startup_banner = true\n";

        // Bscmake settings
        if (cfg.bscmake.suppress_startup_banner)
            out << "bscmake_suppress_startup_banner = true\n";
        if (!cfg.bscmake.output_file.empty())
            out << "bscmake_output_file = " << cfg.bscmake.output_file << "\n";

        // ResourceCompile settings
        if (!cfg.resource_compile.culture.empty())
            out << "rc_culture = " << cfg.resource_compile.culture << "\n";
        if (!cfg.resource_compile.preprocessor_definitions.empty())
            out << "rc_defines = " << join_vector(cfg.resource_compile.preprocessor_definitions, ", ") << "\n";
        if (!cfg.resource_compile.additional_include_directories.empty())
            out << "rc_includes = " << join_vector(cfg.resource_compile.additional_include_directories, ", ") << "\n";

        // Manifest settings
        if (cfg.manifest.suppress_startup_banner)
            out << "manifest_suppress_startup_banner = true\n";
        if (!cfg.manifest.additional_manifest_files.empty())
            out << "manifest_additional_files = " << cfg.manifest.additional_manifest_files << "\n";

        // Build events
        if (!cfg.pre_build_event.command.empty()) {
            out << "prebuild = " << format_value(cfg.pre_build_event.command) << "\n";
            if (!cfg.pre_build_event.message.empty())
                out << "prebuild_message = " << format_value(cfg.pre_build_event.message) << "\n";
            if (!cfg.pre_build_event.use_in_build)
                out << "prebuild_use_in_build = false\n";
        }
        if (!cfg.pre_link_event.command.empty()) {
            out << "prelink = " << format_value(cfg.pre_link_event.command) << "\n";
            if (!cfg.pre_link_event.message.empty())
                out << "prelink_message = " << format_value(cfg.pre_link_event.message) << "\n";
            if (!cfg.pre_link_event.use_in_build)
                out << "prelink_use_in_build = false\n";
        }
        if (!cfg.post_build_event.command.empty()) {
            out << "postbuild = " << format_value(cfg.post_build_event.command) << "\n";
            if (!cfg.post_build_event.message.empty())
                out << "postbuild_message = " << format_value(cfg.post_build_event.message) << "\n";
            if (!cfg.post_build_event.use_in_build)
                out << "postbuild_use_in_build = false\n";
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

    // Write project-level PCH defaults if they exist
    bool wrote_pch_defaults = false;
    for (const auto& [config_key, mode] : default_pch_mode) {
        // Write pch mode if not "NotUsing" (NotUsing is the implicit default)
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

    // Write pch_header even when mode is "NotUsing" if header is specified
    // This is needed for files that have Create mode - they inherit this header
    for (const auto& [config_key, header] : default_pch_header) {
        // Only write if we haven't already written it above
        if (!default_pch_mode.count(config_key) || default_pch_mode[config_key] == "NotUsing") {
            if (!wrote_pch_defaults) {
                out << "\n# Precompiled header defaults\n";
                wrote_pch_defaults = true;
            }
            out << "pch_header[" << config_key << "] = " << header << "\n";
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
                    // Write pch_output if specified
                    if (!pch.output.empty())
                        out << "pch_output[" << config_key << "] = " << pch.output << "\n";
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
                    out << "custom_command[" << config_key << "] = " << format_value(cmd) << "\n";
                    if (src.custom_outputs.count(config_key))
                        out << "custom_outputs[" << config_key << "] = " << src.custom_outputs.at(config_key) << "\n";
                    if (src.custom_message.count(config_key))
                        out << "custom_message[" << config_key << "] = " << format_value(src.custom_message.at(config_key)) << "\n";
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
