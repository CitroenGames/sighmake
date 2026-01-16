#pragma once

namespace vcxproj {

// Constant for "all configurations"
constexpr const char* ALL_CONFIGS = "*";

// File types in Visual Studio projects
enum class FileType {
    ClCompile,      // .cpp files
    ClInclude,      // .h, .hpp files
    CustomBuild,    // Files with custom build rules
    None,           // Other files
    ResourceCompile // .rc files
};

// Precompiled header settings
struct PrecompiledHeader {
    std::string mode;    // "Use", "Create", "NotUsing"
    std::string header;  // Header file name
    std::string output;  // Output file path
};

// File-specific settings
struct FileSettings {
    std::map<std::string, std::vector<std::string>> additional_includes;  // Per-config
    std::map<std::string, std::vector<std::string>> preprocessor_defines; // Per-config
    std::map<std::string, std::vector<std::string>> additional_options;   // Per-config
    std::map<std::string, PrecompiledHeader> pch;                         // Per-config
    std::map<std::string, bool> excluded;                                 // Per-config
    std::map<std::string, std::string> object_file;                       // Per-config
    std::map<std::string, std::string> compile_as;                        // Per-config
};

// Source file entry
struct SourceFile {
    std::string path;
    FileType type = FileType::ClCompile;
    FileSettings settings;

    // Custom build tool settings
    std::map<std::string, std::string> custom_command;  // Per-config
    std::map<std::string, std::string> custom_message;  // Per-config
    std::map<std::string, std::string> custom_outputs;  // Per-config
    std::map<std::string, std::string> custom_inputs;   // Per-config
};

// Library file reference
struct LibraryFile {
    std::string path;
    std::map<std::string, bool> excluded;  // Per-config
};

// Compiler settings
struct ClCompileSettings {
    std::string optimization;                           // "Disabled", "MaxSpeed", "MinSpace", "Full"
    std::string inline_function_expansion;              // "Default", "Disabled", "OnlyExplicitInline", "AnySuitable"
    bool intrinsic_functions = false;
    std::string favor_size_or_speed;                    // "Neither", "Speed", "Size"
    std::vector<std::string> additional_include_directories;
    std::vector<std::string> preprocessor_definitions;
    std::vector<std::string> forced_include_files;          // ForcedIncludeFiles
    bool string_pooling = false;
    bool minimal_rebuild = false;
    std::string exception_handling;                     // "false", "Sync", "Async"
    std::string basic_runtime_checks;                   // "Default", "StackFrameRuntimeCheck", "UninitVariables", "EnableFastChecks"
    std::string runtime_library;                        // "MultiThreaded", "MultiThreadedDebug", etc.
    bool buffer_security_check = true;
    bool function_level_linking = false;
    std::string enhanced_instruction_set;               // "NotSet", "StreamingSIMDExtensions2", "AdvancedVectorExtensions2"
    std::string floating_point_model;                   // "Precise", "Fast", "Strict"
    bool force_conformance_in_for_loop_scope = true;
    bool runtime_type_info = true;
    PrecompiledHeader pch;
    std::string assembler_listing_location;
    std::string object_file_name;
    std::string program_database_file_name;
    bool generate_xml_documentation_files = false;
    bool browse_information = false;
    std::string browse_information_file;
    std::string warning_level;                          // "Level0" to "Level4"
    std::string debug_information_format;               // "EditAndContinue", "ProgramDatabase", etc.
    std::string compile_as;                             // "Default", "CompileAsC", "CompileAsCpp"
    std::vector<std::string> disable_specific_warnings;
    bool multi_processor_compilation = false;
    std::string error_reporting;                        // "None", "Prompt", "Queue", "Send"
    std::string additional_options;
    std::string language_standard;                      // "stdcpp14", "stdcpp17", "stdcpp20"
    bool treat_wchar_t_as_built_in_type = true;        // TreatWChar_tAsBuiltInType
    std::string assembler_output;                       // "NoListing", "AssemblyCode", "All", etc.
    bool expand_attributed_source = false;              // ExpandAttributedSource
    bool openmp_support = false;                        // OpenMPSupport
    bool treat_warning_as_error = false;                // TreatWarningAsError
};

// Linker settings
struct LinkSettings {
    std::string show_progress;                          // "NotSet", "LinkVerbose", "LinkVerboseLib"
    std::string output_file;                            // Custom output file path
    bool suppress_startup_banner = false;
    std::vector<std::string> additional_dependencies;
    std::vector<std::string> additional_library_directories;
    std::vector<std::string> ignore_specific_default_libraries;
    bool generate_debug_info = false;
    std::string program_database_file;                  // Custom .pdb file path
    std::string sub_system;                             // "Console", "Windows"
    bool optimize_references = false;
    bool enable_comdat_folding = false;
    std::string base_address;                           // DLL base address
    std::string target_machine;                         // "MachineX86", "MachineX64"
    std::string error_reporting;                        // "PromptImmediately", "QueueForNextLogin", "SendErrorReport", "NoErrorReport"
    bool image_has_safe_exception_handlers = false;
    std::string additional_options;
    std::string entry_point_symbol;                     // Entry point for executables (e.g., "mainCRTStartup")
    std::string version;                                // Version number (e.g., "1.1")
    bool generate_map_file = false;                     // Generate .map file
    std::string map_file_name;                          // Custom .map file path
    bool fixed_base_address = false;                    // FixedBaseAddress
    bool large_address_aware = false;                   // LargeAddressAware
};

// Librarian settings (for static library projects)
struct LibrarianSettings {
    std::string output_file;                            // Custom output .lib file path
    bool suppress_startup_banner = false;
    bool use_unicode_response_files = false;
    std::string additional_options;
    std::vector<std::string> additional_dependencies;   // Additional libs to embed (e.g., Rpcrt4.lib)
};

// Build event
struct BuildEvent {
    std::string command;
    std::string message;
    bool use_in_build = true;
};

// Resource compiler settings
struct ResourceCompileSettings {
    std::vector<std::string> preprocessor_definitions;
    std::string culture;                                // "1033" for US English, etc.
    std::vector<std::string> additional_include_directories;
};

// Manifest tool settings
struct ManifestSettings {
    bool suppress_startup_banner = false;
    std::string additional_manifest_files;              // Additional manifest files to merge
};

// XML Documentation settings
struct XdcmakeSettings {
    bool suppress_startup_banner = false;
};

// Browse Information Maintenance Utility settings
struct BscmakeSettings {
    bool suppress_startup_banner = false;
    std::string output_file;
};

// Project configuration (e.g., Debug|Win32, Release|x64)
struct Configuration {
    std::string config_type;                            // "Application", "StaticLibrary", "DynamicLibrary"
    std::string platform_toolset;                       // "v143", "v142", etc.
    std::string windows_target_platform_version;        // "10.0", "10.0.19041.0", etc.
    std::string character_set;                          // "MultiByte", "Unicode"
    bool use_debug_libraries = false;
    bool whole_program_optimization = false;
    std::string use_of_mfc;                             // "false", "Static", "Dynamic"
    std::string use_of_atl;                             // "false", "Static", "Dynamic"
    std::string out_dir;                                // Output directory
    std::string int_dir;                                // Intermediate directory
    std::string target_name;                            // Target file name (without extension)
    std::string target_ext;                             // Target extension (.exe, .dll, .lib)
    bool link_incremental = false;
    std::string executable_path;                        // Additional executable search paths
    bool generate_manifest = true;
    bool ignore_import_library = false;                 // IgnoreImportLibrary for DLLs
    std::string import_library;                         // Custom import library path

    ClCompileSettings cl_compile;
    LinkSettings link;
    LibrarianSettings lib;
    ResourceCompileSettings resource_compile;

    BuildEvent pre_build_event;
    BuildEvent pre_link_event;
    BuildEvent post_build_event;

    ManifestSettings manifest;
    XdcmakeSettings xdcmake;
    BscmakeSettings bscmake;

    // Template inheritance
    std::string template_name;  // Name of template this config inherits from (e.g., "Release")
    bool is_template = false;   // Whether this is used as a template by other configs
};

// Dependency visibility for transitive propagation (CMake-style)
enum class DependencyVisibility {
    PUBLIC,      // Affects target and all dependents (transitive)
    PRIVATE,     // Affects only the target (non-transitive)
    INTERFACE    // Affects dependents only, not the target itself
};

// Represents a dependency on another project with visibility information
struct ProjectDependency {
    std::string name;                          // Name of the dependent project
    DependencyVisibility visibility = DependencyVisibility::PUBLIC;  // Default for backward compat

    // Constructors for convenience
    ProjectDependency() = default;
    ProjectDependency(const std::string& n, DependencyVisibility v = DependencyVisibility::PUBLIC)
        : name(n), visibility(v) {}
};

// Helper to parse visibility keyword to enum
inline DependencyVisibility parse_visibility(const std::string& keyword) {
    if (keyword == "PRIVATE") return DependencyVisibility::PRIVATE;
    if (keyword == "INTERFACE") return DependencyVisibility::INTERFACE;
    return DependencyVisibility::PUBLIC;  // Default: PUBLIC
}

// Helper to convert visibility enum to string (for debugging/output)
inline std::string visibility_to_string(DependencyVisibility vis) {
    switch (vis) {
        case DependencyVisibility::PRIVATE: return "PRIVATE";
        case DependencyVisibility::INTERFACE: return "INTERFACE";
        case DependencyVisibility::PUBLIC: return "PUBLIC";
        default: return "PUBLIC";
    }
}

// Project
struct Project {
    std::string name;
    std::string project_name;                           // Custom display name (ProjectName in vcxproj)
    std::string uuid;
    std::string root_namespace;
    bool ignore_warn_compile_duplicated_filename = false;
    std::string vcxproj_path;                           // Original .vcxproj file path (for reverse conversion)
    std::string buildscript_path;                       // Buildscript file path (for path resolution in custom commands)

    std::vector<SourceFile> sources;
    std::vector<LibraryFile> libraries;
    std::vector<ProjectDependency> project_references;  // Structured dependencies with visibility

    std::map<std::string, Configuration> configurations; // Key is "Config|Platform"

    // Project-level settings that apply to ALL configurations
    // These are stored separately and applied post-parsing to handle cases where
    // configurations are discovered after project settings are parsed (e.g., when
    // including multiple buildscripts, configs from later projects affect all projects)
    std::vector<std::string> project_level_preprocessor_definitions;

    // Public properties that propagate to dependent projects via target_link_libraries
    std::vector<std::string> public_includes;   // Include directories exposed to dependents
    std::vector<std::string> public_libs;       // Libraries exposed to dependents
    std::vector<std::string> public_defines;    // Preprocessor definitions exposed to dependents
};

// Solution
struct Solution {
    std::string name;
    std::string uuid;
    std::vector<std::string> configurations;            // ["Debug", "Release"]
    std::vector<std::string> platforms;                 // ["Win32", "x64"]
    std::vector<Project> projects;

    // Get all configuration keys (e.g., "Debug|Win32", "Release|x64")
    std::vector<std::string> get_config_keys() const {
        std::vector<std::string> keys;
        for (const auto& config : configurations) {
            for (const auto& platform : platforms) {
                keys.push_back(config + "|" + platform);
            }
        }
        return keys;
    }
};

// Helper function to generate UUIDs
inline std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::uppercase << std::hex;

    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4"; // Version 4
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen); // Variant
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);

    return ss.str();
}

// Helper function to get file type from extension
inline FileType get_file_type(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();

    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c") {
        return FileType::ClCompile;
    } else if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
        return FileType::ClInclude;
    } else if (ext == ".rc") {
        return FileType::ResourceCompile;
    }

    return FileType::None;
}

// Helper function to parse config key "Debug|Win32" into (config, platform)
inline std::pair<std::string, std::string> parse_config_key(const std::string& key) {
    size_t pos = key.find('|');
    if (pos == std::string::npos) {
        return {key, "Win32"};
    }
    return {key.substr(0, pos), key.substr(pos + 1)};
}

} // namespace vcxproj
