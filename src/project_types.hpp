#pragma once
#ifndef PROJECT_TYPES_HPP
#define PROJECT_TYPES_HPP

#include <string>
#include <vector>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cctype>
#include <algorithm>

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
};

// Compiler settings
struct ClCompileSettings {
    std::string optimization;                           // "Disabled", "MaxSpeed", etc.
    std::vector<std::string> additional_include_directories;
    std::vector<std::string> preprocessor_definitions;
    std::string runtime_library;                        // "MultiThreaded", "MultiThreadedDebug", etc.
    std::string debug_information_format;               // "EditAndContinue", "ProgramDatabase", etc.
    bool function_level_linking = false;
    bool intrinsic_functions = false;
    std::string warning_level;                          // "Level0" to "Level4"
    std::vector<std::string> disable_specific_warnings;
    std::string additional_options;
    PrecompiledHeader pch;
    std::string language_standard;                      // "stdcpp14", "stdcpp17", "stdcpp20"
    std::string exception_handling;                     // "false", "Sync", "Async"
    bool runtime_type_info = true;
    bool multi_processor_compilation = false;
    std::string enhanced_instruction_set;               // "NotSet", "StreamingSIMDExtensions2", "AdvancedVectorExtensions2"
    std::string floating_point_model;                   // "Precise", "Fast", "Strict"
};

// Linker settings
struct LinkSettings {
    std::string sub_system;                             // "Console", "Windows"
    bool generate_debug_info = false;
    std::vector<std::string> additional_dependencies;
    std::vector<std::string> additional_library_directories;
    std::vector<std::string> ignore_specific_default_libraries;
    bool enable_comdat_folding = false;
    bool references = false;                            // Optimize references
    std::string additional_options;
};

// Build event
struct BuildEvent {
    std::string command;
    std::string message;
};

// Project configuration (e.g., Debug|Win32, Release|x64)
struct Configuration {
    std::string config_type;                            // "Application", "StaticLibrary", "DynamicLibrary"
    std::string platform_toolset;                       // "v143", "v142", etc.
    std::string windows_target_platform_version;        // "10.0", "10.0.19041.0", etc.
    std::string character_set;                          // "MultiByte", "Unicode"
    bool use_debug_libraries = false;
    std::string out_dir;                                // Output directory
    std::string int_dir;                                // Intermediate directory
    std::string target_name;                            // Target file name (without extension)
    std::string target_ext;                             // Target extension (.exe, .dll, .lib)
    bool link_incremental = false;

    ClCompileSettings cl_compile;
    LinkSettings link;

    BuildEvent pre_build_event;
    BuildEvent pre_link_event;
    BuildEvent post_build_event;
};

// Project
struct Project {
    std::string name;
    std::string uuid;
    std::string root_namespace;

    std::vector<SourceFile> sources;
    std::vector<LibraryFile> libraries;
    std::vector<std::string> project_references;        // Names of dependent projects

    std::map<std::string, Configuration> configurations; // Key is "Config|Platform"
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

#endif // PROJECT_TYPES_HPP
