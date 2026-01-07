#include "config.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/vcxproj_generator.hpp"
#include "generators/makefile_generator.hpp"
#include "parsers/vcxproj_reader.hpp"
#include "common/toolset_registry.hpp"

// Standard library includes
#include <iostream>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

void print_usage(const char* program_name) {
    std::cout << "sighmake - Build system generator\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " <buildscript> [options]\n";
    std::cout << "  " << program_name << " --convert <solution.sln> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -g, --generator <type>     Generator type (default: vcxproj)\n";
    std::cout << "  -c, --convert              Convert Visual Studio solution to buildscripts\n";
    std::cout << "  -t, --toolset <name>       Default toolset (msvc2022, msvc2019, etc)\n";
    std::cout << "      --list-toolsets        List available toolsets\n";
    std::cout << "  -l, --list                 List available generators\n";
    std::cout << "  -h, --help                 Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " project.buildscript -t msvc2022\n";
    std::cout << "  " << program_name << " --convert solution.sln\n";
}

int main(int argc, char* argv[]) {

    // Check for SIGHMAKE_DEFAULT_TOOLSET environment variable
    if (const char* env_toolset = std::getenv("SIGHMAKE_DEFAULT_TOOLSET")) {
        auto& registry = vcxproj::ToolsetRegistry::instance();
        auto resolved = registry.resolve(env_toolset);
        if (resolved) {
            registry.set_default(*resolved);
        }
    }
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string buildscript_path;
    std::string output_dir = ".";
#ifdef _WIN32
    std::string generator_type = "vcxproj";  // Default generator
#else
    std::string generator_type = "makefile"; // Default generator for non-Windows
#endif
    std::string default_toolset;
    bool convert_mode = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--convert") == 0) {
            convert_mode = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--toolset") == 0) {
            if (i + 1 < argc) {
                default_toolset = argv[++i];
            } else {
                std::cerr << "Error: -t requires an argument\n";
                return 1;
            }
        } else if (strcmp(argv[i], "--list-toolsets") == 0) {
            std::cout << "Available toolsets:\n\n";
            std::cout << "  Toolset     Description\n";
            std::cout << "  -------     -----------\n";
            std::cout << "  msvc2026    Visual Studio 2026\n";
            std::cout << "  msvc2022    Visual Studio 2022 (default)\n";
            std::cout << "  msvc2019    Visual Studio 2019\n";
            std::cout << "  msvc2017    Visual Studio 2017\n";
            std::cout << "  msvc2015    Visual Studio 2015\n";
            std::cout << "  msvc2013    Visual Studio 2013\n";
            std::cout << "  msvc2012    Visual Studio 2012\n";
            std::cout << "  msvc2010    Visual Studio 2010\n";
            std::cout << "\nUse the normalized toolset name (e.g., 'msvc2022').\n";
            std::cout << "Case-insensitive: MSVC2022, msvc2022, Msvc2022 all work.\n";
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            std::cout << "Available generators:\n";
            auto& factory = vcxproj::GeneratorFactory::instance();
            for (const auto& name : factory.available_generators()) {
                auto gen = factory.create(name);
                if (gen) {
                    std::cout << "  " << name << " - " << gen->description() << "\n";
                }
            }
            return 0;
        } else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--generator") == 0) {
            if (i + 1 < argc) {
                generator_type = argv[++i];
            } else {
                std::cerr << "Error: -g requires an argument\n";
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
        std::cerr << "Error: No input file specified\n";
        print_usage(argv[0]);
        return 1;

    }
    // Apply CLI default toolset if specified
    if (!default_toolset.empty()) {
        auto& registry = vcxproj::ToolsetRegistry::instance();
        auto resolved = registry.resolve(default_toolset);
        if (resolved) {
            registry.set_default(*resolved);
            std::cout << "Using default toolset: " << *resolved << "\n";
        } else {
            std::cerr << "Warning: Could not resolve toolset '" << default_toolset << "'\n";
        }
    }

    // Check if input file exists
    if (!fs::exists(buildscript_path)) {
        std::cerr << "Error: Input file not found: " << buildscript_path << "\n";
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
        // Handle conversion mode (solution -> buildscripts)
        if (convert_mode) {
            fs::path input_path(buildscript_path);

            // Check if it's a .sln file
            if (input_path.extension() != ".sln") {
                std::cerr << "Error: Conversion mode requires a .sln file\n";
                return 1;
            }

            std::cout << "Converting solution: " << buildscript_path << "\n";

            vcxproj::SlnReader sln_reader;
            vcxproj::Solution solution = sln_reader.read_sln(buildscript_path);

            std::cout << "Solution: " << solution.name << "\n";
            std::cout << "Projects: " << solution.projects.size() << "\n";

            vcxproj::BuildscriptWriter writer;

            // Determine base directory (use output_dir if specified, otherwise solution directory)
            std::string base_dir = output_dir;
            if (base_dir == ".") {
                base_dir = input_path.parent_path().string();
                if (base_dir.empty()) {
                    base_dir = ".";
                }
            }

            if (!writer.write_solution_buildscripts(solution, base_dir)) {
                std::cerr << "Error: Failed to write buildscripts\n";
                return 1;
            }

            std::cout << "\nSuccess! Generated " << solution.projects.size() << " buildscript(s).\n";
            return 0;
        }

        // Normal mode: buildscript -> project files
        // Parse buildscript
        std::cout << "Parsing buildscript: " << buildscript_path << "\n";
        vcxproj::BuildscriptParser parser;
        vcxproj::Solution solution = parser.parse(buildscript_path);

        std::cout << "Solution: " << solution.name << "\n";
        std::cout << "Projects: " << solution.projects.size() << "\n";

        // Create the appropriate generator
        auto& factory = vcxproj::GeneratorFactory::instance();
        auto generator = factory.create(generator_type);

        if (!generator) {
            std::cerr << "Error: Unknown generator type: " << generator_type << "\n";
            std::cerr << "Available generators:\n";
            for (const auto& name : factory.available_generators()) {
                auto gen = factory.create(name);
                if (gen) {
                    std::cerr << "  " << name << " - " << gen->description() << "\n";
                }
            }
            return 1;
        }

        std::cout << "Using generator: " << generator->name() << "\n";

        // Generate project files
        if (!generator->generate(solution, output_dir)) {
            std::cerr << "Error: Generation failed\n";
            return 1;
        }

        std::cout << "\nSuccess! All files generated.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
