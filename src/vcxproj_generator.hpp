#pragma once
#ifndef VCXPROJ_GENERATOR_HPP
#define VCXPROJ_GENERATOR_HPP

#include "project_types.hpp"
#include <string>

namespace vcxproj {

// Generator for Visual Studio .vcxproj and .sln files
class VcxprojGenerator {
public:
    VcxprojGenerator() = default;

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

#endif // VCXPROJ_GENERATOR_HPP
