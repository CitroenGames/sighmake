#pragma once
#ifndef BUILDSCRIPT_PARSER_HPP
#define BUILDSCRIPT_PARSER_HPP

#include "project_types.hpp"
#include <string>
#include <iostream>

namespace vcxproj {

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
        std::string base_path;
        int line_number = 0;
    };
    
    // Parse a single line
    void parse_line(const std::string& line, ParseState& state);
    
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
    
    // Helper to split string by delimiter
    static std::vector<std::string> split(const std::string& str, char delim);
    
    // Helper to trim whitespace
    static std::string trim(const std::string& str);
    
    // Helper to expand wildcards in source paths
    std::vector<std::string> expand_wildcards(const std::string& pattern, const std::string& base_path);
    
    // Helper to find or create a source file entry
    SourceFile* find_or_create_source(const std::string& path, ParseState& state);
};

} // namespace vcxproj

#endif // BUILDSCRIPT_PARSER_HPP
