#include "pch.h"
#include "config.hpp"
#include "version.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/cmake_parser.hpp"
#include "parsers/vpc_parser.hpp"
#include "generators/vcxproj_generator.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/deps_exporter.hpp"
#include "generators/cmake_generator.hpp"
#include "parsers/vcxproj_reader.hpp"
#include "parsers/vcproj_reader.hpp"
#include "common/toolset_registry.hpp"
#include "common/build_runner.hpp"
#include "common/updater.hpp"
#include "common/defaults.hpp"

namespace fs = std::filesystem;

void print_usage(const char* program_name) {
    std::cout << "sighmake - Build system generator for C++ projects\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " <input-file> [options]\n";
    std::cout << "  " << program_name << " --build <dir> [build-options]\n";
    std::cout << "  " << program_name << " --convert <file.sln|.slnx|.vcxproj|.vcproj> [options]\n";
    std::cout << "  " << program_name << " convert vpc <file.vpc> [options]\n\n";
    std::cout << "Input formats:\n";
    std::cout << "  .buildscript               Sighmake buildscript (INI-style)\n";
    std::cout << "  CMakeLists.txt / .cmake    CMake project files\n\n";
    std::cout << "Generation options:\n";
    std::cout << "  -g, --generator <type>     Generator type (vcxproj, cmake, makefile, buildscript)\n";
    std::cout << "  -B, --build-dir <dir>      Subdirectory for generated .vcxproj/.sln/.slnx\n";
    std::cout << "                             (vcxproj generator only; default: build)\n";
    std::cout << "  -D <NAME>=<VALUE>          Define a variable for ${NAME} substitution\n";
    std::cout << "  -t, --toolset <name>       Default toolset (msvc2022, msvc2019, etc)\n";
    std::cout << "      --export-deps          Export dependency report as HTML\n\n";
    std::cout << "Build options:\n";
    std::cout << "  -b, --build <dir>          Build using previously generated project files\n";
    std::cout << "      --config <cfg>         Build configuration (e.g. Debug, Release)\n";
    std::cout << "      --target <tgt>         Build specific target\n";
    std::cout << "      --project <name|file>  Build a generated project instead of the whole graph\n";
    std::cout << "      --no-project-references Do not build referenced projects with --project\n";
    std::cout << "      --clean                Clean build artifacts without building\n";
    std::cout << "      --clean-first          Clean before building\n";
    std::cout << "  -j, --parallel <N>         Parallel build jobs\n\n";
    std::cout << "Conversion:\n";
    std::cout << "  -c, --convert              Convert Visual Studio solutions (.sln/.slnx) or\n";
    std::cout << "                             single projects (.vcxproj/.vcproj) to buildscripts\n";
    std::cout << "  convert vpc <file.vpc>     Convert Valve VPC file to buildscript\n\n";
    std::cout << "Info:\n";
    std::cout << "      --version             Show sighmake version\n";
    std::cout << "      update [options]      Update sighmake from GitHub releases\n";
    std::cout << "      --list-toolsets        List available toolsets\n";
    std::cout << "  -l, --list                 List available generators\n";
    std::cout << "  -h, --help                 Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " project.buildscript\n";
    std::cout << "  " << program_name << " project.buildscript -g cmake\n";
    std::cout << "  " << program_name << " CMakeLists.txt -g buildscript\n";
    std::cout << "  " << program_name << " project.buildscript -t msvc2022\n";
    std::cout << "  " << program_name << " project.buildscript -D ENGINE=C:/Engine\n";
    std::cout << "  " << program_name << " CMakeLists.txt -g makefile\n";
    std::cout << "  " << program_name << " --build . --config Release -j 8\n";
    std::cout << "  " << program_name << " --build . --config Debug --project MyPlugin --no-project-references\n";
    std::cout << "  " << program_name << " --convert solution.slnx\n";
    std::cout << "  " << program_name << " --convert legacy.vcproj\n";
    std::cout << "  " << program_name << " update --check-only\n";
    std::cout << "  " << program_name << " convert vpc project.vpc\n\n";
    std::cout << "Environment variables:\n";
    std::cout << "  SIGHMAKE_DEFAULT_TOOLSET   Default toolset when -t is not specified\n";
    std::cout << "  SIGHMAKE_UPDATE_MANIFEST_URL Override updater manifest URL\n";
    std::cout << "  SIGHMAKE_DEBUG             Set to 1 for verbose [DEBUG] diagnostics\n";
}

void print_update_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " update [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "      --check-only           Check latest GitHub release without installing\n";
    std::cout << "      --force                Install latest release even if versions match\n";
    std::cout << "  -h, --help                 Show this help message\n";
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

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        std::cout << "sighmake " << SIGHMAKE_VERSION << "\n";
        std::cout << "release repo: " << SIGHMAKE_RELEASE_REPO << "\n";
        return 0;
    }

    if (strcmp(argv[1], "update") == 0 || strcmp(argv[1], "--update") == 0) {
        vcxproj::updater::UpdateOptions options;
        options.current_version = SIGHMAKE_VERSION;
        options.repository = SIGHMAKE_RELEASE_REPO;
        options.executable_path = vcxproj::updater::current_executable_path(argv[0]);
        options.out = &std::cout;
        options.err = &std::cerr;

        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--check-only") == 0) {
                options.check_only = true;
            } else if (strcmp(argv[i], "--force") == 0) {
                options.force = true;
            } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                print_update_usage(argv[0]);
                return 0;
            } else {
                std::cerr << "Error: Unknown update option: " << argv[i] << "\n";
                print_update_usage(argv[0]);
                return 1;
            }
        }

        return vcxproj::updater::run_update(options);
    }

    // Handle "convert" subcommand
    if (argc >= 2 && strcmp(argv[1], "convert") == 0) {
        if (argc < 4) {
            std::cerr << "Usage: " << argv[0] << " convert <format> <file>\n";
            std::cerr << "Formats: vpc\n";
            return 1;
        }

        std::string format = argv[2];
        std::string input_file = argv[3];

        if (format == "vpc") {
            // Convert VPC to buildscript
            if (!fs::exists(input_file)) {
                std::cerr << "Error: VPC file not found: " << input_file << "\n";
                return 1;
            }

            try {
                std::cout << "Parsing VPC file: " << input_file << "\n";

                vcxproj::VpcParser parser;
                vcxproj::Solution solution = parser.parse(input_file);

                std::cout << "Solution: " << solution.name << "\n";
                std::cout << "Projects: " << solution.projects.size() << "\n";

                vcxproj::BuildscriptWriter writer;
                std::string output_dir = fs::path(input_file).parent_path().string();
                if (output_dir.empty()) {
                    output_dir = ".";
                }

                if (!writer.write_solution_buildscripts(solution, output_dir)) {
                    std::cerr << "Error: Failed to write buildscripts\n";
                    return 1;
                }

                std::cout << "\nSuccess! Converted VPC to " << solution.projects.size() << " buildscript(s).\n";
                return 0;

            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else {
            std::cerr << "Error: Unknown format '" << format << "'. Supported formats: vpc\n";
            return 1;
        }
    }

    // Handle --build mode
    if (argc >= 2 && (strcmp(argv[1], "--build") == 0 || strcmp(argv[1], "-b") == 0)) {
        if (argc < 3) {
            std::cerr << "Error: --build requires a directory argument\n";
            std::cerr << "Usage: " << argv[0] << " --build <dir> [--config <cfg>]\n";
            return 1;
        }

        vcxproj::BuildOptions options;
        options.directory = argv[2];

        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
                options.config = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                options.target = argv[++i];
            } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
                options.project = argv[++i];
            } else if (strcmp(argv[i], "--clean-first") == 0) {
                options.clean_first = true;
            } else if (strcmp(argv[i], "--clean") == 0) {
                options.clean_only = true;
            } else if (strcmp(argv[i], "--no-project-references") == 0 || strcmp(argv[i], "--no-deps") == 0) {
                options.build_project_references = false;
            } else if ((strcmp(argv[i], "--parallel") == 0 || strcmp(argv[i], "-j") == 0) && i + 1 < argc) {
                options.parallel = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: Unknown --build option: " << argv[i] << "\n";
                return 1;
            }
        }

        return vcxproj::BuildRunner::run(options);
    }

    std::string buildscript_path;
    std::string output_dir = ".";
    // vcxproj generator subdir for generated .vcxproj/.sln/.slnx. Default keeps
    // all IDE artifacts in a single build/ tree; users can override.
    std::string build_dir = "build";
    bool build_dir_explicit = false;
#ifdef _WIN32
    std::string generator_type = "vcxproj";  // Default generator
#else
    std::string generator_type = "makefile"; // Default generator for non-Windows
#endif
    std::string default_toolset;
    bool convert_mode = false;
    bool export_deps = false;
    std::map<std::string, std::string> cli_variables;

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
            auto& registry = vcxproj::ToolsetRegistry::instance();
            std::string current_default = registry.get_default();
            std::cout << "Available toolsets:\n\n";
            std::cout << "  Toolset     Description\n";
            std::cout << "  -------     -----------\n";
            for (const auto& alias : registry.toolchain_aliases()) {
                std::cout << "  " << std::left << std::setw(12) << alias.alias
                          << alias.vs_version;
                if (!current_default.empty() && alias.toolset_id == current_default) {
                    std::cout << " (default)";
                }
                std::cout << "\n";
            }
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
        } else if (strcmp(argv[i], "-B") == 0 || strcmp(argv[i], "--build-dir") == 0) {
            if (i + 1 < argc) {
                build_dir = argv[++i];
                build_dir_explicit = true;
            } else {
                std::cerr << "Error: -B requires an argument\n";
                return 1;
            }
        } else if (strcmp(argv[i], "--export-deps") == 0) {
            export_deps = true;
        } else if (strcmp(argv[i], "-D") == 0) {
            if (i + 1 < argc) {
                std::string def = argv[++i];
                size_t eq = def.find('=');
                if (eq != std::string::npos && eq > 0) {
                    cli_variables[def.substr(0, eq)] = def.substr(eq + 1);
                } else {
                    std::cerr << "Error: -D requires NAME=VALUE format\n";
                    return 1;
                }
            } else {
                std::cerr << "Error: -D requires an argument\n";
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
            std::string input_ext = input_path.extension().string();
            std::transform(input_ext.begin(), input_ext.end(), input_ext.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

            if (input_ext != ".sln" && input_ext != ".slnx" &&
                input_ext != ".vcxproj" && input_ext != ".vcproj") {
                std::cerr << "Error: Conversion mode requires a .sln, .slnx, .vcxproj or .vcproj file\n";
                return 1;
            }

            vcxproj::Solution solution;

            if (input_ext == ".vcxproj" || input_ext == ".vcproj") {
                // Standalone project: wrap it in a single-project solution so
                // the buildscript writer emits one merged buildscript.
                std::cout << "Converting project: " << buildscript_path << "\n";

                vcxproj::Project proj;
                if (input_ext == ".vcproj") {
                    vcxproj::VcprojReader reader;
                    proj = reader.read_vcproj(buildscript_path);
                } else {
                    vcxproj::VcxprojReader reader;
                    proj = reader.read_vcxproj(buildscript_path);
                }

                if (proj.name.empty()) {
                    proj.name = input_path.stem().string();
                }
                proj.vcxproj_path = input_path.filename().string();

                solution.name = proj.name;
                solution.uuid = vcxproj::generate_uuid();

                std::set<std::string> configs, platforms;
                for (const auto& [config_key, cfg] : proj.configurations) {
                    (void)cfg;
                    auto [config_name, platform_name] = vcxproj::parse_config_key(config_key);
                    if (!config_name.empty()) configs.insert(config_name);
                    if (!platform_name.empty()) platforms.insert(vcxproj::normalize_platform(platform_name));
                }
                solution.configurations = configs.empty()
                    ? vcxproj::defaults::configurations()
                    : std::vector<std::string>(configs.begin(), configs.end());
                solution.platforms = platforms.empty()
                    ? vcxproj::defaults::platforms()
                    : std::vector<std::string>(platforms.begin(), platforms.end());

                solution.projects.push_back(std::move(proj));
            } else {
                std::cout << "Converting solution: " << buildscript_path << "\n";

                vcxproj::SlnReader sln_reader;
                solution = (input_ext == ".slnx")
                    ? sln_reader.read_slnx(buildscript_path)
                    : sln_reader.read_sln(buildscript_path);
            }

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

            if (export_deps) {
                vcxproj::export_dependencies_html(solution, base_dir);
            }

            std::cout << "\nSuccess! Generated " << solution.projects.size() << " buildscript(s).\n";
            return 0;
        }

        // Normal mode: buildscript -> project files
        vcxproj::Solution solution;
        fs::path input_path(buildscript_path);
        std::string filename = input_path.filename().string();
        std::string ext = input_path.extension().string();
        
        // Convert to lowercase for checking
        std::transform(filename.begin(), filename.end(), filename.begin(), 
                      [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        
        if (filename == "cmakelists.txt" || ext == ".cmake") {
            std::cout << "Parsing CMake file: " << buildscript_path << "\n";
            vcxproj::CMakeParser parser;
            solution = parser.parse(buildscript_path);
        } else {
            std::cout << "Parsing buildscript: " << buildscript_path << "\n";
            vcxproj::BuildscriptParser parser;
            if (!cli_variables.empty()) {
                parser.set_variables(cli_variables);
            }
            solution = parser.parse(buildscript_path);
        }

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

        // -B / --build-dir only affects the vcxproj generator today. Warn if
        // the flag was explicit but points at a generator that doesn't use it,
        // so the user knows it's being ignored rather than silently honored.
        if (auto* vcxgen = dynamic_cast<vcxproj::VcxprojGenerator*>(generator.get())) {
            vcxgen->set_build_dir(build_dir);
        } else if (build_dir_explicit) {
            std::cerr << "Warning: -B / --build-dir is only honored by the vcxproj generator; ignored for '"
                      << generator->name() << "'.\n";
        }

        // Generate project files
        if (!generator->generate(solution, output_dir)) {
            std::cerr << "Error: Generation failed\n";
            return 1;
        }

        // Export dependency report if requested
        if (export_deps) {
            if (!vcxproj::export_dependencies_html(solution, output_dir)) {
                std::cerr << "Warning: Failed to generate dependency report\n";
            }
        }

        std::cout << "\nSuccess! All files generated.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
