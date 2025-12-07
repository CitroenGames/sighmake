#include "buildscript_parser.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

namespace vcxproj {

std::string BuildscriptParser::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::vector<std::string> BuildscriptParser::split(const std::string& str, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim)) {
        std::string trimmed = trim(token);
        if (!trimmed.empty()) {
            tokens.push_back(trimmed);
        }
    }
    return tokens;
}

std::vector<std::string> BuildscriptParser::expand_wildcards(const std::string& pattern, 
                                                              const std::string& base_path) {
    std::vector<std::string> result;
    
    fs::path full_pattern = fs::path(base_path) / pattern;
    std::string dir = full_pattern.parent_path().string();
    std::string file_pattern = full_pattern.filename().string();
    
    if (dir.empty()) dir = ".";
    
    // Check if this is a wildcard pattern
    if (file_pattern.find('*') == std::string::npos) {
        // Not a wildcard, just return the path
        result.push_back(pattern);
        return result;
    }
    
    // Convert wildcard to regex
    std::string regex_pattern = file_pattern;
    // Escape special regex characters except *
    for (size_t i = 0; i < regex_pattern.size(); ++i) {
        char c = regex_pattern[i];
        if (c == '.' || c == '+' || c == '?' || c == '[' || c == ']' || 
            c == '(' || c == ')' || c == '{' || c == '}' || c == '^' || c == '$') {
            regex_pattern.insert(i, "\\");
            ++i;
        } else if (c == '*') {
            regex_pattern.replace(i, 1, ".*");
            ++i;
        }
    }
    
    std::regex re(regex_pattern, std::regex::icase);
    
    try {
        if (fs::exists(dir)) {
            // Check if pattern includes ** for recursive search
            bool recursive = pattern.find("**") != std::string::npos;
            
            if (recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (std::regex_match(filename, re)) {
                            // Make path relative to base_path
                            std::string rel_path = fs::relative(entry.path(), base_path).string();
                            result.push_back(rel_path);
                        }
                    }
                }
            } else {
                for (const auto& entry : fs::directory_iterator(dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (std::regex_match(filename, re)) {
                            // Reconstruct path with original directory
                            fs::path p = fs::path(pattern).parent_path() / filename;
                            result.push_back(p.string());
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Error expanding wildcard " << pattern << ": " << e.what() << "\n";
    }
    
    return result;
}

SourceFile* BuildscriptParser::find_or_create_source(const std::string& path, ParseState& state) {
    if (!state.current_project) return nullptr;
    
    // Look for existing entry
    for (auto& src : state.current_project->sources) {
        if (src.path == path) {
            return &src;
        }
    }
    
    // Create new entry
    state.current_project->sources.emplace_back();
    auto& src = state.current_project->sources.back();
    src.path = path;
    src.type = get_file_type(path);
    return &src;
}

Solution BuildscriptParser::parse(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open buildscript: " + filepath);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    
    fs::path base = fs::path(filepath).parent_path();
    return parse_string(buffer.str(), base.empty() ? "." : base.string());
}

Solution BuildscriptParser::parse_string(const std::string& content, const std::string& base_path) {
    Solution solution;
    solution.uuid = generate_uuid();
    solution.configurations = {"Debug", "Release"};
    solution.platforms = {"Win32"};
    
    ParseState state;
    state.solution = &solution;
    state.base_path = base_path;
    
    std::istringstream stream(content);
    std::string line;
    
    while (std::getline(stream, line)) {
        state.line_number++;
        parse_line(line, state);
    }
    
    // Set default solution name if not specified
    if (solution.name.empty() && !solution.projects.empty()) {
        solution.name = solution.projects[0].name;
    }
    
    // Ensure all projects have configurations set up
    for (auto& project : solution.projects) {
        for (const auto& config_key : solution.get_config_keys()) {
            auto& cfg = project.configurations[config_key];
            auto [config, platform] = parse_config_key(config_key);

            // Set defaults if not already set
            if (cfg.platform_toolset.empty()) {
                cfg.platform_toolset = "v143";
            }
            if (cfg.windows_target_platform_version.empty()) {
                cfg.windows_target_platform_version = "10.0";
            }
            if (cfg.character_set.empty()) {
                cfg.character_set = "MultiByte";
            }

            // Set use_debug_libraries based on config name if not explicitly set
            if (config == "Debug") {
                cfg.use_debug_libraries = true;
            }

            // Set default optimization and other settings if not already set
            if (cfg.cl_compile.optimization.empty()) {
                if (config == "Debug") {
                    cfg.cl_compile.optimization = "Disabled";
                } else {
                    cfg.cl_compile.optimization = "MaxSpeed";
                }
            }

            if (cfg.cl_compile.runtime_library.empty()) {
                if (config == "Debug") {
                    cfg.cl_compile.runtime_library = "MultiThreadedDebug";
                } else {
                    cfg.cl_compile.runtime_library = "MultiThreaded";
                }
            }

            if (cfg.cl_compile.debug_information_format.empty()) {
                if (config == "Debug") {
                    cfg.cl_compile.debug_information_format = "EditAndContinue";
                } else {
                    cfg.cl_compile.debug_information_format = "ProgramDatabase";
                }
            }

            if (config == "Debug") {
                cfg.link.generate_debug_info = true;
            } else {
                cfg.cl_compile.function_level_linking = true;
                cfg.cl_compile.intrinsic_functions = true;
                cfg.link.enable_comdat_folding = true;
                cfg.link.references = true;
            }
        }
    }
    
    return solution;
}

void BuildscriptParser::parse_line(const std::string& line, ParseState& state) {
    std::string trimmed = trim(line);
    
    // Skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
        return;
    }
    
    // Check for section headers
    if (trimmed[0] == '[' && trimmed.back() == ']') {
        parse_section(trimmed, state);
        return;
    }
    
    // Parse key=value pairs
    size_t eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
        std::cerr << "Warning: Invalid line " << state.line_number << ": " << trimmed << "\n";
        return;
    }
    
    std::string key = trim(trimmed.substr(0, eq_pos));
    std::string value = trim(trimmed.substr(eq_pos + 1));
    
    parse_key_value(key, value, state);
}

bool BuildscriptParser::parse_section(const std::string& line, ParseState& state) {
    std::string section = line.substr(1, line.length() - 2);
    
    if (section == "solution") {
        state.current_project = nullptr;
        state.current_file = nullptr;
        return true;
    }
    
    // [project:name]
    if (section.rfind("project:", 0) == 0) {
        state.solution->projects.emplace_back();
        state.current_project = &state.solution->projects.back();
        state.current_project->name = trim(section.substr(8));
        state.current_project->uuid = generate_uuid();
        state.current_project->root_namespace = state.current_project->name;
        state.current_file = nullptr;
        return true;
    }
    
    // [file:path] - for per-file settings block
    if (section.rfind("file:", 0) == 0) {
        std::string file_path = trim(section.substr(5));
        state.current_file = find_or_create_source(file_path, state);
        return true;
    }
    
    // [config:Debug|Win32] - for config-specific settings
    if (section.rfind("config:", 0) == 0) {
        // This sets context for following settings
        return true;
    }
    
    std::cerr << "Warning: Unknown section '" << section << "' at line " << state.line_number << "\n";
    return false;
}

void BuildscriptParser::parse_key_value(const std::string& key, const std::string& value, 
                                         ParseState& state) {
    // Check for per-file settings: file.cpp:setting = value
    // Or: file.cpp:setting[Debug|Win32] = value
    size_t colon_pos = key.find(':');
    if (colon_pos != std::string::npos && colon_pos > 0) {
        std::string file_path = trim(key.substr(0, colon_pos));
        std::string rest = trim(key.substr(colon_pos + 1));
        
        // Check for config specifier
        std::string config_key = ALL_CONFIGS;
        size_t bracket_start = rest.find('[');
        size_t bracket_end = rest.find(']');
        std::string setting = rest;
        
        if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
            config_key = rest.substr(bracket_start + 1, bracket_end - bracket_start - 1);
            setting = trim(rest.substr(0, bracket_start));
        }
        
        parse_file_setting(file_path, setting, config_key, value, state);
        return;
    }
    
    // Check for config-specific project setting: setting[Debug|Win32] = value
    size_t bracket_start = key.find('[');
    size_t bracket_end = key.find(']');
    if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
        std::string config_key = key.substr(bracket_start + 1, bracket_end - bracket_start - 1);
        std::string setting = trim(key.substr(0, bracket_start));
        parse_config_setting(setting, value, config_key, state);
        return;
    }
    
    // Regular key=value
    if (state.current_project == nullptr) {
        parse_solution_setting(key, value, state);
    } else if (state.current_file != nullptr) {
        parse_file_setting(state.current_file->path, key, ALL_CONFIGS, value, state);
    } else {
        parse_project_setting(key, value, state);
    }
}

void BuildscriptParser::parse_solution_setting(const std::string& key, const std::string& value,
                                                ParseState& state) {
    if (key == "name") {
        state.solution->name = value;
    } else if (key == "configurations" || key == "configs") {
        state.solution->configurations = split(value, ',');
    } else if (key == "platforms") {
        state.solution->platforms = split(value, ',');
    }
}

void BuildscriptParser::parse_project_setting(const std::string& key, const std::string& value,
                                               ParseState& state) {
    if (!state.current_project) return;
    
    Project& proj = *state.current_project;
    
    // Basic project settings
    if (key == "name") {
        proj.name = value;
    } else if (key == "uuid" || key == "guid") {
        proj.uuid = value;
    } else if (key == "type") {
        std::string config_type;
        if (value == "exe" || value == "application" || value == "Application") {
            config_type = "Application";
        } else if (value == "lib" || value == "static" || value == "staticlib" || value == "StaticLibrary") {
            config_type = "StaticLibrary";
        } else if (value == "dll" || value == "shared" || value == "dynamiclib" || value == "DynamicLibrary") {
            config_type = "DynamicLibrary";
        } else {
            config_type = value;
        }
        // Apply to all configurations
        for (auto& [_, cfg] : proj.configurations) {
            cfg.config_type = config_type;
        }
        // Also set default for new configs
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].config_type = config_type;
        }
    } else if (key == "toolset" || key == "platform_toolset") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].platform_toolset = value;
        }
    } else if (key == "windows_sdk" || key == "windows_sdk_version" || key == "windows_target_platform_version") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].windows_target_platform_version = value;
        }
    } else if (key == "charset" || key == "character_set") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].character_set = value;
        }
    } else if (key == "target_name" || key == "targetname") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].target_name = value;
        }
    } else if (key == "target_ext" || key == "targetext") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].target_ext = value;
        }
    } else if (key == "outdir" || key == "output_dir") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].out_dir = value;
        }
    } else if (key == "intdir" || key == "intermediate_dir") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].int_dir = value;
        }
    }
    // Source files
    else if (key == "sources" || key == "src" || key == "files") {
        for (const auto& src : split(value, ',')) {
            auto expanded = expand_wildcards(src, state.base_path);
            if (expanded.empty()) {
                // If no expansion, add as-is
                find_or_create_source(src, state);
            } else {
                for (const auto& path : expanded) {
                    find_or_create_source(path, state);
                }
            }
        }
    } else if (key == "headers" || key == "includes_files") {
        for (const auto& src : split(value, ',')) {
            auto expanded = expand_wildcards(src, state.base_path);
            for (const auto& path : expanded) {
                auto* file = find_or_create_source(path, state);
                if (file) file->type = FileType::ClInclude;
            }
        }
    } else if (key == "libs" || key == "libraries") {
        auto libs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& deps = proj.configurations[config_key].link.additional_dependencies;
            deps.insert(deps.end(), libs.begin(), libs.end());
        }
        // Also keep for backwards compatibility
        for (const auto& lib : libs) {
            LibraryFile lf;
            lf.path = lib;
            proj.libraries.push_back(lf);
        }
    }
    // Compiler settings (apply to all configs)
    else if (key == "includes" || key == "include_dirs" || key == "additional_include_directories") {
        auto dirs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& includes = proj.configurations[config_key].cl_compile.additional_include_directories;
            includes.insert(includes.end(), dirs.begin(), dirs.end());
        }
    } else if (key == "defines" || key == "preprocessor" || key == "preprocessor_definitions") {
        auto defs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& defines = proj.configurations[config_key].cl_compile.preprocessor_definitions;
            defines.insert(defines.end(), defs.begin(), defs.end());
        }
    } else if (key == "std" || key == "cpp_standard" || key == "language_standard") {
        std::string std_value = "stdcpp" + value;
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.language_standard = std_value;
        }
    } else if (key == "cflags" || key == "compiler_flags" || key == "additional_options") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& opts = proj.configurations[config_key].cl_compile.additional_options;
            if (!opts.empty()) opts += " ";
            opts += value;
        }
    } else if (key == "warning_level") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.warning_level = value;
        }
    } else if (key == "disable_warnings" || key == "disable_specific_warnings") {
        auto warnings = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& disabled = proj.configurations[config_key].cl_compile.disable_specific_warnings;
            disabled.insert(disabled.end(), warnings.begin(), warnings.end());
        }
    } else if (key == "exception_handling" || key == "exceptions") {
        std::string eh_value = value;
        if (value == "false" || value == "no" || value == "0") eh_value = "false";
        else if (value == "true" || value == "yes" || value == "sync") eh_value = "Sync";
        else if (value == "async") eh_value = "Async";
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.exception_handling = eh_value;
        }
    } else if (key == "rtti" || key == "runtime_type_info") {
        bool rtti = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.runtime_type_info = rtti;
        }
    } else if (key == "multiprocessor" || key == "mp" || key == "multi_processor_compilation") {
        bool mp = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.multi_processor_compilation = mp;
        }
    } else if (key == "sse" || key == "simd" || key == "enhanced_instruction_set") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.enhanced_instruction_set = value;
        }
    } else if (key == "floating_point" || key == "fp_model" || key == "floating_point_model") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.floating_point_model = value;
        }
    }
    // PCH settings
    else if (key == "pch" || key == "precompiled_header") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.pch.mode = value;
        }
    } else if (key == "pch_header" || key == "precompiled_header_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.pch.header = value;
        }
    } else if (key == "pch_output" || key == "precompiled_header_output_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.pch.output = value;
        }
    }
    // Linker settings
    else if (key == "ldflags" || key == "linker_flags" || key == "link_options") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& opts = proj.configurations[config_key].link.additional_options;
            if (!opts.empty()) opts += " ";
            opts += value;
        }
    } else if (key == "libdirs" || key == "lib_dirs" || key == "additional_library_directories") {
        auto dirs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& libdirs = proj.configurations[config_key].link.additional_library_directories;
            libdirs.insert(libdirs.end(), dirs.begin(), dirs.end());
        }
    } else if (key == "link_libs" || key == "additional_dependencies") {
        auto libs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& deps = proj.configurations[config_key].link.additional_dependencies;
            deps.insert(deps.end(), libs.begin(), libs.end());
        }
    } else if (key == "subsystem" || key == "sub_system") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.sub_system = value;
        }
    } else if (key == "ignore_libs" || key == "ignore_specific_default_libraries") {
        auto libs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& ignored = proj.configurations[config_key].link.ignore_specific_default_libraries;
            ignored.insert(ignored.end(), libs.begin(), libs.end());
        }
    }
    // Build events
    else if (key == "prebuild" || key == "pre_build_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_build_event.command = value;
        }
    } else if (key == "prelink" || key == "pre_link_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_link_event.command = value;
        }
    } else if (key == "postbuild" || key == "post_build_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].post_build_event.command = value;
        }
    }
    // Project references
    else if (key == "depends" || key == "dependencies" || key == "project_references") {
        auto deps = split(value, ',');
        proj.project_references.insert(proj.project_references.end(), deps.begin(), deps.end());
    }
}

void BuildscriptParser::parse_file_setting(const std::string& file_path, const std::string& setting,
                                           const std::string& config_key, const std::string& value,
                                           ParseState& state) {
    SourceFile* file = find_or_create_source(file_path, state);
    if (!file) return;
    
    if (setting == "includes" || setting == "include_dirs" || setting == "additional_include_directories") {
        auto dirs = split(value, ',');
        auto& target = file->settings.additional_includes[config_key];
        target.insert(target.end(), dirs.begin(), dirs.end());
    } else if (setting == "defines" || setting == "preprocessor" || setting == "preprocessor_definitions") {
        auto defs = split(value, ',');
        auto& target = file->settings.preprocessor_defines[config_key];
        target.insert(target.end(), defs.begin(), defs.end());
    } else if (setting == "flags" || setting == "cflags" || setting == "additional_options") {
        auto flags = split(value, ',');
        auto& target = file->settings.additional_options[config_key];
        target.insert(target.end(), flags.begin(), flags.end());
    } else if (setting == "pch" || setting == "precompiled_header") {
        file->settings.pch[config_key].mode = value;
    } else if (setting == "pch_header" || setting == "precompiled_header_file") {
        file->settings.pch[config_key].header = value;
    } else if (setting == "pch_output" || setting == "precompiled_header_output_file") {
        file->settings.pch[config_key].output = value;
    } else if (setting == "exclude" || setting == "excluded" || setting == "excluded_from_build") {
        bool excluded = (value == "true" || value == "yes" || value == "1");
        file->settings.excluded[config_key] = excluded;
    } else if (setting == "object_file" || setting == "object_file_name") {
        file->settings.object_file[config_key] = value;
    } else if (setting == "compile_as") {
        file->settings.compile_as[config_key] = value;
    }
    // Custom build settings
    else if (setting == "custom_command" || setting == "command") {
        file->custom_command[config_key] = value;
        if (file->type == FileType::ClCompile || file->type == FileType::ClInclude) {
            file->type = FileType::CustomBuild;
        }
    } else if (setting == "custom_message" || setting == "message") {
        file->custom_message[config_key] = value;
    } else if (setting == "custom_outputs" || setting == "outputs") {
        file->custom_outputs[config_key] = value;
    } else if (setting == "custom_inputs" || setting == "inputs" || setting == "additional_inputs") {
        file->custom_inputs[config_key] = value;
    }
}

void BuildscriptParser::parse_config_setting(const std::string& key, const std::string& value,
                                              const std::string& config_key, ParseState& state) {
    if (!state.current_project) return;
    
    auto& cfg = state.current_project->configurations[config_key];
    
    // Configuration-specific settings
    if (key == "optimization") {
        cfg.cl_compile.optimization = value;
    } else if (key == "runtime_library") {
        cfg.cl_compile.runtime_library = value;
    } else if (key == "debug_info" || key == "debug_information_format") {
        cfg.cl_compile.debug_information_format = value;
    } else if (key == "toolset" || key == "platform_toolset") {
        cfg.platform_toolset = value;
    } else if (key == "windows_sdk" || key == "windows_sdk_version" || key == "windows_target_platform_version") {
        cfg.windows_target_platform_version = value;
    } else if (key == "outdir" || key == "output_dir") {
        cfg.out_dir = value;
    } else if (key == "intdir" || key == "intermediate_dir") {
        cfg.int_dir = value;
    } else if (key == "includes" || key == "additional_include_directories") {
        auto dirs = split(value, ',');
        cfg.cl_compile.additional_include_directories.insert(
            cfg.cl_compile.additional_include_directories.end(), dirs.begin(), dirs.end());
    } else if (key == "defines" || key == "preprocessor_definitions") {
        auto defs = split(value, ',');
        cfg.cl_compile.preprocessor_definitions.insert(
            cfg.cl_compile.preprocessor_definitions.end(), defs.begin(), defs.end());
    } else if (key == "libs" || key == "additional_dependencies") {
        auto libs = split(value, ',');
        cfg.link.additional_dependencies.insert(
            cfg.link.additional_dependencies.end(), libs.begin(), libs.end());
    } else if (key == "libdirs" || key == "lib_dirs" || key == "additional_library_directories") {
        auto dirs = split(value, ',');
        cfg.link.additional_library_directories.insert(
            cfg.link.additional_library_directories.end(), dirs.begin(), dirs.end());
    } else if (key == "link_incremental") {
        cfg.link_incremental = (value == "true" || value == "yes" || value == "1");
    } else if (key == "generate_debug_info") {
        cfg.link.generate_debug_info = (value == "true" || value == "yes" || value == "1");
    }
    // Also support the same keys as project settings
    else {
        // Forward to project setting parser but it will only affect this config
        // This is a simplified approach - in production you'd want more granular control
    }
}

} // namespace vcxproj
