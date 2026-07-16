#pragma once

#include "common/project_types.hpp"
#include "common/generator.hpp"

namespace vcxproj {

// Generator for GNU Makefiles
class MakefileGenerator : public Generator {
public:
    MakefileGenerator() = default;

    // Generate all Makefiles (implements Generator interface)
    bool generate(Solution& solution, const std::string& output_dir) override;

    // Get generator name
    std::string name() const override { return "makefile"; }

    // Get generator description
    std::string description() const override {
        return "GNU Makefile generator for Linux/macOS (GCC/Clang) and Android (NDK)";
    }

    // Generate a Makefile for a specific project and configuration
    bool generate_makefile(const Project& project, const Solution& solution,
                          const std::string& config_key, const std::string& output_path);

    // Generate master Makefile to build all projects
    bool generate_master_makefile(const Solution& solution, const std::string& output_dir);

private:
    using ProjectLookup = std::map<std::string, const Project*>;

    bool generate_makefile_with_lookup(const Project& project, const Solution& solution,
                                       const std::string& config_key, const std::string& output_path,
                                       const ProjectLookup& project_lookup);

    // Helper functions for mapping settings to flags
    std::string map_optimization(const std::string& opt);
    std::string map_warning_level(const std::string& level);
    std::string get_compiler_flags(const Configuration& config, const Project& project,
                                   const std::filesystem::path& makefile_dir, bool c_flags);
    std::string get_linker_flags(const Configuration& config, const std::filesystem::path& makefile_dir,
                                 bool android);
    std::string get_linker_libs(const Configuration& config);

    // Helper to convert Windows paths to Unix paths
    std::string to_unix_path(const std::string& path);

    // Helper to compute relative path from makefile directory
    std::string compute_relative_path(const std::string& path, const std::filesystem::path& makefile_dir);

    // Build a unique object path under OBJ_DIR for a source file
    std::string make_object_path(const SourceFile& src, const std::string& src_relative,
                                 const std::string& config_key, const std::string& int_dir,
                                 const std::filesystem::path& makefile_dir);

    // Strip .lib or .dll extension from library names
    std::string strip_lib_extension(const std::string& lib);

    // PCH helper functions
    std::pair<bool, std::string> get_pch_info(const Configuration& config);
    std::tuple<std::string, std::string> get_file_pch_mode(
        const SourceFile& src,
        const std::string& config_key,
        const Configuration& config);
};

} // namespace vcxproj
