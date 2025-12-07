#include "buildscript_parser.hpp"
#include "vcxproj_generator.hpp"
#include <iostream>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

void print_usage(const char* program_name) {
    std::cout << "sighmake - Build system generator for Visual Studio projects\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " <buildscript> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -b, --build <dir>    Output build directory (default: current directory)\n";
    std::cout << "  -h, --help            Show this help message\n\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " build.txt -b ./build \n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string buildscript_path;
    std::string output_dir = ".";

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--build") == 0) {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            } else {
                std::cerr << "Error: -b requires an argument\n";
                return 1;
            }
        } else if (buildscript_path.empty()) {
            buildscript_path = argv[i];
        } else {
            std::cerr << "Error: Unexpected argument: " << argv[i] << "\n";
            return 1;
        }
    }

    if (buildscript_path.empty()) {
        std::cerr << "Error: No buildscript specified\n";
        print_usage(argv[0]);
        return 1;
    }

    // Check if buildscript exists
    if (!fs::exists(buildscript_path)) {
        std::cerr << "Error: Buildscript not found: " << buildscript_path << "\n";
        return 1;
    }

    // Create output directory if it doesn't exist
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        try {
            fs::create_directories(output_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create output directory: " << e.what() << "\n";
            return 1;
        }
    }

    try {
        // Parse buildscript
        std::cout << "Parsing buildscript: " << buildscript_path << "\n";
        vcxproj::BuildscriptParser parser;
        vcxproj::Solution solution = parser.parse(buildscript_path);

        std::cout << "Solution: " << solution.name << "\n";
        std::cout << "Projects: " << solution.projects.size() << "\n";

        // Generate project files
        vcxproj::VcxprojGenerator generator;

        for (const auto& project : solution.projects) {
            std::cout << "Generating project: " << project.name << "\n";

            fs::path vcxproj_path = fs::path(output_dir) / (project.name + ".vcxproj");

            if (!generator.generate_vcxproj(project, solution, vcxproj_path.string())) {
                std::cerr << "Error: Failed to generate " << vcxproj_path << "\n";
                return 1;
            }

            std::cout << "  Generated: " << vcxproj_path << "\n";
        }

        // Generate solution file
        if (!solution.projects.empty()) {
            std::string sln_name = solution.name.empty() ? solution.projects[0].name : solution.name;
            fs::path sln_path = fs::path(output_dir) / (sln_name + ".sln");

            std::cout << "Generating solution: " << sln_name << "\n";

            if (!generator.generate_sln(solution, sln_path.string())) {
                std::cerr << "Error: Failed to generate " << sln_path << "\n";
                return 1;
            }

            std::cout << "  Generated: " << sln_path << "\n";
        }

        std::cout << "\nSuccess! All files generated.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
