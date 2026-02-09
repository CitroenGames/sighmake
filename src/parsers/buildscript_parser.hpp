#pragma once

#include "common/project_types.hpp"

namespace vcxproj {

// Result from package finding operations
struct PackageFindResult {
    bool found = false;
    std::string include_dirs;      // Semicolon-separated list
    std::string libraries;         // Semicolon-separated list
    std::string library_dirs;      // Semicolon-separated list (default/x86)
    std::string library_dirs_x64;  // x64 library path (optional, for DX9/DX10)
    std::string version;           // Package version if detectable
    std::string error_message;     // Error message if not found
};

// Parser for buildscript files
class BuildscriptParser {
public:
    BuildscriptParser() = default;
    
    // Parse a buildscript file and return a Solution
    Solution parse(const std::string& filepath);
    
    // Parse from string content
    Solution parse_string(const std::string& content, const std::string& base_path = ".");
    
private:
    // Current parsing state
    struct ParseState {
        Solution* solution = nullptr;
        Project* current_project = nullptr;
        SourceFile* current_file = nullptr;
        std::string current_config;  // Track current [config:...] section
        std::string base_path;
        int line_number = 0;
        std::vector<std::string> included_files;  // Track included files to prevent circular includes
        std::string uses_pch_accumulator;  // Accumulate multi-line uses_pch() calls
        bool in_uses_pch = false;  // Track if we're inside a uses_pch() call
        std::string target_link_libraries_accumulator;  // Accumulate multi-line target_link_libraries() calls
        bool in_target_link_libraries = false;  // Track if we're inside a target_link_libraries() call
        std::vector<SourceFile*> file_properties_files;  // Files being set in file_properties() block
        bool in_file_properties = false;  // Track if we're inside a file_properties() block
        SourceFile* set_file_properties_file = nullptr;  // File being set in set_file_properties() block
        bool in_set_file_properties = false;  // Track if we're inside a set_file_properties() block
        std::set<std::string> discovered_configs;  // Track configs discovered from [config:...] sections
        std::set<std::string> discovered_platforms;  // Track platforms discovered from [config:...] sections

        // Template inheritance tracking
        std::map<std::string, std::string> config_templates;  // Maps "Config" or "Config|Platform" -> "TemplateName"
        std::set<std::string> pending_template_applications;  // Configs needing template resolution

        // Auto-population tracking
        bool user_defined_config_sections = false;  // Track if user defined any [config:...] sections

        // Variables storage for find_package() results
        std::map<std::string, std::string> variables;
        std::set<std::string> found_packages;

        // Pending if condition (when { is on next line)
        bool pending_if_condition = false;  // True if we saw if() without { on same line
        bool pending_if_result = false;     // Result of evaluate_condition() for pending if

        // Folder block tracking
        std::string current_folder;
        bool in_folder_block = false;
        bool pending_folder_brace = false;
        std::string pending_folder_name;

        struct ScopeState {
            bool executing;
            bool condition_met;
            int ignored_brace_depth = 0;
        };
        std::vector<ScopeState> conditional_stack;

        bool is_executing() const {
            for (const auto& scope : conditional_stack) {
                if (!scope.executing) return false;
            }
            return true;
        }
    };
    
    // Parse a single line
    void parse_line(const std::string& line, ParseState& state);
    
    // Evaluate if condition
    bool evaluate_condition(const std::string& condition);

    // Parse section header [section] or [project:name]
    bool parse_section(const std::string& line, ParseState& state);
    
    // Parse key=value pair
    void parse_key_value(const std::string& key, const std::string& value, ParseState& state);
    
    // Parse solution-level settings
    void parse_solution_setting(const std::string& key, const std::string& value, ParseState& state);
    
    // Parse project-level settings
    void parse_project_setting(const std::string& key, const std::string& value, ParseState& state);
    
    // Parse file-level settings (file.cpp:setting = value)
    void parse_file_setting(const std::string& file_path, const std::string& setting, 
                           const std::string& config_key, const std::string& value, 
                           ParseState& state);
    
    // Parse configuration-specific settings
    void parse_config_setting(const std::string& key, const std::string& value,
                              const std::string& config_key, ParseState& state);

    // Apply template configuration settings to derived configuration
    void apply_template(Project& project, const std::string& derived_key,
                       const std::string& template_key, ParseState& state);

    // Propagate public_includes, public_libs, and public_defines from dependencies
    void propagate_target_link_libraries(Solution& solution);

    // Parse uses_pch() function call
    void parse_uses_pch(const std::string& line, ParseState& state);

    // Parse find_package() function call
    void parse_find_package(const std::string& line, ParseState& state);

    // Resolve ${VARIABLE} references in a string
    std::string resolve_variables(const std::string& str, const ParseState& state);

    // Package finders
    PackageFindResult find_vulkan();
    PackageFindResult find_opengl();
    PackageFindResult find_sdl2();
    PackageFindResult find_sdl3();
    PackageFindResult find_directx11();
    PackageFindResult find_directx12();
    PackageFindResult find_directx9();
    PackageFindResult find_directx10();

#ifdef __linux__
    // Linux-specific pkg-config helper
    PackageFindResult try_pkg_config(const std::string& package_name);
#endif

    // Helper to split string by delimiter
    static std::vector<std::string> split(const std::string& str, char delim);
    
    // Helper to trim whitespace
    static std::string trim(const std::string& str);
    
    // Helper to expand wildcards in source paths
    std::vector<std::string> expand_wildcards(const std::string& pattern, const std::string& base_path);
    
    // Helper to find or create a source file entry
    SourceFile* find_or_create_source(const std::string& path, ParseState& state);

    // Helper to process include directive
    void process_include(const std::string& include_path, ParseState& state);

    // Helper to parse filename with optional condition: "file.cpp" [!linux]
    // Returns pair of {clean_path, should_include}
    std::pair<std::string, bool> parse_filename_with_condition(const std::string& entry);

    // Extended helper that also returns the condition string (for override tracking)
    // Returns tuple of {clean_path, condition_string, should_include}
    std::tuple<std::string, std::string, bool> parse_filename_with_condition_extended(const std::string& entry);

    // Helper to resolve a path relative to base_path and return absolute path
    static std::string resolve_path(const std::string& path, const std::string& base_path);
};

} // namespace vcxproj