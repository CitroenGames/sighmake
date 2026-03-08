#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;

// Helper to find a project by name
static const Project* find_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

// Helper to check if a vector contains a string
static bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

// Helper to check if any string in a vector contains a substring
static bool contains_substring(const std::vector<std::string>& vec, const std::string& sub) {
    for (const auto& s : vec) {
        if (s.find(sub) != std::string::npos) return true;
    }
    return false;
}

// ============================================================================
// Solution-level parsing
// ============================================================================

TEST_CASE("Parse solution name", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = TestSolution
)");
    CHECK(sol.name == "TestSolution");
}

TEST_CASE("Parse solution configurations", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release, Profile
platforms = Win32, x64
)");
    REQUIRE(sol.configurations.size() == 3);
    CHECK(sol.configurations[0] == "Debug");
    CHECK(sol.configurations[1] == "Release");
    CHECK(sol.configurations[2] == "Profile");
    REQUIRE(sol.platforms.size() == 2);
    CHECK(sol.platforms[0] == "Win32");
    CHECK(sol.platforms[1] == "x64");
}

TEST_CASE("Default configurations when not specified", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
[project:App]
type = exe
)");
    CHECK(sol.configurations.size() >= 2);
    CHECK(sol.platforms.size() >= 1);
}

TEST_CASE("Solution name falls back to first project name", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[project:MyApp]
type = exe
)");
    CHECK(sol.name == "MyApp");
}

// ============================================================================
// Project-level parsing
// ============================================================================

TEST_CASE("Parse project name", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:MyApp]
type = exe
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "MyApp");
}

TEST_CASE("Parse project type exe", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");
    REQUIRE(sol.projects.size() == 1);
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.config_type == "Application");
}

TEST_CASE("Parse project type lib", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.config_type == "StaticLibrary");
}

TEST_CASE("Parse project type dll", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = dll
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.config_type == "DynamicLibrary");
}

TEST_CASE("Parse C++ standard", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
std = 17
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.language_standard == "stdcpp17");
}

TEST_CASE("Parse C++ standard 20", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
std = 20
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.language_standard == "stdcpp20");
}

TEST_CASE("Parse warning level", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
warning_level = Level4
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.warning_level == "Level4");
}

TEST_CASE("Parse multiprocessor compilation", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
multiprocessor = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.multi_processor_compilation == true);
}

TEST_CASE("Parse RTTI setting", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
rtti = false
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.runtime_type_info == false);
}

TEST_CASE("Parse exception handling", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
exception_handling = Sync
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.exception_handling == "Sync");
}

TEST_CASE("Parse subsystem", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
subsystem = Console
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.sub_system == "Console");
}

TEST_CASE("Parse target name and extension", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
target_name = myapp
target_ext = .exe
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.target_name == "myapp");
    CHECK(cfg.target_ext == ".exe");
}

TEST_CASE("Parse preprocessor definitions", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
defines = FOO, BAR, _CRT_SECURE_NO_WARNINGS
)");
    auto& defs = sol.projects[0].project_level_preprocessor_definitions;
    CHECK(contains(defs, "FOO"));
    CHECK(contains(defs, "BAR"));
    CHECK(contains(defs, "_CRT_SECURE_NO_WARNINGS"));
}

TEST_CASE("Parse project language setting", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
language = C
)");
    CHECK(sol.projects[0].language == "C");
}

TEST_CASE("Parse project dependencies", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib

[project:App]
type = exe
depends = LibA
)");
    REQUIRE(sol.projects.size() == 2);
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);
    REQUIRE(app->project_references.size() == 1);
    CHECK(app->project_references[0].name == "LibA");
}

TEST_CASE("Parse multiple projects", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib

[project:LibB]
type = dll

[project:App]
type = exe
)");
    REQUIRE(sol.projects.size() == 3);
    CHECK(find_project(sol, "LibA") != nullptr);
    CHECK(find_project(sol, "LibB") != nullptr);
    CHECK(find_project(sol, "App") != nullptr);
}

// ============================================================================
// Per-configuration settings
// ============================================================================

TEST_CASE("Parse per-config optimization with bracket notation", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
optimization[Debug|Win32] = Disabled
optimization[Release|Win32] = MaxSpeed
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.optimization == "Disabled");
    CHECK(sol.projects[0].configurations["Release|Win32"].cl_compile.optimization == "MaxSpeed");
}

TEST_CASE("Parse per-config runtime library", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
runtime_library[Debug|Win32] = MultiThreadedDebugDLL
runtime_library[Release|Win32] = MultiThreadedDLL
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.runtime_library == "MultiThreadedDebugDLL");
    CHECK(sol.projects[0].configurations["Release|Win32"].cl_compile.runtime_library == "MultiThreadedDLL");
}

TEST_CASE("Parse per-config defines", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
defines[Debug|Win32] = _DEBUG
defines[Release|Win32] = NDEBUG
)");
    auto& debug_defs = sol.projects[0].configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    auto& release_defs = sol.projects[0].configurations["Release|Win32"].cl_compile.preprocessor_definitions;
    CHECK(contains(debug_defs, "_DEBUG"));
    CHECK(contains(release_defs, "NDEBUG"));
}

// ============================================================================
// Per-file settings
// ============================================================================

TEST_CASE("Parse per-file PCH settings", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
pch.cpp:pch_header = pch.h
special.cpp:pch = NotUsing
)");
    // Check the project-level PCH setting
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.pch.mode == "Use");

    // Find the files and check their per-file settings
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        std::string filename = p.filename().string();

        if (filename == "pch.cpp") {
            // pch.cpp should have Create mode
            bool found_create = false;
            for (const auto& [key, pch_setting] : src.settings.pch) {
                if (pch_setting.mode == "Create") found_create = true;
            }
            CHECK(found_create);
        }
        if (filename == "special.cpp") {
            // special.cpp should have NotUsing mode
            bool found_not_using = false;
            for (const auto& [key, pch_setting] : src.settings.pch) {
                if (pch_setting.mode == "NotUsing") found_not_using = true;
            }
            CHECK(found_not_using);
        }
    }
}

TEST_CASE("Parse per-file optimization", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
slow.cpp:optimization = Disabled
)");
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        if (p.filename().string() == "slow.cpp") {
            CHECK(src.settings.optimization.count(ALL_CONFIGS) > 0);
            CHECK(src.settings.optimization.at(ALL_CONFIGS) == "Disabled");
        }
    }
}

TEST_CASE("Parse per-file per-config optimization", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
hot.cpp:optimization[Release|Win32] = Full
)");
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        if (p.filename().string() == "hot.cpp") {
            CHECK(src.settings.optimization.count("Release|Win32") > 0);
            CHECK(src.settings.optimization.at("Release|Win32") == "Full");
        }
    }
}

TEST_CASE("Parse per-file compile_as", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
legacy.cpp:compile_as = CompileAsC
)");
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        if (p.filename().string() == "legacy.cpp") {
            CHECK(src.settings.compile_as.count(ALL_CONFIGS) > 0);
            CHECK(src.settings.compile_as.at(ALL_CONFIGS) == "CompileAsC");
        }
    }
}

TEST_CASE("Parse per-file exclude", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
old.cpp:exclude = true
)");
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        if (p.filename().string() == "old.cpp") {
            bool excluded = false;
            for (const auto& [key, val] : src.settings.excluded) {
                if (val) excluded = true;
            }
            CHECK(excluded);
        }
    }
}

// ============================================================================
// Multiline values
// ============================================================================

TEST_CASE("Parse brace-block sources", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = {
    main.cpp
    util.cpp
    helper.cpp
}
)");
    // Should have 3 source files
    REQUIRE(sol.projects[0].sources.size() == 3);
}

TEST_CASE("Parse triple-quoted multiline postbuild", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(
        "[solution]\n"
        "name = Test\n"
        "configurations = Debug\n"
        "platforms = Win32\n"
        "\n"
        "[project:App]\n"
        "type = exe\n"
        "postbuild = \"\"\"echo line1\n"
        "echo line2\"\"\"\n"
    );
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    // Triple-quoted values are preprocessed and unescaped
    // The command should be non-empty and contain the text
    CHECK(!cfg.post_build_event.command.empty());
}

// ============================================================================
// target_link_libraries()
// ============================================================================

TEST_CASE("Parse target_link_libraries basic", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib

[project:LibB]
type = lib

[project:App]
type = exe
target_link_libraries(LibA LibB)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);
    REQUIRE(app->project_references.size() == 2);
    CHECK(app->project_references[0].name == "LibA");
    CHECK(app->project_references[1].name == "LibB");
}

TEST_CASE("Parse target_link_libraries with visibility", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib

[project:LibB]
type = lib

[project:App]
type = exe
target_link_libraries(
    PUBLIC
        LibA
    PRIVATE
        LibB
)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);
    REQUIRE(app->project_references.size() == 2);

    // Find LibA and LibB dependencies
    const ProjectDependency* dep_a = nullptr;
    const ProjectDependency* dep_b = nullptr;
    for (const auto& dep : app->project_references) {
        if (dep.name == "LibA") dep_a = &dep;
        if (dep.name == "LibB") dep_b = &dep;
    }
    REQUIRE(dep_a != nullptr);
    REQUIRE(dep_b != nullptr);
    CHECK(dep_a->visibility == DependencyVisibility::PUBLIC);
    CHECK(dep_b->visibility == DependencyVisibility::PRIVATE);
}

// ============================================================================
// Config sections
// ============================================================================

TEST_CASE("Parse config section with config|platform", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
optimization = Disabled
runtime_library = MultiThreadedDebug

[config:Release|Win32]
optimization = MaxSpeed
runtime_library = MultiThreaded
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.optimization == "Disabled");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.runtime_library == "MultiThreadedDebug");
    CHECK(sol.projects[0].configurations["Release|Win32"].cl_compile.optimization == "MaxSpeed");
    CHECK(sol.projects[0].configurations["Release|Win32"].cl_compile.runtime_library == "MultiThreaded");
}

// ============================================================================
// Default values
// ============================================================================

TEST_CASE("Debug defaults are applied when no config sections defined", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");
    auto& debug_cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(debug_cfg.cl_compile.optimization == "Disabled");
    CHECK(debug_cfg.cl_compile.runtime_library == "MultiThreadedDebug");
    CHECK(debug_cfg.link.generate_debug_info == true);
}

TEST_CASE("Release defaults are applied when no config sections defined", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");
    auto& release_cfg = sol.projects[0].configurations["Release|Win32"];
    CHECK(release_cfg.cl_compile.optimization == "MaxSpeed");
    CHECK(release_cfg.cl_compile.runtime_library == "MultiThreaded");
    CHECK(release_cfg.cl_compile.function_level_linking == true);
    CHECK(release_cfg.cl_compile.intrinsic_functions == true);
    CHECK(release_cfg.link.enable_comdat_folding == true);
    CHECK(release_cfg.link.optimize_references == true);
}

TEST_CASE("Character set defaults to MultiByte", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].character_set == "MultiByte");
}

TEST_CASE("Windows target platform version defaults to 10.0", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].windows_target_platform_version == "10.0");
}

// ============================================================================
// Solution-level defines
// ============================================================================

TEST_CASE("Solution-level defines are stored", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32
defines = GLOBAL_DEF

[project:App]
type = exe
)");
    CHECK(contains(sol.solution_level_preprocessor_definitions, "GLOBAL_DEF"));
}

// ============================================================================
// UTF-8 setting
// ============================================================================

TEST_CASE("Parse utf8 setting", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
utf8 = true
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.utf8_source == true);
}

// ============================================================================
// Public properties
// ============================================================================

TEST_CASE("Parse public_includes", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib
public_includes = include
)");
    CHECK(contains_substring(sol.projects[0].public_includes, "include"));
}

TEST_CASE("Parse public_defines", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib
public_defines = LIB_EXPORTED
)");
    CHECK(contains(sol.projects[0].public_defines, "LIB_EXPORTED"));
}

TEST_CASE("Parse public_libs", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib
public_libs = kernel32.lib
)");
    CHECK(contains(sol.projects[0].public_libs, "kernel32.lib"));
}

// ============================================================================
// C standard
// ============================================================================

TEST_CASE("Parse C standard", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
language = C
c_standard = 11
)");
    CHECK(sol.projects[0].language == "C");
    CHECK(sol.projects[0].c_standard == "11");
}

// ============================================================================
// Type aliases
// ============================================================================

TEST_CASE("Parse type aliases", "[buildscript_parser]") {
    BuildscriptParser parser;

    SECTION("static -> StaticLibrary") {
        auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32
[project:Lib]
type = static
)");
        CHECK(sol.projects[0].configurations["Debug|Win32"].config_type == "StaticLibrary");
    }

    SECTION("shared -> DynamicLibrary") {
        auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32
[project:Lib]
type = shared
)");
        CHECK(sol.projects[0].configurations["Debug|Win32"].config_type == "DynamicLibrary");
    }

    SECTION("application -> Application") {
        auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32
[project:App]
type = application
)");
        CHECK(sol.projects[0].configurations["Debug|Win32"].config_type == "Application");
    }
}

// ============================================================================
// Conditional sections (if/platform)
// ============================================================================

TEST_CASE("Parse if(windows) block on Windows", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
if(windows) {
    defines = WIN_ONLY
}
)");
#if defined(_WIN32)
    CHECK(contains(sol.projects[0].project_level_preprocessor_definitions, "WIN_ONLY"));
#endif
}

TEST_CASE("Parse if(linux) block is skipped on Windows", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
if(linux) {
    defines = LINUX_ONLY
}
)");
#if defined(_WIN32)
    CHECK_FALSE(contains(sol.projects[0].project_level_preprocessor_definitions, "LINUX_ONLY"));
#endif
}

TEST_CASE("Parse if with negation", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
if(!linux) {
    defines = NOT_LINUX
}
)");
#if defined(_WIN32)
    CHECK(contains(sol.projects[0].project_level_preprocessor_definitions, "NOT_LINUX"));
#endif
}

TEST_CASE("Nested if blocks", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
if(windows) {
    defines = OUTER
    if(!linux) {
        defines = INNER
    }
}
)");
#if defined(_WIN32)
    CHECK(contains(sol.projects[0].project_level_preprocessor_definitions, "OUTER"));
    CHECK(contains(sol.projects[0].project_level_preprocessor_definitions, "INNER"));
#endif
}

TEST_CASE("False outer if skips inner content", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
if(linux) {
    defines = LINUX_DEF
    if(windows) {
        defines = NESTED_WIN
    }
}
)");
#if defined(_WIN32)
    CHECK_FALSE(contains(sol.projects[0].project_level_preprocessor_definitions, "LINUX_DEF"));
    CHECK_FALSE(contains(sol.projects[0].project_level_preprocessor_definitions, "NESTED_WIN"));
#endif
}

// ============================================================================
// Additional build settings
// ============================================================================

TEST_CASE("Parse additional_options", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
additional_options = /W4 /WX
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.additional_options.find("/W4") != std::string::npos);
}

TEST_CASE("Parse additional_dependencies", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
additional_dependencies = ws2_32.lib, user32.lib
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains(cfg.link.additional_dependencies, "ws2_32.lib"));
    CHECK(contains(cfg.link.additional_dependencies, "user32.lib"));
}

TEST_CASE("Parse libdirs", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
libdirs = lib/x86
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains_substring(cfg.link.additional_library_directories, "lib"));
}

TEST_CASE("Parse prebuild event", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
prebuild = echo hello
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(!cfg.pre_build_event.command.empty());
}

TEST_CASE("Parse postbuild event as single line", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
postbuild = copy output
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(!cfg.post_build_event.command.empty());
}

// ============================================================================
// Link settings
// ============================================================================

TEST_CASE("Parse output directory settings", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
outdir[Debug|Win32] = bin/debug
outdir[Release|Win32] = bin/release
intdir[Debug|Win32] = obj/debug
intdir[Release|Win32] = obj/release
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].out_dir.find("debug") != std::string::npos);
    CHECK(sol.projects[0].configurations["Release|Win32"].out_dir.find("release") != std::string::npos);
    CHECK(sol.projects[0].configurations["Debug|Win32"].int_dir.find("debug") != std::string::npos);
}

TEST_CASE("Parse libs with system libraries", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
libs = kernel32.lib
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);
    // System libraries go to additional_dependencies
    bool found = false;
    for (const auto& [key, cfg] : app->configurations) {
        if (contains(cfg.link.additional_dependencies, "kernel32.lib")) {
            found = true;
        }
    }
    CHECK(found);
}

// ============================================================================
// Multi-line and edge cases
// ============================================================================

TEST_CASE("Parse brace-block headers", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
headers = {
    types.h
    utils.h
}
)");
    int header_count = 0;
    for (const auto& src : sol.projects[0].sources) {
        if (src.type == FileType::ClInclude) header_count++;
    }
    CHECK(header_count >= 2);
}

TEST_CASE("Comments and blank lines are ignored", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
# This is a comment
[solution]
name = Test
; This is also a comment
configurations = Debug
platforms = Win32

# Project comment
[project:App]
type = exe
)");
    CHECK(sol.name == "Test");
    CHECK(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "App");
}

TEST_CASE("Parse folder() block groups sources", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp
folder("Core") {
    sources = engine.cpp
}
)");
    CHECK(sol.projects[0].sources.size() >= 2);
}

TEST_CASE("Parse multiple defines with config section", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
defines = COMMON_DEF

[config:Debug|Win32]
defines = _DEBUG, DEBUG_MODE

[config:Release|Win32]
defines = NDEBUG, RELEASE_MODE
)");
    auto& debug_defs = sol.projects[0].configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    auto& release_defs = sol.projects[0].configurations["Release|Win32"].cl_compile.preprocessor_definitions;
    CHECK(contains(debug_defs, "_DEBUG"));
    CHECK(contains(release_defs, "NDEBUG"));
    // Common define should also be present
    CHECK(contains(debug_defs, "COMMON_DEF"));
    CHECK(contains(release_defs, "COMMON_DEF"));
}

TEST_CASE("Parse pch settings explicitly", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
pch = Use
pch_header = pch.h
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.pch.mode == "Use");
    CHECK(cfg.cl_compile.pch.header == "pch.h");
}

TEST_CASE("Parse include directories", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
includes = src, include, third_party
)");
    auto& inc = sol.projects[0].configurations["Debug|Win32"].cl_compile.additional_include_directories;
    CHECK(contains_substring(inc, "src"));
    CHECK(contains_substring(inc, "include"));
    CHECK(contains_substring(inc, "third_party"));
}

TEST_CASE("Parse exception handling Async", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
exception_handling = Async
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.exception_handling == "Async");
}

TEST_CASE("Parse subsystem Windows", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
subsystem = Windows
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.sub_system == "Windows");
}

TEST_CASE("Parse multiprocessor false", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
multiprocessor = false
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.multi_processor_compilation == false);
}

TEST_CASE("Parse RTTI true by default", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.runtime_type_info == true);
}

TEST_CASE("Parse multiple platforms", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32, x64

[project:App]
type = exe
)");
    CHECK(sol.platforms.size() == 2);
    CHECK(sol.projects[0].configurations.count("Debug|Win32"));
    CHECK(sol.projects[0].configurations.count("Debug|x64"));
}

TEST_CASE("Parse toolset setting", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
toolset = v142
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.platform_toolset == "v142");
}

TEST_CASE("Parse charset Unicode", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
charset = Unicode
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.character_set == "Unicode");
}

// ============================================================================
// Advanced compiler settings
// ============================================================================

TEST_CASE("Parse enhanced_instruction_set", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
enhanced_instruction_set = AdvancedVectorExtensions2
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.enhanced_instruction_set == "AdvancedVectorExtensions2");
}

TEST_CASE("Parse simd alias for enhanced_instruction_set", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
simd = StreamingSIMDExtensions2
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.enhanced_instruction_set == "StreamingSIMDExtensions2");
}

TEST_CASE("Parse floating_point_model", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
floating_point_model = Fast
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.floating_point_model == "Fast");
}

TEST_CASE("Parse fp_model alias", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
fp_model = Strict
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.floating_point_model == "Strict");
}

TEST_CASE("Parse inline_function_expansion", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
inline_function_expansion = AnySuitable
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.inline_function_expansion == "AnySuitable");
}

TEST_CASE("Parse favor_size_or_speed", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
favor_size_or_speed = Speed
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.favor_size_or_speed == "Speed");
}

TEST_CASE("Parse string_pooling", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
string_pooling = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.string_pooling == true);
}

TEST_CASE("Parse buffer_security_check", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
buffer_security_check = false
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.buffer_security_check == false);
}

TEST_CASE("Parse treat_warning_as_error", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
treat_warning_as_error = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.treat_warning_as_error == true);
}

TEST_CASE("Parse openmp_support", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
openmp = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.openmp_support == true);
}

// ============================================================================
// Linker settings
// ============================================================================

TEST_CASE("Parse linker output_file", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
output_file = $(OutDir)myapp.exe
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.output_file == "$(OutDir)myapp.exe");
}

TEST_CASE("Parse linker base_address", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
base_address = 0x10000000
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.base_address == "0x10000000");
}

TEST_CASE("Parse linker entry_point", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
entry_point = mainCRTStartup
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.entry_point_symbol == "mainCRTStartup");
}

TEST_CASE("Parse linker generate_map_file", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
generate_map_file = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.generate_map_file == true);
}

TEST_CASE("Parse linker show_progress", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
show_progress = LinkVerbose
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.show_progress == "LinkVerbose");
}

TEST_CASE("Parse linker safe_seh", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
safe_seh = true
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.image_has_safe_exception_handlers == true);
}

TEST_CASE("Parse linker error_reporting", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
link_error_reporting = PromptImmediately
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.error_reporting == "PromptImmediately");
}

// ============================================================================
// Librarian settings
// ============================================================================

TEST_CASE("Parse librarian output_file", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib

[config:Debug|Win32]
lib_output_file = $(OutDir)mylib.lib
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.lib.output_file == "$(OutDir)mylib.lib");
}

TEST_CASE("Parse librarian additional_dependencies via libs key", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib

[config:Debug|Win32]
libs = oldnames.lib
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    // For StaticLibrary type, config-level libs routes to lib.additional_dependencies
    CHECK(contains(cfg.lib.additional_dependencies, "oldnames.lib"));
}

TEST_CASE("Parse librarian additional_options", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib

[config:Debug|Win32]
libflags = /NODEFAULTLIB
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.lib.additional_options.find("/NODEFAULTLIB") != std::string::npos);
}

// ============================================================================
// Resource compiler settings
// ============================================================================

TEST_CASE("Parse resource compiler culture", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
rc_culture = 1033
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.resource_compile.culture == "1033");
}

TEST_CASE("Parse resource compiler defines", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
rc_defines = MY_RC_DEF
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains(cfg.resource_compile.preprocessor_definitions, "MY_RC_DEF"));
}

TEST_CASE("Parse resource compiler includes", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
rc_includes = res
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains_substring(cfg.resource_compile.additional_include_directories, "res"));
}

// ============================================================================
// Build event details
// ============================================================================

TEST_CASE("Parse prebuild event message", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
prebuild = echo hello
prebuild_message = Building...
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.pre_build_event.message == "Building...");
}

TEST_CASE("Parse postbuild use_in_build", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
postbuild = copy output
postbuild_use_in_build = false
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.post_build_event.use_in_build == false);
}

// ============================================================================
// Miscellaneous project settings
// ============================================================================

TEST_CASE("Parse forced_includes", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
forced_includes = force.h
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains(cfg.cl_compile.forced_include_files, "force.h"));
}

TEST_CASE("Parse disable_warnings", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
disable_warnings = 4996, 4267
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains(cfg.cl_compile.disable_specific_warnings, "4996"));
    CHECK(contains(cfg.cl_compile.disable_specific_warnings, "4267"));
}

TEST_CASE("Parse ignore_libs", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
ignore_libs = LIBCMT
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(contains(cfg.link.ignore_specific_default_libraries, "LIBCMT"));
}

TEST_CASE("Parse ldflags", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe

[config:Debug|Win32]
ldflags = /LARGEADDRESSAWARE
)");
    auto& cfg = sol.projects[0].configurations["Debug|Win32"];
    CHECK(cfg.link.additional_options.find("/LARGEADDRESSAWARE") != std::string::npos);
}

TEST_CASE("Parse whole_program_optimization", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Win32

[project:App]
type = exe

[config:Release|Win32]
whole_program_optimization = true
)");
    auto& cfg = sol.projects[0].configurations["Release|Win32"];
    CHECK(cfg.whole_program_optimization == true);
}

// ============================================================================
// Structural features
// ============================================================================

TEST_CASE("Parse file_properties block", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = file1.cpp, file2.cpp, file3.cpp
file_properties(file1.cpp, file2.cpp) {
    optimization = Disabled
}
)");
    int disabled_count = 0;
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        std::string filename = p.filename().string();
        if (filename == "file1.cpp" || filename == "file2.cpp") {
            if (src.settings.optimization.count(ALL_CONFIGS) > 0 &&
                src.settings.optimization.at(ALL_CONFIGS) == "Disabled") {
                disabled_count++;
            }
        }
    }
    CHECK(disabled_count == 2);
}

TEST_CASE("Parse set_file_properties block", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(
        "[solution]\n"
        "name = Test\n"
        "configurations = Debug\n"
        "platforms = Win32\n"
        "\n"
        "[project:App]\n"
        "type = exe\n"
        "sources = special.cpp, main.cpp\n"
        "set_file_properties(special.cpp,\n"
        "    pch = NotUsing\n"
        ")\n"
    );
    for (const auto& src : sol.projects[0].sources) {
        std::filesystem::path p(src.path);
        if (p.filename().string() == "special.cpp") {
            bool found_not_using = false;
            for (const auto& [key, pch_setting] : src.settings.pch) {
                if (pch_setting.mode == "NotUsing") found_not_using = true;
            }
            CHECK(found_not_using);
        }
    }
}

TEST_CASE("Parse template inheritance", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
platforms = Win32

[project:App]
type = exe

[config:Release|Win32]
optimization = MaxSpeed
runtime_library = MultiThreaded

[config:Profile|Win32 : Template:Release]
warning_level = Level4
)");
    // Profile should inherit optimization and runtime_library from Release
    auto& profile_cfg = sol.projects[0].configurations["Profile|Win32"];
    CHECK(profile_cfg.cl_compile.optimization == "MaxSpeed");
    CHECK(profile_cfg.cl_compile.runtime_library == "MultiThreaded");
    // Profile-specific setting should also be set
    CHECK(profile_cfg.cl_compile.warning_level == "Level4");
}

TEST_CASE("Parse solution-level per-config defines", "[buildscript_parser]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32
defines[Debug|Win32] = _DEBUG_SOL

[project:App]
type = exe
)");
    auto& debug_defs = sol.projects[0].configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    CHECK(contains(debug_defs, "_DEBUG_SOL"));
}
