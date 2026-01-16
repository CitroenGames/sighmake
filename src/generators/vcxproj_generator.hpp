#pragma once

#include "common/project_types.hpp"
#include "common/generator.hpp"
#include <string>
#include <filesystem>

namespace vcxproj {

// Generator for Visual Studio .vcxproj and .sln files
class VcxprojGenerator : public Generator {
public:
    VcxprojGenerator() = default;

    // Generate all project files (implements Generator interface)
    bool generate(Solution& solution, const std::string& output_dir) override;

    // Get generator name
    std::string name() const override { return "vcxproj"; }

    // Get generator description
    std::string description() const override {
        return "Visual Studio project and solution generator";
    }

    // Generate a .vcxproj file from a Project
    bool generate_vcxproj(const Project& project, const Solution& solution,
                          const std::string& output_path);

    // Generate a .sln file from a Solution
    bool generate_sln(const Solution& solution, const std::string& output_path);

    // Generate a .slnx file from a Solution (XML format for VS 2026)
    bool generate_slnx(const Solution& solution, const std::string& output_path);

private:
    // Helper functions for generating XML elements
    std::string escape_xml(const std::string& str);
    std::string join_vector(const std::vector<std::string>& vec, const std::string& separator);
    std::string map_c_standard(const std::string& std);  // Map C standard to MSVC format
    std::string get_file_type_name(FileType type);

    // Determine if solution should use .slnx format based on detected VS installation
    bool should_use_slnx_format() const;

    // Determine ToolsVersion based on toolset
    std::string get_tools_version(const std::string& toolset) const;
};

} // namespace vcxproj