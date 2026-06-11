#pragma once

#include "common/project_types.hpp"
#include "common/generator.hpp"

namespace vcxproj {

// Generator for CMakeLists.txt files
class CMakeGenerator : public Generator {
public:
    CMakeGenerator() = default;

    // Generate CMakeLists.txt files (implements Generator interface)
    bool generate(Solution& solution, const std::string& output_dir) override;

    // Get generator name
    std::string name() const override { return "cmake"; }

    // Get generator description
    std::string description() const override {
        return "CMake project generator (cross-platform)";
    }

private:
    // Generate root CMakeLists.txt
    bool generate_root_cmakelists(const Solution& solution, const std::string& output_dir);

    // Generate per-project CMakeLists.txt
    bool generate_project_cmakelists(const Project& project, const Solution& solution,
                                     const std::string& project_dir,
                                     const std::string& output_dir);

    // Write sections of per-project CMakeLists.txt
    void write_target_definition(std::ostream& out, const Project& project, const Solution& solution);
    void write_sources(std::ostream& out, const Project& project, const std::string& project_dir);
    void write_include_directories(std::ostream& out, const Project& project, const Solution& solution,
                                   const std::string& project_dir);
    void write_compile_definitions(std::ostream& out, const Project& project, const Solution& solution);
    void write_compile_options(std::ostream& out, const Project& project, const Solution& solution);
    void write_link_libraries(std::ostream& out, const Project& project, const Solution& solution);
    void write_link_directories(std::ostream& out, const Project& project, const Solution& solution,
                                const std::string& project_dir);
    void write_target_properties(std::ostream& out, const Project& project, const Solution& solution);
    void write_pch_settings(std::ostream& out, const Project& project, const Solution& solution);
    void write_per_file_settings(std::ostream& out, const Project& project, const Solution& solution,
                                 const std::string& project_dir);
    void write_build_events(std::ostream& out, const Project& project, const Solution& solution);
    void write_custom_build_rules(std::ostream& out, const Project& project, const std::string& project_dir);
    void write_nasm_support(std::ostream& out, const Project& project, const Solution& solution,
                            const std::string& project_dir);

    // Path helpers
    std::string to_cmake_path(const std::string& path);
    std::string compute_relative_path(const std::string& path, const std::filesystem::path& base_dir);

    // Mapping helpers
    std::string map_cpp_standard(const std::string& std_str);
    std::string map_c_standard(const std::string& std_str);

    // Collect unique config names from a project (without platform)
    std::vector<std::string> get_config_names(const Solution& solution) const;

    // Get the first configuration for a given config name
    const Configuration* find_config(const Project& project, const std::string& config_name) const;
};

} // namespace vcxproj
