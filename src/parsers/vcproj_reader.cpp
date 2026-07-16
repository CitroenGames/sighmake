#include "pch.h"
#include "vcproj_reader.hpp"
#define PUGIXML_HEADER_ONLY
#include "pugixml.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

namespace {

std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string strip_quotes(std::string value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool is_inherit_marker(const std::string& item) {
    return item == "$(NoInherit)" || item == "$(Inherit)";
}

// Split a ';'- or ','-separated vcproj list attribute. Items may be quoted.
std::vector<std::string> split_list(const std::string& value) {
    std::vector<std::string> items;
    std::string current;
    bool in_quotes = false;

    for (char c : value) {
        if (c == '"') {
            in_quotes = !in_quotes;
            current += c;
        } else if ((c == ';' || c == ',') && !in_quotes) {
            std::string item = strip_quotes(trim_copy(current));
            if (!item.empty() && !is_inherit_marker(item)) items.push_back(item);
            current.clear();
        } else {
            current += c;
        }
    }
    std::string item = strip_quotes(trim_copy(current));
    if (!item.empty() && !is_inherit_marker(item)) items.push_back(item);
    return items;
}

// Split a space-separated vcproj list (AdditionalDependencies). Items with
// spaces are quoted.
std::vector<std::string> split_space_list(const std::string& value) {
    std::vector<std::string> items;
    std::string current;
    bool in_quotes = false;

    for (char c : value) {
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if ((c == ' ' || c == '\t') && !in_quotes) {
            std::string item = trim_copy(current);
            if (!item.empty() && !is_inherit_marker(item)) items.push_back(item);
            current.clear();
        } else {
            current += c;
        }
    }
    std::string item = trim_copy(current);
    if (!item.empty() && !is_inherit_marker(item)) items.push_back(item);
    return items;
}

bool starts_with_ci(const std::string& value, size_t pos, const std::string& prefix) {
    if (value.size() - pos < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[pos + i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

void replace_all_ci(std::string& value, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) return;
    for (size_t pos = 0; pos + needle.size() <= value.size();) {
        if (starts_with_ci(value, pos, needle)) {
            value.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        } else {
            ++pos;
        }
    }
}

// Translate VS2003-2008 project-level macros to their MSBuild equivalents so
// the rest of the pipeline (buildscript writer, vcxproj generator) understands
// them.
std::string translate_config_macros(std::string value) {
    replace_all_ci(value, "$(ConfigurationName)", "$(Configuration)");
    replace_all_ci(value, "$(PlatformName)", "$(Platform)");
    return value;
}

// Translate per-file macros used in custom build steps. $(Input*) referred to
// the file the custom build step ran on; MSBuild uses %(...) item metadata.
std::string translate_file_macros(std::string value) {
    value = translate_config_macros(std::move(value));
    replace_all_ci(value, "$(InputFileName)", "%(Filename)%(Extension)");
    replace_all_ci(value, "$(InputName)", "%(Filename)");
    replace_all_ci(value, "$(InputDir)", "%(RootDir)%(Directory)");
    replace_all_ci(value, "$(InputPath)", "%(FullPath)");
    replace_all_ci(value, "$(InputExt)", "%(Extension)");
    return value;
}

// vcproj paths conventionally start with ".\" — strip it (string-based so it
// behaves the same on non-Windows hosts where '\' is not a path separator).
std::string clean_relative_path(std::string path) {
    while (path.size() >= 2 && path[0] == '.' && (path[1] == '\\' || path[1] == '/')) {
        path.erase(0, 2);
    }
    return path;
}

std::string strip_guid_braces(std::string guid) {
    if (guid.size() >= 2 && guid.front() == '{' && guid.back() == '}') {
        guid = guid.substr(1, guid.size() - 2);
    }
    return guid;
}

// --- numeric enum mappings (VCProjectEngine values) ---

std::string map_configuration_type(int value) {
    switch (value) {
        case 1: return "Application";
        case 2: return "DynamicLibrary";
        case 4: return "StaticLibrary";
        case 10: return "Utility";
        default: return "Utility"; // 0 = Makefile — no direct equivalent
    }
}

std::string map_character_set(int value) {
    switch (value) {
        case 1: return "Unicode";
        case 2: return "MultiByte";
        default: return "";
    }
}

std::string map_use_of_mfc_atl(int value) {
    switch (value) {
        case 1: return "Static";
        case 2: return "Dynamic";
        default: return "";
    }
}

std::string map_optimization(int value) {
    switch (value) {
        case 0: return "Disabled";
        case 1: return "MinSpace";
        case 2: return "MaxSpeed";
        case 3: return "Full";
        case 4: return "Custom";
        default: return "";
    }
}

std::string map_inline_expansion(int value) {
    switch (value) {
        case 0: return "Disabled";
        case 1: return "OnlyExplicitInline";
        case 2: return "AnySuitable";
        default: return "";
    }
}

std::string map_favor_size_or_speed(int value) {
    switch (value) {
        case 1: return "Speed";
        case 2: return "Size";
        default: return "";
    }
}

std::string map_runtime_library(int value) {
    switch (value) {
        case 0: return "MultiThreaded";
        case 1: return "MultiThreadedDebug";
        case 2: return "MultiThreadedDLL";
        case 3: return "MultiThreadedDebugDLL";
        // 4/5 were the VS2003 single-threaded CRTs (/ML, /MLd) — long gone,
        // map to the closest surviving equivalents.
        case 4: return "MultiThreaded";
        case 5: return "MultiThreadedDebug";
        default: return "";
    }
}

std::string map_basic_runtime_checks(int value) {
    switch (value) {
        case 1: return "StackFrameRuntimeCheck";
        case 2: return "UninitializedLocalUsageCheck";
        case 3: return "EnableFastChecks";
        default: return "";
    }
}

std::string map_enhanced_instruction_set(int value) {
    switch (value) {
        case 1: return "StreamingSIMDExtensions";
        case 2: return "StreamingSIMDExtensions2";
        default: return "";
    }
}

std::string map_floating_point_model(int value) {
    switch (value) {
        case 0: return "Precise";
        case 1: return "Strict";
        case 2: return "Fast";
        default: return "";
    }
}

// VS2005+: 0 = not using, 1 = create (/Yc), 2 = use (/Yu)
// VS2003:  0 = not using, 1 = create, 2 = automatic (/YX), 3 = use (/Yu)
std::string map_use_precompiled_header(int value, int format_major_version) {
    if (format_major_version < 8) {
        switch (value) {
            case 1: return "Create";
            case 2: return "Use"; // /YX auto — closest modern equivalent
            case 3: return "Use";
            default: return "NotUsing";
        }
    }
    switch (value) {
        case 1: return "Create";
        case 2: return "Use";
        default: return "NotUsing";
    }
}

std::string map_warning_level(int value) {
    if (value == 0) return "TurnOffAllWarnings";
    if (value >= 1 && value <= 4) return "Level" + std::to_string(value);
    return "";
}

std::string map_debug_information_format(int value) {
    switch (value) {
        case 1: return "OldStyle";           // /Z7
        case 2: return "OldStyle";           // /Zd (VS2003 line numbers only)
        case 3: return "ProgramDatabase";    // /Zi
        case 4: return "EditAndContinue";    // /ZI
        default: return "";
    }
}

std::string map_compile_as(int value) {
    switch (value) {
        case 1: return "CompileAsC";
        case 2: return "CompileAsCpp";
        default: return "";
    }
}

// ExceptionHandling was a bool in VS2003 and an enum (0/1/2) from VS2005 on.
std::string map_exception_handling(const std::string& raw) {
    std::string lowered = to_lower(trim_copy(raw));
    if (lowered == "true") return "Sync";
    if (lowered == "false" || lowered == "0") return "false";
    if (lowered == "1") return "Sync";
    if (lowered == "2") return "Async";
    return "";
}

std::string map_sub_system(int value) {
    switch (value) {
        case 1: return "Console";
        case 2: return "Windows";
        case 3: return "Native";
        default: return "";
    }
}

std::string map_show_progress(int value) {
    switch (value) {
        case 1: return "LinkVerbose";
        case 2: return "LinkVerboseLib";
        default: return "";
    }
}

std::string map_target_machine(int value) {
    switch (value) {
        case 1: return "MachineX86";
        case 17: return "MachineX64";
        default: return "";
    }
}

std::string map_link_error_reporting(int value) {
    switch (value) {
        case 1: return "PromptImmediately";
        case 2: return "QueueForNextLogin";
        default: return "";
    }
}

std::string map_midl_target_environment(int value) {
    switch (value) {
        case 1: return "Win32";
        case 3: return "Win64";
        default: return "";
    }
}

std::string map_midl_default_char_type(int value) {
    switch (value) {
        case 0: return "Unsigned";
        case 1: return "Signed";
        default: return "";
    }
}

// Settings accumulated from .vsprops property sheets. Only the list-type
// settings that meaningfully merge are pulled in; scalar settings stay with
// the project.
struct VspropsSettings {
    std::vector<std::string> include_directories;
    std::vector<std::string> preprocessor_definitions;
    std::vector<std::string> link_dependencies;
    std::vector<std::string> link_library_directories;
};

void read_vsprops_file(const fs::path& filepath, VspropsSettings& out, int depth);

void read_inherited_sheets(const std::string& sheet_list, const fs::path& base_dir,
                           VspropsSettings& out, int depth) {
    if (depth > 8) return; // guard against inheritance cycles

    for (const auto& entry : split_list(sheet_list)) {
        if (entry.find("$(") != std::string::npos) continue; // unresolvable macro path
        fs::path sheet_path = fs::path(entry).is_absolute()
            ? fs::path(entry)
            : base_dir / entry;
        read_vsprops_file(sheet_path.lexically_normal(), out, depth);
    }
}

void read_vsprops_file(const fs::path& filepath, VspropsSettings& out, int depth) {
    pugi::xml_document doc;
    if (!doc.load_file(filepath.string().c_str())) {
        std::cerr << "Warning: Failed to load property sheet: " << filepath.string() << std::endl;
        return;
    }

    auto root = doc.child("VisualStudioPropertySheet");
    if (!root) return;

    // Sheets can inherit from other sheets
    if (auto inherited = root.attribute("InheritedPropertySheets")) {
        read_inherited_sheets(inherited.as_string(), filepath.parent_path(), out, depth + 1);
    }

    for (auto tool : root.children("Tool")) {
        std::string tool_name = tool.attribute("Name").as_string();
        if (tool_name == "VCCLCompilerTool") {
            for (auto& item : split_list(tool.attribute("AdditionalIncludeDirectories").as_string()))
                out.include_directories.push_back(translate_config_macros(item));
            for (auto& item : split_list(tool.attribute("PreprocessorDefinitions").as_string()))
                out.preprocessor_definitions.push_back(item);
        } else if (tool_name == "VCLinkerTool") {
            for (auto& item : split_space_list(tool.attribute("AdditionalDependencies").as_string()))
                out.link_dependencies.push_back(item);
            for (auto& item : split_list(tool.attribute("AdditionalLibraryDirectories").as_string()))
                out.link_library_directories.push_back(translate_config_macros(item));
        }
    }
}

void read_compiler_tool(pugi::xml_node tool, ClCompileSettings& settings, int format_major_version) {
    #define ATTR(name) tool.attribute(name)

    if (auto a = ATTR("Optimization")) settings.optimization = map_optimization(a.as_int());
    if (auto a = ATTR("InlineFunctionExpansion")) settings.inline_function_expansion = map_inline_expansion(a.as_int());
    if (auto a = ATTR("EnableIntrinsicFunctions")) settings.intrinsic_functions = a.as_bool();
    if (auto a = ATTR("FavorSizeOrSpeed")) settings.favor_size_or_speed = map_favor_size_or_speed(a.as_int());

    if (auto a = ATTR("AdditionalIncludeDirectories")) {
        for (auto& item : split_list(a.as_string()))
            settings.additional_include_directories.push_back(translate_config_macros(item));
    }
    if (auto a = ATTR("PreprocessorDefinitions")) {
        for (auto& item : split_list(a.as_string()))
            settings.preprocessor_definitions.push_back(item);
    }
    if (auto a = ATTR("ForcedIncludeFiles")) {
        for (auto& item : split_list(a.as_string()))
            settings.forced_include_files.push_back(item);
    }

    if (auto a = ATTR("StringPooling")) settings.string_pooling = a.as_bool();
    if (auto a = ATTR("MinimalRebuild")) settings.minimal_rebuild = a.as_bool();
    if (auto a = ATTR("ExceptionHandling")) settings.exception_handling = map_exception_handling(a.as_string());
    if (auto a = ATTR("BasicRuntimeChecks")) settings.basic_runtime_checks = map_basic_runtime_checks(a.as_int());
    if (auto a = ATTR("RuntimeLibrary")) settings.runtime_library = map_runtime_library(a.as_int());
    if (auto a = ATTR("BufferSecurityCheck")) settings.buffer_security_check = a.as_bool();
    if (auto a = ATTR("EnableFunctionLevelLinking")) settings.function_level_linking = a.as_bool();
    if (auto a = ATTR("EnableEnhancedInstructionSet")) settings.enhanced_instruction_set = map_enhanced_instruction_set(a.as_int());
    if (auto a = ATTR("FloatingPointModel")) settings.floating_point_model = map_floating_point_model(a.as_int());
    if (auto a = ATTR("ForceConformanceInForLoopScope")) settings.force_conformance_in_for_loop_scope = a.as_bool();
    if (auto a = ATTR("RuntimeTypeInfo")) settings.runtime_type_info = a.as_bool();

    if (auto a = ATTR("UsePrecompiledHeader")) settings.pch.mode = map_use_precompiled_header(a.as_int(), format_major_version);
    if (auto a = ATTR("PrecompiledHeaderThrough")) settings.pch.header = a.as_string();
    if (auto a = ATTR("PrecompiledHeaderFile")) settings.pch.output = translate_config_macros(a.as_string());

    if (auto a = ATTR("AssemblerListingLocation")) settings.assembler_listing_location = translate_config_macros(a.as_string());
    if (auto a = ATTR("ObjectFile")) settings.object_file_name = translate_config_macros(a.as_string());
    if (auto a = ATTR("ProgramDataBaseFileName")) settings.program_database_file_name = translate_config_macros(a.as_string());
    if (auto a = ATTR("GenerateXMLDocumentationFiles")) settings.generate_xml_documentation_files = a.as_bool();
    if (auto a = ATTR("BrowseInformation")) settings.browse_information = a.as_int() != 0;
    if (auto a = ATTR("BrowseInformationFile")) settings.browse_information_file = translate_config_macros(a.as_string());

    if (auto a = ATTR("WarningLevel")) settings.warning_level = map_warning_level(a.as_int());
    if (auto a = ATTR("WarnAsError")) settings.treat_warning_as_error = a.as_bool();
    if (auto a = ATTR("DebugInformationFormat")) settings.debug_information_format = map_debug_information_format(a.as_int());
    if (auto a = ATTR("CompileAs")) settings.compile_as = map_compile_as(a.as_int());
    if (auto a = ATTR("DisableSpecificWarnings")) {
        for (auto& item : split_list(a.as_string()))
            settings.disable_specific_warnings.push_back(item);
    }
    if (auto a = ATTR("AdditionalOptions")) settings.additional_options = a.as_string();
    if (auto a = ATTR("TreatWChar_tAsBuiltInType")) settings.treat_wchar_t_as_built_in_type = a.as_bool();
    if (auto a = ATTR("OpenMP")) settings.openmp_support = a.as_bool();

    #undef ATTR
}

void read_linker_tool(pugi::xml_node tool, Configuration& cfg) {
    auto& settings = cfg.link;
    #define ATTR(name) tool.attribute(name)

    if (auto a = ATTR("OutputFile")) settings.output_file = translate_config_macros(a.as_string());
    if (auto a = ATTR("ShowProgress")) settings.show_progress = map_show_progress(a.as_int());
    if (auto a = ATTR("Version")) settings.version = a.as_string();
    if (auto a = ATTR("LinkIncremental")) cfg.link_incremental = a.as_int() == 2;
    if (auto a = ATTR("SuppressStartupBanner")) settings.suppress_startup_banner = a.as_bool();

    if (auto a = ATTR("AdditionalDependencies")) {
        for (auto& item : split_space_list(a.as_string()))
            settings.additional_dependencies.push_back(item);
    }
    if (auto a = ATTR("AdditionalLibraryDirectories")) {
        for (auto& item : split_list(a.as_string()))
            settings.additional_library_directories.push_back(translate_config_macros(item));
    }
    if (auto a = ATTR("IgnoreAllDefaultLibraries")) settings.ignore_all_default_libraries = a.as_bool();
    if (auto a = ATTR("IgnoreDefaultLibraryNames")) {
        for (auto& item : split_list(a.as_string()))
            settings.ignore_specific_default_libraries.push_back(item);
    }

    if (auto a = ATTR("ModuleDefinitionFile")) settings.module_definition_file = a.as_string();
    if (auto a = ATTR("GenerateManifest")) cfg.generate_manifest = a.as_bool();
    if (auto a = ATTR("IgnoreImportLibrary")) cfg.ignore_import_library = a.as_bool();
    if (auto a = ATTR("ImportLibrary")) cfg.import_library = translate_config_macros(a.as_string());
    if (auto a = ATTR("GenerateDebugInformation")) settings.generate_debug_info = a.as_bool();
    if (auto a = ATTR("ProgramDatabaseFile")) settings.program_database_file = translate_config_macros(a.as_string());
    if (auto a = ATTR("GenerateMapFile")) settings.generate_map_file = a.as_bool();
    if (auto a = ATTR("MapFileName")) settings.map_file_name = translate_config_macros(a.as_string());

    if (auto a = ATTR("SubSystem")) settings.sub_system = map_sub_system(a.as_int());
    if (auto a = ATTR("LargeAddressAware")) settings.large_address_aware = a.as_int() == 2;
    if (auto a = ATTR("OptimizeReferences")) {
        int v = a.as_int();
        if (v == 1) settings.optimize_references = false;
        else if (v == 2) settings.optimize_references = true;
    }
    if (auto a = ATTR("EnableCOMDATFolding")) {
        int v = a.as_int();
        if (v == 1) settings.enable_comdat_folding = false;
        else if (v == 2) settings.enable_comdat_folding = true;
    }
    if (auto a = ATTR("EntryPointSymbol")) settings.entry_point_symbol = a.as_string();
    if (auto a = ATTR("BaseAddress")) settings.base_address = a.as_string();
    if (auto a = ATTR("FixedBaseAddress")) settings.fixed_base_address = a.as_int() == 2;
    if (auto a = ATTR("RandomizedBaseAddress")) {
        int v = a.as_int();
        if (v == 1) settings.randomized_base_address = false;
        else if (v == 2) settings.randomized_base_address = true;
    }
    if (auto a = ATTR("TargetMachine")) settings.target_machine = map_target_machine(a.as_int());
    if (auto a = ATTR("ErrorReporting")) settings.error_reporting = map_link_error_reporting(a.as_int());
    if (auto a = ATTR("AdditionalOptions")) settings.additional_options = a.as_string();

    #undef ATTR
}

void read_librarian_tool(pugi::xml_node tool, LibrarianSettings& settings) {
    if (auto a = tool.attribute("OutputFile")) settings.output_file = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("SuppressStartupBanner")) settings.suppress_startup_banner = a.as_bool();
    if (auto a = tool.attribute("UseUnicodeResponseFiles")) settings.use_unicode_response_files = a.as_bool();
    if (auto a = tool.attribute("AdditionalOptions")) settings.additional_options = a.as_string();
    if (auto a = tool.attribute("AdditionalDependencies")) {
        for (auto& item : split_space_list(a.as_string()))
            settings.additional_dependencies.push_back(item);
    }
}

void read_resource_tool(pugi::xml_node tool, ResourceCompileSettings& settings) {
    if (auto a = tool.attribute("PreprocessorDefinitions")) {
        for (auto& item : split_list(a.as_string()))
            settings.preprocessor_definitions.push_back(item);
    }
    if (auto a = tool.attribute("Culture")) settings.culture = a.as_string();
    if (auto a = tool.attribute("AdditionalIncludeDirectories")) {
        for (auto& item : split_list(a.as_string()))
            settings.additional_include_directories.push_back(translate_config_macros(item));
    }
}

void read_midl_tool(pugi::xml_node tool, MidlSettings& settings) {
    if (auto a = tool.attribute("OutputDirectory")) settings.output_directory = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("HeaderFileName")) settings.header_file_name = a.as_string();
    if (auto a = tool.attribute("TypeLibraryName")) settings.type_library_name = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("DLLDataFileName")) settings.dlldata_file_name = a.as_string();
    if (auto a = tool.attribute("InterfaceIdentifierFileName")) settings.interface_identifier_file_name = a.as_string();
    if (auto a = tool.attribute("ProxyFileName")) settings.proxy_file_name = a.as_string();
    if (auto a = tool.attribute("PreprocessorDefinitions")) {
        for (auto& item : split_list(a.as_string()))
            settings.preprocessor_definitions.push_back(item);
    }
    if (auto a = tool.attribute("AdditionalOptions")) settings.additional_options = a.as_string();
    if (auto a = tool.attribute("DefaultCharType")) settings.default_char_type = map_midl_default_char_type(a.as_int());
    if (auto a = tool.attribute("TargetEnvironment")) settings.target_environment = map_midl_target_environment(a.as_int());
}

void read_build_event_tool(pugi::xml_node tool, BuildEvent& event) {
    if (auto a = tool.attribute("CommandLine")) event.command = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("Description")) event.message = a.as_string();
    if (auto a = tool.attribute("ExcludedFromBuild")) event.use_in_build = !a.as_bool();
}

void read_file_compiler_overrides(pugi::xml_node tool, SourceFile& src,
                                  const std::string& config_key, int format_major_version) {
    if (auto a = tool.attribute("AdditionalIncludeDirectories")) {
        for (auto& item : split_list(a.as_string()))
            src.settings.additional_includes[config_key].push_back(translate_config_macros(item));
    }
    if (auto a = tool.attribute("PreprocessorDefinitions")) {
        for (auto& item : split_list(a.as_string()))
            src.settings.preprocessor_defines[config_key].push_back(item);
    }
    if (auto a = tool.attribute("AdditionalOptions")) {
        std::istringstream ss(a.as_string());
        std::string item;
        while (std::getline(ss, item, ' ')) {
            if (!item.empty()) src.settings.additional_options[config_key].push_back(item);
        }
    }
    if (auto a = tool.attribute("UsePrecompiledHeader"))
        src.settings.pch[config_key].mode = map_use_precompiled_header(a.as_int(), format_major_version);
    if (auto a = tool.attribute("PrecompiledHeaderThrough"))
        src.settings.pch[config_key].header = a.as_string();
    if (auto a = tool.attribute("PrecompiledHeaderFile"))
        src.settings.pch[config_key].output = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("ObjectFile"))
        src.settings.object_file[config_key] = translate_config_macros(a.as_string());
    if (auto a = tool.attribute("CompileAs")) {
        std::string mapped = map_compile_as(a.as_int());
        if (!mapped.empty()) src.settings.compile_as[config_key] = mapped;
    }
    if (auto a = tool.attribute("Optimization"))
        src.settings.optimization[config_key] = map_optimization(a.as_int());
}

} // namespace

Project VcprojReader::read_vcproj(const std::string& filepath) {
    Project project;
    pugi::xml_document doc;

    pugi::xml_parse_result result = doc.load_file(filepath.c_str());
    if (!result) {
        throw std::runtime_error("Failed to parse vcproj: " + std::string(result.description()));
    }

    auto root = doc.child("VisualStudioProject");
    if (!root) {
        throw std::runtime_error("Invalid vcproj file: no VisualStudioProject root element");
    }

    project.project_name = root.attribute("Name").as_string();
    project.name = project.project_name;
    project.uuid = strip_guid_braces(root.attribute("ProjectGUID").as_string());
    project.root_namespace = root.attribute("RootNamespace").as_string();

    // "7.10" (VS2003), "8.00" (VS2005), "9.00" (VS2008) — a couple of enums
    // changed meaning at 8.00.
    int format_major_version = root.attribute("Version").as_int(9);

    const fs::path vcproj_dir = fs::path(filepath).parent_path();

    // Parse configurations
    for (auto cfg_node : root.child("Configurations").children("Configuration")) {
        std::string config_key = cfg_node.attribute("Name").as_string();
        if (config_key.empty()) continue;

        auto& cfg = project.configurations[config_key];

        cfg.config_type = map_configuration_type(cfg_node.attribute("ConfigurationType").as_int());
        cfg.character_set = map_character_set(cfg_node.attribute("CharacterSet").as_int());
        if (auto a = cfg_node.attribute("UseOfMFC")) {
            std::string mapped = map_use_of_mfc_atl(a.as_int());
            if (!mapped.empty()) cfg.use_of_mfc = mapped;
        }
        if (auto a = cfg_node.attribute("UseOfATL")) {
            std::string mapped = map_use_of_mfc_atl(a.as_int());
            if (!mapped.empty()) cfg.use_of_atl = mapped;
        }
        if (auto a = cfg_node.attribute("WholeProgramOptimization"))
            cfg.whole_program_optimization = a.as_int() != 0 || a.as_bool();
        if (auto a = cfg_node.attribute("OutputDirectory"))
            cfg.out_dir = translate_config_macros(a.as_string());
        if (auto a = cfg_node.attribute("IntermediateDirectory"))
            cfg.int_dir = translate_config_macros(a.as_string());

        cfg.use_debug_libraries = config_key.find("Debug") != std::string::npos;

        // Property sheets (.vsprops) — collect list-type settings and append
        // them after the project's own values, mirroring VS inherit-by-default
        // semantics.
        VspropsSettings sheets;
        if (auto a = cfg_node.attribute("InheritedPropertySheets")) {
            read_inherited_sheets(a.as_string(), vcproj_dir, sheets, 0);
        }

        for (auto tool : cfg_node.children("Tool")) {
            std::string tool_name = tool.attribute("Name").as_string();

            if (tool_name == "VCCLCompilerTool") {
                read_compiler_tool(tool, cfg.cl_compile, format_major_version);
            } else if (tool_name == "VCLinkerTool") {
                read_linker_tool(tool, cfg);
            } else if (tool_name == "VCLibrarianTool") {
                read_librarian_tool(tool, cfg.lib);
            } else if (tool_name == "VCResourceCompilerTool") {
                read_resource_tool(tool, cfg.resource_compile);
            } else if (tool_name == "VCMIDLTool") {
                read_midl_tool(tool, cfg.midl);
            } else if (tool_name == "VCManifestTool") {
                if (auto a = tool.attribute("SuppressStartupBanner"))
                    cfg.manifest.suppress_startup_banner = a.as_bool();
                if (auto a = tool.attribute("AdditionalManifestFiles"))
                    cfg.manifest.additional_manifest_files = a.as_string();
            } else if (tool_name == "VCXDCMakeTool") {
                if (auto a = tool.attribute("SuppressStartupBanner"))
                    cfg.xdcmake.suppress_startup_banner = a.as_bool();
            } else if (tool_name == "VCBscMakeTool") {
                if (auto a = tool.attribute("SuppressStartupBanner"))
                    cfg.bscmake.suppress_startup_banner = a.as_bool();
                if (auto a = tool.attribute("OutputFile"))
                    cfg.bscmake.output_file = translate_config_macros(a.as_string());
            } else if (tool_name == "VCPreBuildEventTool") {
                read_build_event_tool(tool, cfg.pre_build_event);
            } else if (tool_name == "VCPreLinkEventTool") {
                read_build_event_tool(tool, cfg.pre_link_event);
            } else if (tool_name == "VCPostBuildEventTool") {
                read_build_event_tool(tool, cfg.post_build_event);
            }
        }

        cfg.cl_compile.additional_include_directories.insert(
            cfg.cl_compile.additional_include_directories.end(),
            sheets.include_directories.begin(), sheets.include_directories.end());
        cfg.cl_compile.preprocessor_definitions.insert(
            cfg.cl_compile.preprocessor_definitions.end(),
            sheets.preprocessor_definitions.begin(), sheets.preprocessor_definitions.end());
        cfg.link.additional_dependencies.insert(
            cfg.link.additional_dependencies.end(),
            sheets.link_dependencies.begin(), sheets.link_dependencies.end());
        cfg.link.additional_library_directories.insert(
            cfg.link.additional_library_directories.end(),
            sheets.link_library_directories.begin(), sheets.link_library_directories.end());
    }

    // Parse the Files tree (nested Filter elements with File leaves)
    std::function<void(pugi::xml_node, const std::string&)> collect_files =
        [&](pugi::xml_node node, const std::string& filter_path) {
            for (auto child : node.children()) {
                std::string node_name = child.name();

                if (node_name == "Filter") {
                    std::string name = child.attribute("Name").as_string();
                    std::string sub_path = filter_path;
                    if (!name.empty()) {
                        sub_path = filter_path.empty() ? name : filter_path + "\\" + name;
                    }
                    collect_files(child, sub_path);
                    continue;
                }

                if (node_name != "File") continue;

                SourceFile src;
                src.path = clean_relative_path(child.attribute("RelativePath").as_string());
                if (src.path.empty()) continue;
                src.type = get_file_type(src.path);
                src.filter = filter_path;

                bool has_custom_build = false;
                for (auto file_cfg : child.children("FileConfiguration")) {
                    std::string config_key = file_cfg.attribute("Name").as_string();
                    if (config_key.empty()) continue;

                    if (auto a = file_cfg.attribute("ExcludedFromBuild"))
                        src.settings.excluded[config_key] = a.as_bool();

                    for (auto tool : file_cfg.children("Tool")) {
                        std::string tool_name = tool.attribute("Name").as_string();
                        if (tool_name == "VCCLCompilerTool") {
                            read_file_compiler_overrides(tool, src, config_key, format_major_version);
                        } else if (tool_name == "VCCustomBuildTool") {
                            if (auto a = tool.attribute("CommandLine")) {
                                src.custom_command[config_key] = translate_file_macros(a.as_string());
                                has_custom_build = true;
                            }
                            if (auto a = tool.attribute("Description"))
                                src.custom_message[config_key] = a.as_string();
                            if (auto a = tool.attribute("Outputs"))
                                src.custom_outputs[config_key] = translate_file_macros(a.as_string());
                            if (auto a = tool.attribute("AdditionalDependencies"))
                                src.custom_inputs[config_key] = translate_file_macros(a.as_string());
                        }
                    }
                }

                if (has_custom_build) {
                    src.type = FileType::CustomBuild;
                } else if (src.type == FileType::MASM) {
                    project.has_masm_files = true;
                } else if (src.type == FileType::Midl) {
                    project.has_idl_files = true;
                } else if (src.type == FileType::MessageCompile) {
                    project.has_mc_files = true;
                }

                project.sources.push_back(src);
            }
        };

    collect_files(root.child("Files"), "");

    // Project references (VS2005/2008 keep them in <References>)
    for (auto ref : root.child("References").children("ProjectReference")) {
        std::string rel_path = ref.attribute("RelativePathToProject").as_string();
        if (rel_path.empty()) continue;
        std::string dep_name = fs::path(clean_relative_path(rel_path)).stem().string();
        if (!dep_name.empty()) {
            project.project_references.push_back(ProjectDependency(dep_name));
        }
    }

    // Detect project language from CompileAs settings and file extensions
    if (project.language.empty()) {
        int c_count = 0;
        int cpp_count = 0;

        for (const auto& src : project.sources) {
            for (const auto& [config, compile_as] : src.settings.compile_as) {
                (void)config;
                if (compile_as == "CompileAsC") c_count++;
                else if (compile_as == "CompileAsCpp") cpp_count++;
            }

            std::string ext = to_lower(fs::path(src.path).extension().string());
            if (ext == ".c") c_count++;
            else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") cpp_count++;
        }

        if (c_count > 0 && cpp_count == 0) {
            project.language = "C";
        } else if (cpp_count > 0) {
            project.language = "C++";
        }
    }

    return project;
}

} // namespace vcxproj
