#include "pch.h"
#include "buildscript_parser.hpp"
#include "common/toolset_registry.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace vcxproj {

// Helper function to normalize paths using std::filesystem
static std::string normalize_path(const std::string& path) {
    if (path.empty()) return path;

    try {
        // Use filesystem to normalize the path
        // lexically_normal() removes redundant . and .. and simplifies the path
        fs::path p(path);
        std::string normalized = p.lexically_normal().string();

        // Preserve trailing slash/backslash if original had one
        if (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
            if (!normalized.empty() && normalized.back() != '/' && normalized.back() != '\\') {
                normalized += path.back();
            }
        }

        return normalized;
    } catch (...) {
        // If filesystem operation fails, return original path
        return path;
    }
}

std::string BuildscriptParser::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::string unescape_value(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
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

static std::string trim_local(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::string preprocess_multiline(const std::string& content) {
    std::string processed_content;
    std::istringstream preprocess_stream(content);
    std::string line;
    
    bool in_multiline_quote = false; // """
    bool in_brace_block = false;     // { }
    
    std::string multiline_accumulator;
    std::string multiline_prefix;
    std::vector<std::string> brace_items;

    while (std::getline(preprocess_stream, line)) {
        if (in_multiline_quote) {
            // Check if this line contains the closing """
            size_t close_pos = line.find("\"\"\"");
            if (close_pos != std::string::npos) {
                // End of multiline value
                multiline_accumulator += line.substr(0, close_pos);
                // Write accumulated multiline as escaped single line
                processed_content += multiline_prefix;
                // Escape newlines in accumulated value
                for (char c : multiline_accumulator) {
                    if (c == '\n') {
                        processed_content += "\\n";
                    } else if (c == '\\') {
                        processed_content += "\\\\";
                    } else {
                        processed_content += c;
                    }
                }
                processed_content += "\n";
                in_multiline_quote = false;
                multiline_accumulator.clear();
                multiline_prefix.clear();
            } else {
                // Continue accumulating
                multiline_accumulator += line + "\n";
            }
            continue;
        }

        if (in_brace_block) {
            // Remove comments for checking }
            std::string no_comment = line;
            size_t comment_pos = line.find('#');
            if (comment_pos != std::string::npos) no_comment = line.substr(0, comment_pos);
            
            std::string trimmed = trim_local(no_comment);
            
            // Check for closing brace
            // We assume } is the last significant char or the only one
            if (trimmed == "}" || (trimmed.size() > 0 && trimmed.back() == '}')) {
                 // If line is "file.cpp }", we should capture "file.cpp"
                 if (trimmed != "}") {
                     std::string item = trimmed.substr(0, trimmed.size() - 1);
                     item = trim_local(item);
                     if (!item.empty()) brace_items.push_back(item);
                 }
                 
                 // Join items
                 std::string joined;
                 for(size_t i=0; i<brace_items.size(); ++i) {
                     if (i > 0) joined += ",";
                     joined += brace_items[i];
                 }
                 
                 processed_content += multiline_prefix + joined + "\n";
                 in_brace_block = false;
                 brace_items.clear();
                 multiline_prefix.clear();
            } else {
                if (!trimmed.empty()) {
                    brace_items.push_back(trimmed);
                }
            }
            continue;
        }

        // Check for start of multiline value or brace block
        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string value_part = line.substr(eq_pos + 1);
            size_t first_nonspace = value_part.find_first_not_of(" \t");
            
            // Check for """
            if (first_nonspace != std::string::npos &&
                value_part.substr(first_nonspace, 3) == "\"\"\"") {
                // Start of multiline value
                in_multiline_quote = true;
                multiline_prefix = line.substr(0, eq_pos + 1) + " ";
                // Check if closing """ is on the same line
                size_t close_pos = value_part.find("\"\"\"", first_nonspace + 3);
                if (close_pos != std::string::npos) {
                    // Single line with """ ... """
                    multiline_accumulator = value_part.substr(first_nonspace + 3, close_pos - first_nonspace - 3);
                    processed_content += multiline_prefix;
                    for (char c : multiline_accumulator) {
                        if (c == '\n') {
                            processed_content += "\\n";
                        } else if (c == '\\') {
                            processed_content += "\\\\";
                        } else {
                            processed_content += c;
                        }
                    }
                    processed_content += "\n";
                    in_multiline_quote = false;
                    multiline_accumulator.clear();
                    multiline_prefix.clear();
                }
                continue;
            }
            
            // Check for {
            // Ignore comments in value part
            std::string no_comment = value_part;
            size_t comment_pos = value_part.find('#');
            if (comment_pos != std::string::npos) no_comment = value_part.substr(0, comment_pos);
            
            if (trim_local(no_comment) == "{") {
                in_brace_block = true;
                multiline_prefix = line.substr(0, eq_pos + 1) + " ";
                continue;
            }
        }
        
        // Normal line, pass through
        processed_content += line + "\n";
    }

    return processed_content;
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

std::pair<std::string, bool> BuildscriptParser::parse_filename_with_condition(const std::string& entry) {
    std::string trimmed = trim(entry);
    if (trimmed.empty()) return {"", false};

    bool include = true;
    std::string path = trimmed;

    // Check for condition [condition] at the end
    if (trimmed.back() == ']') {
        size_t open_bracket = trimmed.find_last_of('[');
        if (open_bracket != std::string::npos) {
            std::string condition = trimmed.substr(open_bracket + 1, trimmed.size() - open_bracket - 2);
            path = trim(trimmed.substr(0, open_bracket));
            include = evaluate_condition(condition);
        }
    }

    // Remove quotes if present
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }

    return {path, include};
}

std::vector<std::string> BuildscriptParser::expand_wildcards(const std::string& pattern,
                                                              const std::string& base_path) {
    std::vector<std::string> result;

    // Check if pattern includes ** for recursive search
    bool recursive = pattern.find("**") != std::string::npos;

    std::string dir;
    std::string file_pattern;

    if (recursive) {
        // Extract the base directory (part before **)
        size_t star_pos = pattern.find("**");
        std::string prefix = pattern.substr(0, star_pos);

        // Remove trailing slash/backslash from prefix
        while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\')) {
            prefix.pop_back();
        }

        // Get the file pattern (part after **)
        std::string suffix = pattern.substr(star_pos + 2);
        // Remove leading slash/backslash from suffix
        while (!suffix.empty() && (suffix.front() == '/' || suffix.front() == '\\')) {
            suffix.erase(0, 1);
        }

        // Directory is the prefix, file pattern is the filename part of suffix
        dir = prefix.empty() ? "." : prefix;
        fs::path suffix_path(suffix);
        file_pattern = suffix_path.filename().string();
    } else {
        fs::path full_pattern = fs::path(base_path) / pattern;
        dir = full_pattern.parent_path().string();
        file_pattern = full_pattern.filename().string();
    }

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
        fs::path search_dir = fs::path(base_path) / dir;
        if (fs::exists(search_dir)) {
            if (recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(search_dir)) {
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
                for (const auto& entry : fs::directory_iterator(search_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (std::regex_match(filename, re)) {
                            // Make path relative to base_path
                            std::string rel_path = fs::relative(entry.path(), base_path).string();
                            result.push_back(rel_path);
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

std::string BuildscriptParser::resolve_path(const std::string& path, const std::string& base_path) {
    // Check if original path has trailing slash/backslash
    bool has_trailing_slash = !path.empty() && (path.back() == '/' || path.back() == '\\');

    // Resolve path relative to base_path to get absolute path
    fs::path abs_path;
    if (fs::path(path).is_absolute()) {
        abs_path = path;
    } else {
        abs_path = fs::path(base_path) / path;
    }

    // Normalize the path
    try {
        abs_path = fs::canonical(abs_path);
    } catch (...) {
        // If canonical fails (file doesn't exist yet), use absolute path
        abs_path = fs::absolute(abs_path).lexically_normal();
    }

    std::string result = abs_path.string();

    // Preserve trailing slash if original had one
    if (has_trailing_slash && !result.empty() && result.back() != '\\' && result.back() != '/') {
        result += '\\';
    }

    return result;
}

SourceFile* BuildscriptParser::find_or_create_source(const std::string& path, ParseState& state) {
    if (!state.current_project) return nullptr;

    // Resolve path relative to base_path to get absolute path
    std::string abs_path_str = resolve_path(path, state.base_path);

    // Look for existing entry using absolute path
    for (auto& src : state.current_project->sources) {
        if (src.path == abs_path_str) {
            return &src;
        }
    }

    // Create new entry with absolute path
    state.current_project->sources.emplace_back();
    auto& src = state.current_project->sources.back();
    src.path = abs_path_str;
    src.type = get_file_type(abs_path_str);
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

    Solution solution = parse_string(buffer.str(), base.empty() ? "." : base.string());

    // Track the initial file as included
    solution.name = solution.name.empty() && !solution.projects.empty()
                    ? solution.projects[0].name
                    : solution.name;

    return solution;
}

Solution BuildscriptParser::parse_string(const std::string& content, const std::string& base_path) {
    Solution solution;
    solution.uuid = generate_uuid();
    // Initialize with defaults - these will be updated if [config:...] sections are discovered
    solution.configurations = {"Debug", "Release"};
    solution.platforms = {"Win32", "x64"};

    ParseState state;
    state.solution = &solution;
    state.base_path = base_path;

    // Pre-process content to handle multiline values (""")
    std::string processed_content = preprocess_multiline(content);

    // Parse the processed content
    std::istringstream stream(processed_content);
    std::string line;

    while (std::getline(stream, line)) {
        state.line_number++;
        parse_line(line, state);
    }

    // Update configurations from discovered [config:...] sections
    // (this is redundant if configs were discovered, since they're updated immediately,
    // but ensures the final state is correct)
    if (!state.discovered_configs.empty() && !state.discovered_platforms.empty()) {
        solution.configurations = std::vector<std::string>(
            state.discovered_configs.begin(),
            state.discovered_configs.end()
        );
        solution.platforms = std::vector<std::string>(
            state.discovered_platforms.begin(),
            state.discovered_platforms.end()
        );
    }
    // If no configs were discovered, defaults (set at initialization) remain

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
            // Note: platform_toolset is left empty if not specified - will be set by
            // generator based on detected Visual Studio installation
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
                cfg.link.optimize_references = true;
            }

            // Don't automatically add library ignore lists - preserve what's in the buildscript
            // Users can manually specify ignore_libs if needed
        }

        // Apply project-level preprocessor definitions to ALL final configurations
        // This ensures defines are present even if configs were discovered after parsing project settings
        if (!project.project_level_preprocessor_definitions.empty()) {
            for (const auto& config_key : solution.get_config_keys()) {
                auto& defines = project.configurations[config_key].cl_compile.preprocessor_definitions;
                // Insert project-level defines at the beginning (they should come before config-specific ones)
                defines.insert(defines.begin(),
                              project.project_level_preprocessor_definitions.begin(),
                              project.project_level_preprocessor_definitions.end());
            }
        }
    }

    return solution;
}

void BuildscriptParser::parse_line(const std::string& line, ParseState& state) {
    std::string trimmed = trim(line);

    // If we're accumulating a uses_pch() call, continue accumulating
    if (state.in_uses_pch) {
        state.uses_pch_accumulator += " " + trimmed;
        // Check if this line closes the function call by counting parentheses in the full accumulator
        int paren_count = 0;
        bool in_string = false;
        for (size_t i = 0; i < state.uses_pch_accumulator.size(); ++i) {
            char c = state.uses_pch_accumulator[i];
            if (c == '"' && (i == 0 || state.uses_pch_accumulator[i-1] != '\\')) {
                in_string = !in_string;
            } else if (!in_string) {
                if (c == '(') paren_count++;
                else if (c == ')') paren_count--;
            }
        }

        if (paren_count == 0) {
            // Function call complete, parse it
            parse_uses_pch(state.uses_pch_accumulator, state);
            state.uses_pch_accumulator.clear();
            state.in_uses_pch = false;
        }
        return;
    }

    // Skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
        return;
    }

    // Check for if statement
    if (trimmed.rfind("if", 0) == 0) {
        size_t start_paren = trimmed.find('(');
        size_t end_paren = trimmed.rfind(')');
        size_t brace_pos = trimmed.rfind('{');
        
        if (start_paren != std::string::npos && end_paren != std::string::npos && 
            brace_pos != std::string::npos && brace_pos > end_paren) {
            
            std::string condition = trimmed.substr(start_paren + 1, end_paren - start_paren - 1);
            bool cond_met = evaluate_condition(condition);
            bool parent_exec = state.is_executing();
            
            state.conditional_stack.push_back({parent_exec && cond_met, cond_met, 0});
            return;
        }
    }

    // Handle skipping (must be done before other checks)
    if (!state.is_executing()) {
        // Track nested braces to handle if blocks or other blocks inside skipped code
        if (trimmed.find('{') != std::string::npos) {
             state.conditional_stack.back().ignored_brace_depth++;
        }
        if (trimmed.find('}') != std::string::npos) {
             if (state.conditional_stack.back().ignored_brace_depth > 0) {
                 state.conditional_stack.back().ignored_brace_depth--;
             } else {
                 state.conditional_stack.pop_back();
             }
        }
        return;
    }

    // Check for closing brace }
    if (trimmed == "}") {
        if (state.in_file_properties) {
             state.in_file_properties = false;
             state.file_properties_files.clear();
             return;
        }
        
        if (!state.conditional_stack.empty()) {
             state.conditional_stack.pop_back();
             return;
        }
    }

    // Check for section headers
    if (trimmed[0] == '[' && trimmed.back() == ']') {
        parse_section(trimmed, state);
        return;
    }

    // Check for file_properties() function call
    if (trimmed.find("file_properties(") == 0) {
        if (!state.current_project) {
            std::cerr << "Warning: file_properties() outside of project context at line " << state.line_number << "\n";
            return;
        }

        // Extract file list between parentheses
        size_t start_paren = trimmed.find('(');
        size_t end_paren = trimmed.rfind(')');
        if (start_paren != std::string::npos && end_paren != std::string::npos && end_paren > start_paren) {
            std::string content = trimmed.substr(start_paren + 1, end_paren - start_paren - 1);
            auto file_paths = split(content, ',');

            state.file_properties_files.clear();
            for (const auto& file_path : file_paths) {
                std::string path = trim(file_path);
                if (!path.empty()) {
                    SourceFile* src = find_or_create_source(path, state);
                    state.file_properties_files.push_back(src);
                }
            }

            // Check if the line ends with {
            size_t brace_pos = trimmed.find('{', end_paren);
            if (brace_pos != std::string::npos) {
                state.in_file_properties = true;
                state.current_file = nullptr;  // Clear current file since we're setting multiple files
            }
        }
        return;
    }

    // Check for set_file_properties() function call
    if (trimmed.find("set_file_properties(") == 0) {
        if (!state.current_project) {
            std::cerr << "Warning: set_file_properties() outside of project context at line " << state.line_number << "\n";
            return;
        }

        // Extract file path (first argument before comma)
        size_t start_paren = trimmed.find('(');
        size_t comma_pos = trimmed.find(',', start_paren);
        if (start_paren != std::string::npos && comma_pos != std::string::npos) {
            std::string file_path = trim(trimmed.substr(start_paren + 1, comma_pos - start_paren - 1));
            if (!file_path.empty()) {
                state.set_file_properties_file = find_or_create_source(file_path, state);
                state.in_set_file_properties = true;
                state.current_file = nullptr;  // Clear current file context
            }
        }
        return;
    }

    // Check for closing paren of set_file_properties() block
    if (state.in_set_file_properties && trimmed == ")") {
        state.in_set_file_properties = false;
        state.set_file_properties_file = nullptr;
        return;
    }

    // Check for target_link_libraries() function call
    if (trimmed.find("target_link_libraries(") == 0) {
        if (!state.current_project) {
            std::cerr << "Warning: target_link_libraries() outside of project context at line " << state.line_number << "\n";
            return;
        }

        // Extract content between parentheses
        size_t start_paren = trimmed.find('(');
        size_t end_paren = trimmed.rfind(')');
        if (start_paren != std::string::npos && end_paren != std::string::npos && end_paren > start_paren) {
            std::string content = trimmed.substr(start_paren + 1, end_paren - start_paren - 1);
            auto deps = split(content, ',');
            for (const auto& dep : deps) {
                std::string dep_name = trim(dep);
                if (!dep_name.empty()) {
                    state.current_project->project_references.push_back(dep_name);
                }
            }
        }
        return;
    }

    // Check for uses_pch() function call
    if (trimmed.find("uses_pch(") == 0) {
        // Check if the function call is complete on this line by counting parentheses
        int paren_count = 0;
        bool in_string = false;
        for (size_t i = 0; i < trimmed.size(); ++i) {
            char c = trimmed[i];
            if (c == '"' && (i == 0 || trimmed[i-1] != '\\')) {
                in_string = !in_string;
            } else if (!in_string) {
                if (c == '(') paren_count++;
                else if (c == ')') paren_count--;
            }
        }

        if (paren_count == 0) {
            // Single-line function call (all parens matched)
            parse_uses_pch(trimmed, state);
        } else {
            // Multi-line function call, start accumulating
            state.in_uses_pch = true;
            state.uses_pch_accumulator = trimmed;
        }
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
        state.current_config.clear();
        return true;
    }

    // [project:name]
    if (section.rfind("project:", 0) == 0) {
        state.solution->projects.emplace_back();
        state.current_project = &state.solution->projects.back();
        state.current_project->name = trim(section.substr(8));
        state.current_project->uuid = generate_uuid();
        state.current_project->root_namespace = state.current_project->name;
        // Store the buildscript directory for path resolution in custom commands
        state.current_project->buildscript_path = state.base_path;
        state.current_file = nullptr;
        state.current_config.clear();
        return true;
    }

    // [file:path] - for per-file settings block
    if (section.rfind("file:", 0) == 0) {
        std::string file_path = trim(section.substr(5));
        state.current_file = find_or_create_source(file_path, state);
        state.current_config.clear();
        return true;
    }
    
    // [config:Debug|Win32] - for config-specific settings
    if (section.rfind("config:", 0) == 0) {
        state.current_config = trim(section.substr(7));  // Extract config name (e.g., "Debug|x64")
        state.current_file = nullptr;  // Leave file context

        // Parse config|platform and track them for solution-level configuration generation
        size_t pipe_pos = state.current_config.find('|');
        if (pipe_pos != std::string::npos) {
            std::string config = state.current_config.substr(0, pipe_pos);
            std::string platform = state.current_config.substr(pipe_pos + 1);
            if (!config.empty() && !platform.empty()) {
                state.discovered_configs.insert(config);
                state.discovered_platforms.insert(platform);

                // Update solution configurations/platforms immediately so project-level settings
                // can be applied to all configs (including newly discovered ones)
                state.solution->configurations = std::vector<std::string>(
                    state.discovered_configs.begin(),
                    state.discovered_configs.end()
                );
                state.solution->platforms = std::vector<std::string>(
                    state.discovered_platforms.begin(),
                    state.discovered_platforms.end()
                );
            }
        } else {
            std::cerr << "Warning: Invalid config format (missing platform): " << state.current_config << "\n";
        }

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
    
    // Check for config-specific setting: setting[Debug|Win32] = value
    size_t bracket_start = key.find('[');
    size_t bracket_end = key.find(']');
    if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
        std::string config_key = key.substr(bracket_start + 1, bracket_end - bracket_start - 1);
        std::string setting = trim(key.substr(0, bracket_start));

        // If we're in a file_properties() block, apply to all files in the group
        if (state.in_file_properties && !state.file_properties_files.empty()) {
            for (SourceFile* file : state.file_properties_files) {
                parse_file_setting(file->path, setting, config_key, value, state);
            }
        }
        // If we're in a set_file_properties() block, apply to that file
        else if (state.in_set_file_properties && state.set_file_properties_file != nullptr) {
            parse_file_setting(state.set_file_properties_file->path, setting, config_key, value, state);
        }
        // If we're in a file context, treat as per-file setting
        else if (state.current_file != nullptr) {
            parse_file_setting(state.current_file->path, setting, config_key, value, state);
        } else {
            parse_config_setting(setting, value, config_key, state);
        }
        return;
    }

    // Regular key=value
    if (state.current_project == nullptr) {
        parse_solution_setting(key, value, state);
    } else if (state.in_file_properties && !state.file_properties_files.empty()) {
        // If we're in a file_properties() block, apply to all files in the group
        for (SourceFile* file : state.file_properties_files) {
            parse_file_setting(file->path, key, ALL_CONFIGS, value, state);
        }
    } else if (state.in_set_file_properties && state.set_file_properties_file != nullptr) {
        // If we're in a set_file_properties() block, apply to that file
        parse_file_setting(state.set_file_properties_file->path, key, ALL_CONFIGS, value, state);
    } else if (state.current_file != nullptr) {
        parse_file_setting(state.current_file->path, key, ALL_CONFIGS, value, state);
    } else if (!state.current_config.empty()) {
        // We're in a [config:...] section
        parse_config_setting(key, value, state.current_config, state);
    } else {
        parse_project_setting(key, value, state);
    }
}

void BuildscriptParser::process_include(const std::string& include_path, ParseState& state) {
    // Resolve include path relative to base_path
    fs::path full_path = fs::path(state.base_path) / include_path;
    std::string canonical_path;

    try {
        canonical_path = fs::canonical(full_path).string();
    } catch (const std::exception&) {
        // If canonical fails, try with absolute path
        canonical_path = fs::absolute(full_path).string();
    }

    // Check for circular includes
    for (const auto& included : state.included_files) {
        if (included == canonical_path) {
            std::cerr << "Warning: Circular include detected: " << include_path << "\n";
            return;
        }
    }

    // Check if file exists
    if (!fs::exists(canonical_path)) {
        std::cerr << "Warning: Include file not found: " << include_path << "\n";
        return;
    }

    // Mark as included
    state.included_files.push_back(canonical_path);

    // Read and parse the included file
    std::ifstream file(canonical_path);
    if (!file.is_open()) {
        std::cerr << "Warning: Cannot open include file: " << include_path << "\n";
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    // Pre-process content to handle multiline values (""")
    std::string processed_content = preprocess_multiline(buffer.str());

    // Parse the included content with the same state
    std::istringstream stream(processed_content);
    std::string line;
    int saved_line_number = state.line_number;
    state.line_number = 0;

    // Get base path for the included file
    fs::path include_base = fs::path(canonical_path).parent_path();
    std::string saved_base_path = state.base_path;
    state.base_path = include_base.string();

    // Save and restore current_project and current_file to prevent included files from affecting parent context
    Project* saved_current_project = state.current_project;
    SourceFile* saved_current_file = state.current_file;

    while (std::getline(stream, line)) {
        state.line_number++;
        parse_line(line, state);
    }

    // Restore original state
    state.line_number = saved_line_number;
    state.base_path = saved_base_path;
    state.current_project = saved_current_project;
    state.current_file = saved_current_file;
}

void BuildscriptParser::parse_solution_setting(const std::string& key, const std::string& value,
                                                ParseState& state) {
    if (key == "name") {
        state.solution->name = value;
    } else if (key == "configurations" || key == "configs") {
        state.solution->configurations = split(value, ',');
    } else if (key == "platforms") {
        state.solution->platforms = split(value, ',');
    } else if (key == "include") {
        process_include(value, state);
    }
}

void BuildscriptParser::parse_project_setting(const std::string& key, const std::string& value,
                                               ParseState& state) {
    if (!state.current_project) return;

    Project& proj = *state.current_project;

    // Include directive within project context
    if (key == "include") {
        process_include(value, state);
        return;
    }

    // Basic project settings
    if (key == "name") {
        proj.name = value;
    } else if (key == "project_name") {
        proj.project_name = value;
    } else if (key == "uuid" || key == "guid") {
        proj.uuid = value;
    } else if (key == "root_namespace") {
        proj.root_namespace = value;
    } else if (key == "ignore_warn_duplicated_filename") {
        proj.ignore_warn_compile_duplicated_filename = (value == "true" || value == "yes" || value == "1");
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
        auto& registry = ToolsetRegistry::instance();
        auto resolved = registry.resolve(value);
        
        if (resolved) {
            std::string toolset_id = *resolved;
            
            // Warn if unknown (but still allow for forward compatibility)
            if (!registry.is_known(toolset_id)) {
                std::cerr << "Warning: Unknown toolset '" << toolset_id << "'\n";
            }
            
            for (const auto& config_key : state.solution->get_config_keys()) {
                proj.configurations[config_key].platform_toolset = toolset_id;
            }
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
        // Resolve relative to buildscript location to get absolute path
        std::string resolved_dir = resolve_path(value, state.base_path);
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].out_dir = resolved_dir;
        }
    } else if (key == "intdir" || key == "intermediate_dir") {
        // Resolve relative to buildscript location to get absolute path
        std::string resolved_dir = resolve_path(value, state.base_path);
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].int_dir = resolved_dir;
        }
    }
    // Source files
    else if (key == "sources" || key == "src" || key == "files") {
        for (const auto& src : split(value, ',')) {
            auto [path, include] = parse_filename_with_condition(src);
            if (!include) continue;

            auto expanded = expand_wildcards(path, state.base_path);
            if (expanded.empty()) {
                // If no expansion, add as-is
                find_or_create_source(path, state);
            } else {
                for (const auto& expanded_path : expanded) {
                    find_or_create_source(expanded_path, state);
                }
            }
        }
    } else if (key == "headers" || key == "includes_files") {
        for (const auto& src : split(value, ',')) {
            auto [path, include] = parse_filename_with_condition(src);
            if (!include) continue;

            auto expanded = expand_wildcards(path, state.base_path);
            for (const auto& expanded_path : expanded) {
                auto* file = find_or_create_source(expanded_path, state);
                if (file) file->type = FileType::ClInclude;
            }
        }
    } else if (key == "resources" || key == "resource_files") {
        for (const auto& src : split(value, ',')) {
            auto [path, include] = parse_filename_with_condition(src);
            if (!include) continue;

            auto expanded = expand_wildcards(path, state.base_path);
            for (const auto& expanded_path : expanded) {
                auto* file = find_or_create_source(expanded_path, state);
                if (file) file->type = FileType::ResourceCompile;
            }
        }
    } else if (key == "libs" || key == "libraries") {
        auto libs = split(value, ',');

        // Split libraries into file paths vs system libraries
        for (const auto& lib : libs) {
            auto [path, include] = parse_filename_with_condition(lib);
            if (!include) continue;

            // If the library has a path separator, it's a file path → use <Library> element
            if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos) {
                LibraryFile lf;
                // Don't normalize library paths - preserve exact case and format
                lf.path = path;
                proj.libraries.push_back(lf);
            } else {
                // System library (e.g., shell32.lib) → use <AdditionalDependencies>
                for (const auto& config_key : state.solution->get_config_keys()) {
                    auto& deps = proj.configurations[config_key].link.additional_dependencies;
                    deps.push_back(path);
                }
            }
        }
    }
    // Compiler settings (apply to all configs)
    else if (key == "includes" || key == "include_dirs" || key == "additional_include_directories") {
        auto dirs = split(value, ',');
        std::vector<std::string> resolved_dirs;
        for (const auto& dir : dirs) {
            resolved_dirs.push_back(resolve_path(dir, state.base_path));
        }
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& includes = proj.configurations[config_key].cl_compile.additional_include_directories;
            includes.insert(includes.end(), resolved_dirs.begin(), resolved_dirs.end());
        }
    } else if (key == "forced_includes" || key == "forced_include_files") {
        auto files = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& forced = proj.configurations[config_key].cl_compile.forced_include_files;
            forced.insert(forced.end(), files.begin(), files.end());
        }
    } else if (key == "defines" || key == "preprocessor" || key == "preprocessor_definitions") {
        auto defs = split(value, ',');
        // Store as project-level defines that will be applied to all final configs
        // Note: Unlike other settings, defines use deferred application (post-parsing) to ensure
        // they appear in ALL configurations, even those discovered later from other buildscripts
        proj.project_level_preprocessor_definitions.insert(
            proj.project_level_preprocessor_definitions.end(),
            defs.begin(), defs.end()
        );
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
    } else if (key == "error_reporting") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.error_reporting = value;
        }
    } else if (key == "assembler_listing" || key == "assembler_listing_location") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.assembler_listing_location = value;
        }
    } else if (key == "object_file_name") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.object_file_name = value;
        }
    } else if (key == "program_database_file" || key == "program_database_file_name") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.program_database_file_name = value;
        }
    } else if (key == "browse_information_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.browse_information_file = value;
        }
    } else if (key == "basic_runtime_checks") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.basic_runtime_checks = value;
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
    } else if (key == "simd" || key == "enhanced_instruction_set") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.enhanced_instruction_set = value;
        }
    } else if (key == "floating_point" || key == "fp_model" || key == "floating_point_model") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.floating_point_model = value;
        }
    } else if (key == "inline_function_expansion" || key == "inline_expansion") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.inline_function_expansion = value;
        }
    } else if (key == "favor_size_or_speed" || key == "favor") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.favor_size_or_speed = value;
        }
    } else if (key == "string_pooling") {
        bool sp = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.string_pooling = sp;
        }
    } else if (key == "minimal_rebuild") {
        bool mr = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.minimal_rebuild = mr;
        }
    } else if (key == "basic_runtime_checks" || key == "runtime_checks") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.basic_runtime_checks = value;
        }
    } else if (key == "buffer_security_check" || key == "buffer_security") {
        bool bsc = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.buffer_security_check = bsc;
        }
    } else if (key == "force_conformance_in_for_loop_scope" || key == "force_conformance") {
        bool fc = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.force_conformance_in_for_loop_scope = fc;
        }
    } else if (key == "assembler_listing_location") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.assembler_listing_location = value;
        }
    } else if (key == "object_file_name") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.object_file_name = value;
        }
    } else if (key == "program_database_file_name" || key == "pdb_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.program_database_file_name = value;
        }
    } else if (key == "generate_xml_documentation_files" || key == "xml_docs") {
        bool xml = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.generate_xml_documentation_files = xml;
        }
    } else if (key == "browse_information") {
        bool bi = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.browse_information = bi;
        }
    } else if (key == "browse_information_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.browse_information_file = value;
        }
    } else if (key == "compile_as") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.compile_as = value;
        }
    } else if (key == "error_reporting") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.error_reporting = value;
        }
    } else if (key == "treat_wchar_t_as_builtin" || key == "treat_wchar_t_as_built_in_type") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.treat_wchar_t_as_built_in_type = val;
        }
    } else if (key == "assembler_output") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.assembler_output = value;
        }
    } else if (key == "expand_attributed_source") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.expand_attributed_source = val;
        }
    } else if (key == "openmp" || key == "openmp_support") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.openmp_support = val;
        }
    } else if (key == "treat_warning_as_error") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].cl_compile.treat_warning_as_error = val;
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
        std::vector<std::string> resolved_dirs;
        for (const auto& dir : dirs) {
            resolved_dirs.push_back(resolve_path(dir, state.base_path));
        }
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& libdirs = proj.configurations[config_key].link.additional_library_directories;
            libdirs.insert(libdirs.end(), resolved_dirs.begin(), resolved_dirs.end());
        }
    } else if (key == "libs") {
        // Project-level libs should become Library elements
        auto libs = split(value, ',');
        for (const auto& lib_path : libs) {
            LibraryFile lf;
            lf.path = trim(lib_path);
            proj.libraries.push_back(lf);
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
    } else if (key == "show_progress" || key == "link_show_progress") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.show_progress = value;
        }
    } else if (key == "output_file" || key == "link_output_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.output_file = value;
        }
    } else if (key == "suppress_startup_banner" || key == "link_suppress_startup_banner") {
        bool ssb = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.suppress_startup_banner = ssb;
        }
    } else if (key == "program_database_file" || key == "link_pdb_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.program_database_file = value;
        }
    } else if (key == "base_address") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.base_address = value;
        }
    } else if (key == "target_machine") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.target_machine = value;
        }
    } else if (key == "link_error_reporting") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.error_reporting = value;
        }
    } else if (key == "image_has_safe_exception_handlers" || key == "safe_seh") {
        bool seh = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.image_has_safe_exception_handlers = seh;
        }
    } else if (key == "ldflags" || key == "linker_flags" || key == "link_additional_options") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& opts = proj.configurations[config_key].link.additional_options;
            if (!opts.empty()) opts += " ";
            opts += value;
        }
    } else if (key == "suppress_startup_banner") {
        bool suppress = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.suppress_startup_banner = suppress;
        }
    } else if (key == "show_progress") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.show_progress = value;
        }
    } else if (key == "entry_point" || key == "entry_point_symbol") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.entry_point_symbol = value;
        }
    } else if (key == "link_version" || key == "version") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.version = value;
        }
    } else if (key == "link_output_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.output_file = value;
        }
    } else if (key == "link_program_database_file" || key == "link_pdb") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.program_database_file = value;
        }
    } else if (key == "generate_map_file") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.generate_map_file = val;
        }
    } else if (key == "map_file_name") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.map_file_name = value;
        }
    } else if (key == "fixed_base_address") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.fixed_base_address = val;
        }
    } else if (key == "large_address_aware") {
        bool val = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].link.large_address_aware = val;
        }
    }
    // Librarian settings (for static libraries)
    else if (key == "lib_output_file") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].lib.output_file = normalize_path(value);
        }
    } else if (key == "lib_suppress_startup_banner") {
        bool ssb = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].lib.suppress_startup_banner = ssb;
        }
    } else if (key == "lib_use_unicode_response_files") {
        bool unicode = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].lib.use_unicode_response_files = unicode;
        }
    } else if (key == "libflags" || key == "lib_options" || key == "lib_additional_options") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& opts = proj.configurations[config_key].lib.additional_options;
            if (!opts.empty()) opts += " ";
            opts += value;
        }
    } else if (key == "lib_additional_dependencies" || key == "lib_deps") {
        auto deps = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& lib_deps = proj.configurations[config_key].lib.additional_dependencies;
            lib_deps.insert(lib_deps.end(), deps.begin(), deps.end());
        }
    }
    // ResourceCompile settings
    else if (key == "rc_culture" || key == "resource_culture") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].resource_compile.culture = value;
        }
    } else if (key == "rc_defines" || key == "rc_preprocessor" || key == "resource_defines") {
        auto defs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& defines = proj.configurations[config_key].resource_compile.preprocessor_definitions;
            defines.insert(defines.end(), defs.begin(), defs.end());
        }
    } else if (key == "rc_includes" || key == "resource_includes") {
        auto dirs = split(value, ',');
        std::vector<std::string> resolved_dirs;
        for (const auto& dir : dirs) {
            resolved_dirs.push_back(resolve_path(dir, state.base_path));
        }
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& includes = proj.configurations[config_key].resource_compile.additional_include_directories;
            includes.insert(includes.end(), resolved_dirs.begin(), resolved_dirs.end());
        }
    }
    // Configuration-level properties
    else if (key == "executable_path") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].executable_path = value;
        }
    } else if (key == "generate_manifest") {
        bool gm = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].generate_manifest = gm;
        }
    }
    // Resource compile settings
    else if (key == "resource_defines" || key == "resource_preprocessor_definitions") {
        auto defs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& res_defines = proj.configurations[config_key].resource_compile.preprocessor_definitions;
            res_defines.insert(res_defines.end(), defs.begin(), defs.end());
        }
    } else if (key == "resource_culture") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].resource_compile.culture = value;
        }
    } else if (key == "resource_includes" || key == "resource_additional_include_directories") {
        auto dirs = split(value, ',');
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& res_includes = proj.configurations[config_key].resource_compile.additional_include_directories;
            res_includes.insert(res_includes.end(), dirs.begin(), dirs.end());
        }
    }
    // Project-level properties
    else if (key == "ignore_warn_duplicated_filename") {
        proj.ignore_warn_compile_duplicated_filename = (value == "true" || value == "yes" || value == "1");
    }
    // Build events
    else if (key == "prebuild" || key == "pre_build_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_build_event.command = unescape_value(value);
        }
    } else if (key == "prelink" || key == "pre_link_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_link_event.command = unescape_value(value);
        }
    } else if (key == "postbuild" || key == "post_build_event") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].post_build_event.command = unescape_value(value);
        }
    } else if (key == "prebuild_message" || key == "pre_build_event_message") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_build_event.message = unescape_value(value);
        }
    } else if (key == "prelink_message" || key == "pre_link_event_message") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_link_event.message = unescape_value(value);
        }
    } else if (key == "postbuild_message" || key == "post_build_event_message") {
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].post_build_event.message = unescape_value(value);
        }
    } else if (key == "prebuild_use_in_build" || key == "pre_build_event_use_in_build") {
        bool use = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_build_event.use_in_build = use;
        }
    } else if (key == "prelink_use_in_build" || key == "pre_link_event_use_in_build") {
        bool use = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].pre_link_event.use_in_build = use;
        }
    } else if (key == "postbuild_use_in_build" || key == "post_build_event_use_in_build") {
        bool use = (value == "true" || value == "yes" || value == "1");
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj.configurations[config_key].post_build_event.use_in_build = use;
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
        std::vector<std::string> resolved_dirs;
        for (const auto& dir : dirs) {
            resolved_dirs.push_back(resolve_path(dir, state.base_path));
        }
        auto& target = file->settings.additional_includes[config_key];
        target.insert(target.end(), resolved_dirs.begin(), resolved_dirs.end());
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
        // Any file with a custom command should be treated as CustomBuild
        if (file->type != FileType::ResourceCompile) {
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
        auto& registry = ToolsetRegistry::instance();
        auto resolved = registry.resolve(value);
        
        if (resolved) {
            cfg.platform_toolset = *resolved;
            
            if (!registry.is_known(*resolved)) {
                std::cerr << "Warning: Unknown toolset '" << *resolved << "'\n";
            }
        }
    } else if (key == "windows_sdk" || key == "windows_sdk_version" || key == "windows_target_platform_version") {
        cfg.windows_target_platform_version = value;
    } else if (key == "outdir" || key == "output_dir") {
        // Resolve relative to buildscript location to get absolute path
        cfg.out_dir = resolve_path(value, state.base_path);
    } else if (key == "intdir" || key == "intermediate_dir") {
        // Resolve relative to buildscript location to get absolute path
        cfg.int_dir = resolve_path(value, state.base_path);
    } else if (key == "includes" || key == "additional_include_directories") {
        auto dirs = split(value, ',');
        cfg.cl_compile.additional_include_directories.insert(
            cfg.cl_compile.additional_include_directories.end(), dirs.begin(), dirs.end());
    } else if (key == "defines" || key == "preprocessor_definitions") {
        auto defs = split(value, ',');
        cfg.cl_compile.preprocessor_definitions.insert(
            cfg.cl_compile.preprocessor_definitions.end(), defs.begin(), defs.end());
    } else if (key == "libs" || key == "additional_dependencies") {
        // Config-level libs should go into AdditionalDependencies
        auto libs = split(value, ',');
        cfg.link.additional_dependencies.insert(
            cfg.link.additional_dependencies.end(), libs.begin(), libs.end());
    } else if (key == "libdirs" || key == "lib_dirs" || key == "additional_library_directories") {
        auto dirs = split(value, ',');
        cfg.link.additional_library_directories.insert(
            cfg.link.additional_library_directories.end(), dirs.begin(), dirs.end());
    } else if (key == "ignore_libs" || key == "ignore_specific_default_libraries") {
        auto libs = split(value, ',');
        cfg.link.ignore_specific_default_libraries.insert(
            cfg.link.ignore_specific_default_libraries.end(), libs.begin(), libs.end());
    } else if (key == "link_incremental") {
        cfg.link_incremental = (value == "true" || value == "yes" || value == "1");
    } else if (key == "whole_program_optimization" || key == "wpo" || key == "ltcg") {
        cfg.whole_program_optimization = (value == "true" || value == "yes" || value == "1");
    } else if (key == "generate_debug_info") {
        cfg.link.generate_debug_info = (value == "true" || value == "yes" || value == "1");
    }
    // New compiler settings
    else if (key == "inline_function_expansion" || key == "inline_expansion") {
        cfg.cl_compile.inline_function_expansion = value;
    } else if (key == "favor_size_or_speed" || key == "favor") {
        cfg.cl_compile.favor_size_or_speed = value;
    } else if (key == "string_pooling") {
        cfg.cl_compile.string_pooling = (value == "true" || value == "yes" || value == "1");
    } else if (key == "minimal_rebuild") {
        cfg.cl_compile.minimal_rebuild = (value == "true" || value == "yes" || value == "1");
    } else if (key == "basic_runtime_checks" || key == "runtime_checks") {
        cfg.cl_compile.basic_runtime_checks = value;
    } else if (key == "buffer_security_check" || key == "buffer_security") {
        cfg.cl_compile.buffer_security_check = (value == "true" || value == "yes" || value == "1");
    } else if (key == "force_conformance_in_for_loop_scope" || key == "force_conformance") {
        cfg.cl_compile.force_conformance_in_for_loop_scope = (value == "true" || value == "yes" || value == "1");
    } else if (key == "function_level_linking") {
        cfg.cl_compile.function_level_linking = (value == "true" || value == "yes" || value == "1");
    } else if (key == "intrinsic_functions") {
        cfg.cl_compile.intrinsic_functions = (value == "true" || value == "yes" || value == "1");
    } else if (key == "assembler_listing_location") {
        cfg.cl_compile.assembler_listing_location = value;
    } else if (key == "object_file_name") {
        cfg.cl_compile.object_file_name = value;
    } else if (key == "program_database_file_name" || key == "pdb_file") {
        cfg.cl_compile.program_database_file_name = value;
    } else if (key == "generate_xml_documentation_files" || key == "xml_docs") {
        cfg.cl_compile.generate_xml_documentation_files = (value == "true" || value == "yes" || value == "1");
    } else if (key == "browse_information") {
        cfg.cl_compile.browse_information = (value == "true" || value == "yes" || value == "1");
    } else if (key == "browse_information_file") {
        cfg.cl_compile.browse_information_file = value;
    } else if (key == "warning_level") {
        cfg.cl_compile.warning_level = value;
    } else if (key == "compile_as") {
        cfg.cl_compile.compile_as = value;
    } else if (key == "error_reporting" || key == "compiler_error_reporting") {
        cfg.cl_compile.error_reporting = value;
    } else if (key == "treat_wchar_t_as_builtin" || key == "treat_wchar_t_as_built_in_type") {
        cfg.cl_compile.treat_wchar_t_as_built_in_type = (value == "true" || value == "yes" || value == "1");
    } else if (key == "assembler_output") {
        cfg.cl_compile.assembler_output = value;
    } else if (key == "expand_attributed_source") {
        cfg.cl_compile.expand_attributed_source = (value == "true" || value == "yes" || value == "1");
    } else if (key == "openmp" || key == "openmp_support") {
        cfg.cl_compile.openmp_support = (value == "true" || value == "yes" || value == "1");
    } else if (key == "treat_warning_as_error") {
        cfg.cl_compile.treat_warning_as_error = (value == "true" || value == "yes" || value == "1");
    } else if (key == "exception_handling" || key == "exceptions") {
        std::string eh_value = value;
        if (value == "false" || value == "no" || value == "0") eh_value = "false";
        else if (value == "true" || value == "yes" || value == "sync") eh_value = "Sync";
        else if (value == "async") eh_value = "Async";
        cfg.cl_compile.exception_handling = eh_value;
    } else if (key == "runtime_type_info" || key == "rtti") {
        cfg.cl_compile.runtime_type_info = (value == "true" || value == "yes" || value == "1");
    } else if (key == "multi_processor_compilation" || key == "multiprocessor" || key == "mp") {
        cfg.cl_compile.multi_processor_compilation = (value == "true" || value == "yes" || value == "1");
    } else if (key == "enhanced_instruction_set" || key == "simd") {
        cfg.cl_compile.enhanced_instruction_set = value;
    } else if (key == "floating_point_model" || key == "floating_point" || key == "fp_model") {
        cfg.cl_compile.floating_point_model = value;
    } else if (key == "language_standard" || key == "std" || key == "cpp_standard") {
        cfg.cl_compile.language_standard = (value.find("stdcpp") == 0) ? value : ("stdcpp" + value);
    } else if (key == "cflags" || key == "compiler_flags" || key == "additional_options") {
        if (!cfg.cl_compile.additional_options.empty()) cfg.cl_compile.additional_options += " ";
        cfg.cl_compile.additional_options += value;
    } else if (key == "ldflags" || key == "linker_flags" || key == "link_additional_options") {
        if (!cfg.link.additional_options.empty()) cfg.link.additional_options += " ";
        cfg.link.additional_options += value;
    }
    // New linker settings
    else if (key == "show_progress" || key == "link_show_progress") {
        cfg.link.show_progress = value;
    } else if (key == "output_file" || key == "link_output_file") {
        cfg.link.output_file = normalize_path(value);
    } else if (key == "suppress_startup_banner" || key == "link_suppress_startup_banner") {
        cfg.link.suppress_startup_banner = (value == "true" || value == "yes" || value == "1");
    } else if (key == "program_database_file" || key == "link_pdb_file") {
        cfg.link.program_database_file = value;
    } else if (key == "subsystem" || key == "sub_system") {
        cfg.link.sub_system = value;
    } else if (key == "optimize_references") {
        cfg.link.optimize_references = (value == "true" || value == "yes" || value == "1");
    } else if (key == "enable_comdat_folding") {
        cfg.link.enable_comdat_folding = (value == "true" || value == "yes" || value == "1");
    } else if (key == "base_address") {
        cfg.link.base_address = value;
    } else if (key == "target_machine") {
        cfg.link.target_machine = value;
    } else if (key == "link_error_reporting") {
        cfg.link.error_reporting = value;
    } else if (key == "image_has_safe_exception_handlers" || key == "safe_seh") {
        cfg.link.image_has_safe_exception_handlers = (value == "true" || value == "yes" || value == "1");
    } else if (key == "link_output_file" || key == "output_file") {
        cfg.link.output_file = value;
    } else if (key == "link_program_database_file" || key == "link_pdb" || key == "program_database_file") {
        cfg.link.program_database_file = value;
    } else if (key == "generate_map_file") {
        cfg.link.generate_map_file = (value == "true" || value == "yes" || value == "1");
    } else if (key == "map_file_name") {
        cfg.link.map_file_name = value;
    }
    // Librarian settings (for static libraries)
    else if (key == "lib_output_file") {
        cfg.lib.output_file = normalize_path(value);
    } else if (key == "lib_suppress_startup_banner") {
        cfg.lib.suppress_startup_banner = (value == "true" || value == "yes" || value == "1");
    } else if (key == "lib_use_unicode_response_files") {
        cfg.lib.use_unicode_response_files = (value == "true" || value == "yes" || value == "1");
    } else if (key == "libflags" || key == "lib_options" || key == "lib_additional_options") {
        if (!cfg.lib.additional_options.empty()) cfg.lib.additional_options += " ";
        cfg.lib.additional_options += value;
    }
    // Configuration properties
    else if (key == "executable_path") {
        cfg.executable_path = value;
    } else if (key == "generate_manifest") {
        cfg.generate_manifest = (value == "true" || value == "yes" || value == "1");
    } else if (key == "ignore_import_library") {
        cfg.ignore_import_library = (value == "true" || value == "yes" || value == "1");
    } else if (key == "import_library") {
        cfg.import_library = value;
    } else if (key == "target_name") {
        cfg.target_name = value;
    } else if (key == "target_ext" || key == "target_extension") {
        cfg.target_ext = value;
    }
    // Resource compile settings
    else if (key == "resource_defines" || key == "resource_preprocessor_definitions" || key == "rc_defines" || key == "rc_preprocessor") {
        auto defs = split(value, ',');
        cfg.resource_compile.preprocessor_definitions.insert(
            cfg.resource_compile.preprocessor_definitions.end(), defs.begin(), defs.end());
    } else if (key == "resource_culture" || key == "rc_culture") {
        cfg.resource_compile.culture = value;
    } else if (key == "resource_includes" || key == "resource_additional_include_directories" || key == "rc_includes") {
        auto dirs = split(value, ',');
        cfg.resource_compile.additional_include_directories.insert(
            cfg.resource_compile.additional_include_directories.end(), dirs.begin(), dirs.end());
    }
    // Xdcmake/Bscmake settings
    else if (key == "xdcmake_suppress_startup_banner") {
        cfg.xdcmake.suppress_startup_banner = (value == "true" || value == "yes" || value == "1");
    } else if (key == "bscmake_suppress_startup_banner") {
        cfg.bscmake.suppress_startup_banner = (value == "true" || value == "yes" || value == "1");
    } else if (key == "bscmake_output_file") {
        cfg.bscmake.output_file = value;
    }
    // Manifest settings
    else if (key == "manifest_suppress_startup_banner") {
        cfg.manifest.suppress_startup_banner = (value == "true" || value == "yes" || value == "1");
    } else if (key == "manifest_additional_files") {
        cfg.manifest.additional_manifest_files = value;
    }
    // Build events
    else if (key == "prebuild" || key == "pre_build_event") {
        cfg.pre_build_event.command = unescape_value(value);
    } else if (key == "prelink" || key == "pre_link_event") {
        cfg.pre_link_event.command = unescape_value(value);
    } else if (key == "postbuild" || key == "post_build_event") {
        cfg.post_build_event.command = unescape_value(value);
    } else if (key == "prebuild_message" || key == "pre_build_event_message") {
        cfg.pre_build_event.message = unescape_value(value);
    } else if (key == "prelink_message" || key == "pre_link_event_message") {
        cfg.pre_link_event.message = unescape_value(value);
    } else if (key == "postbuild_message" || key == "post_build_event_message") {
        cfg.post_build_event.message = unescape_value(value);
    } else if (key == "prebuild_use_in_build" || key == "pre_build_event_use_in_build") {
        cfg.pre_build_event.use_in_build = (value == "true" || value == "yes" || value == "1");
    } else if (key == "prelink_use_in_build" || key == "pre_link_event_use_in_build") {
        cfg.pre_link_event.use_in_build = (value == "true" || value == "yes" || value == "1");
    } else if (key == "postbuild_use_in_build" || key == "post_build_event_use_in_build") {
        cfg.post_build_event.use_in_build = (value == "true" || value == "yes" || value == "1");
    }
    // PCH settings
    else if (key == "pch" || key == "precompiled_header") {
        cfg.cl_compile.pch.mode = value;
    } else if (key == "pch_header" || key == "precompiled_header_file") {
        cfg.cl_compile.pch.header = value;
    } else if (key == "pch_output" || key == "precompiled_header_output_file") {
        cfg.cl_compile.pch.output = value;
    } else if (key == "excluded_library") {
        // Handle excluded_library[Config] = path
        // This creates a Library element that is NOT excluded from this config
        auto& proj = *state.current_project;
        std::string lib_path = trim(value);

        // Find or create this library entry
        LibraryFile* found_lib = nullptr;
        for (auto& lib : proj.libraries) {
            if (lib.path == lib_path) {
                found_lib = &lib;
                break;
            }
        }

        if (!found_lib) {
            // Create new library entry, excluded from all configs except this one
            LibraryFile lf;
            lf.path = lib_path;
            for (const auto& [other_config_key, other_cfg] : proj.configurations) {
                if (other_config_key != config_key) {
                    lf.excluded[other_config_key] = true;
                }
            }
            proj.libraries.push_back(lf);
        } else {
            // Library already exists - just ensure this config is NOT excluded
            // and all others ARE excluded
            for (const auto& [other_config_key, other_cfg] : proj.configurations) {
                if (other_config_key != config_key) {
                    found_lib->excluded[other_config_key] = true;
                }
            }
        }
    }
    // Also support the same keys as project settings
    else {
        // Forward to project setting parser but it will only affect this config
        // This is a simplified approach - in production you'd want more granular control
    }
}

void BuildscriptParser::parse_uses_pch(const std::string& line, ParseState& state) {
    if (!state.current_project) {
        std::cerr << "Warning: uses_pch() outside of project context at line " << state.line_number << "\n";
        return;
    }

    // Extract content between uses_pch( and )
    size_t start_paren = line.find('(');
    size_t end_paren = line.rfind(')');
    if (start_paren == std::string::npos || end_paren == std::string::npos) {
        std::cerr << "Warning: Malformed uses_pch() at line " << state.line_number << "\n";
        return;
    }

    std::string content = line.substr(start_paren + 1, end_paren - start_paren - 1);

    // Parse parameters: mode, header, [optional output], file_list
    // Expected format: "Mode", "header.h", ["file1.cpp", "file2.cpp"]
    // Or: "Mode", "header.h", "output.pch", ["file1.cpp", "file2.cpp"]

    std::vector<std::string> params;
    bool in_string = false;
    bool in_array = false;
    std::string current_param;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '"' && (i == 0 || content[i-1] != '\\')) {
            in_string = !in_string;
            if (!in_string && !current_param.empty()) {
                // End of a string parameter
                params.push_back(trim(current_param));
                current_param.clear();
            }
        } else if (c == '[' && !in_string) {
            in_array = true;
            current_param += c;
        } else if (c == ']' && !in_string) {
            in_array = false;
            current_param += c;
            // End of array parameter
            params.push_back(trim(current_param));
            current_param.clear();
        } else if (c == ',' && !in_string && !in_array) {
            // Parameter separator (skip)
            continue;
        } else if (in_string || in_array) {
            current_param += c;
        }
    }

    // We should have 3 or 4 parameters: mode, header, [output], file_list
    if (params.size() < 3) {
        std::cerr << "Warning: uses_pch() requires at least 3 parameters at line " << state.line_number << "\n";
        return;
    }

    std::string pch_mode = params[0];
    std::string pch_header = params[1];
    std::string pch_output;
    std::string file_list_str;

    if (params.size() == 3) {
        // No output specified
        file_list_str = params[2];
    } else {
        // Output specified
        pch_output = params[2];
        file_list_str = params[3];
    }

    // Parse file list from array format: ["file1.cpp", "file2.cpp"]
    std::vector<std::string> files;
    if (file_list_str.front() == '[' && file_list_str.back() == ']') {
        std::string list_content = file_list_str.substr(1, file_list_str.size() - 2);

        // Split by commas, handling quoted strings
        in_string = false;
        std::string current_file;
        for (size_t i = 0; i < list_content.size(); ++i) {
            char c = list_content[i];

            if (c == '"' && (i == 0 || list_content[i-1] != '\\')) {
                in_string = !in_string;
                if (!in_string && !current_file.empty()) {
                    files.push_back(trim(current_file));
                    current_file.clear();
                }
            } else if (c == ',' && !in_string) {
                // Skip comma separators
                continue;
            } else if (in_string) {
                current_file += c;
            }
        }
    }

    // Apply PCH settings to all files in the list
    for (const auto& file_path : files) {
        SourceFile* file = find_or_create_source(file_path, state);
        if (file) {
            // Apply to all configurations using the [*] wildcard
            file->settings.pch[ALL_CONFIGS].mode = pch_mode;
            file->settings.pch[ALL_CONFIGS].header = pch_header;
            if (!pch_output.empty()) {
                file->settings.pch[ALL_CONFIGS].output = pch_output;
            }
        }
    }
}

bool BuildscriptParser::evaluate_condition(const std::string& condition) {
    std::string cond = trim(condition);
    
    bool is_windows = false;
    bool is_linux = false;
    bool is_osx = false;

#if defined(_WIN32)
    is_windows = true;
#elif defined(__linux__)
    is_linux = true;
#elif defined(__APPLE__)
    is_osx = true;
#endif

    if (cond == "windows" || cond == "win32") return is_windows;
    if (cond == "linux") return is_linux;
    if (cond == "osx" || cond == "macos" || cond == "darwin") return is_osx;
    
    // Check for negation
    if (cond.size() > 1 && cond[0] == '!') {
         std::string sub = cond.substr(1);
         if (sub == "windows" || sub == "win32") return !is_windows;
         if (sub == "linux") return !is_linux;
         if (sub == "osx" || sub == "macos" || sub == "darwin") return !is_osx;
    }

    std::cerr << "Warning: Unknown condition '" << condition << "'\n";
    return false;
}

} // namespace vcxproj
