#pragma once

#include "common/project_types.hpp"
#include <string>
#include <vector>

namespace vcxproj {

// Reader for parsing existing .vcxproj files
class VcxprojReader {
public:
    VcxprojReader() = default;

    // Parse a .vcxproj file and return a Project
    Project read_vcxproj(const std::string& filepath);

private:
    // Helper to extract condition key from condition string
    std::string parse_condition(const std::string& condition);

    // Helper to parse configuration from label
    std::pair<std::string, std::string> parse_config_platform(const std::string& label);
};

// Reader for parsing .sln files
class SlnReader {
public:
    SlnReader() = default;

    // Parse a .sln file and return a Solution with project references
    Solution read_sln(const std::string& filepath);

private:
    struct SlnProject {
        std::string name;
        std::string path;
        std::string uuid;
    };

    std::vector<SlnProject> parse_projects(const std::string& content);
    std::map<std::string, std::vector<std::string>> parse_project_dependencies(const std::string& content);
};

// Converter to generate .buildscript from Solution/Project
class BuildscriptWriter {
public:
    BuildscriptWriter() = default;

    // Write a project to a .buildscript file
    bool write_buildscript(const Project& project, const std::string& filepath,
                          const std::vector<std::string>& configurations = {"Debug", "Release"},
                          const std::vector<std::string>& platforms = {"Win32", "x64"});

    // Write a full solution to multiple .buildscript files (one per project)
    bool write_solution_buildscripts(const Solution& solution, const std::string& base_dir);

private:
    // Helper to format a value for output
    std::string format_value(const std::string& value);

    // Helper to write compiler settings
    void write_compiler_settings(std::ostream& out, const ClCompileSettings& settings, int indent = 0);

    // Helper to write linker settings
    void write_linker_settings(std::ostream& out, const LinkSettings& settings, int indent = 0);

    // Helper to write per-config settings
    void write_config_specific_settings(std::ostream& out, const std::map<std::string, Configuration>& configs);

    // Helper to join vector with commas
    std::string join_vector(const std::vector<std::string>& vec, const std::string& sep = ", ");

    // Helper to write project content to stream (extracted for reuse in merged buildscripts)
    void write_project_content(std::ostream& out, const Project& project,
                              const std::string& filepath,
                              const std::vector<std::string>& configurations,
                              const std::vector<std::string>& platforms);
};

} // namespace vcxproj