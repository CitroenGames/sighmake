#pragma once

#include "project_types.hpp"
#include "generator.hpp"
#include <string>
#include <filesystem>

namespace vcxproj {

// Generator for Visual Studio .vcxproj and .sln files
class VcxprojGenerator : public Generator {
public:
    VcxprojGenerator() = default;

    // Generate all project files (implements Generator interface)
    bool generate(const Solution& solution, const std::string& output_dir) override;

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

private:
    // Helper functions for generating XML elements
    std::string escape_xml(const std::string& str);
    std::string join_vector(const std::vector<std::string>& vec, const std::string& separator);
    std::string get_file_type_name(FileType type);
};

} // namespace vcxproj