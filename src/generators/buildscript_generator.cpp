#include "pch.h"
#include "buildscript_generator.hpp"
#include "parsers/vcxproj_reader.hpp"

namespace vcxproj {

bool BuildscriptGenerator::generate(Solution& solution, const std::string& output_dir) {
    std::string base_dir = output_dir.empty() ? "." : output_dir;

    if (!std::filesystem::exists(base_dir)) {
        try {
            std::filesystem::create_directories(base_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create output directory: " << e.what() << "\n";
            return false;
        }
    }

    std::cout << "Generating buildscripts for solution: " << solution.name << "\n";

    BuildscriptWriter writer;
    if (!writer.write_solution_buildscripts(solution, base_dir)) {
        return false;
    }

    std::cout << "Buildscript generation complete!\n";
    return true;
}

REGISTER_GENERATOR(BuildscriptGenerator, "buildscript");

} // namespace vcxproj
