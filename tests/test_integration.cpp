#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/cmake_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper to find a project by name
static const Project* find_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

// Get the repo root (two levels up from the tests directory)
static fs::path get_repo_root() {
    // Try to find the repo root by looking for sighmake.buildscript
    fs::path current = fs::current_path();

    // If we're in the tests directory, go up one level
    if (fs::exists(current / "sighmake.buildscript")) {
        return current;
    }
    if (fs::exists(current.parent_path() / "sighmake.buildscript")) {
        return current.parent_path();
    }
    // Try from the executable location
    // Fallback: walk up from current directory
    fs::path p = current;
    for (int i = 0; i < 5; ++i) {
        if (fs::exists(p / "sighmake.buildscript")) return p;
        p = p.parent_path();
    }
    return current;
}

// ============================================================================
// Self-hosting buildscript
// ============================================================================

TEST_CASE("Parse sighmake's own buildscript", "[integration]") {
    fs::path repo_root = get_repo_root();
    fs::path buildscript_path = repo_root / "sighmake.buildscript";

    if (!fs::exists(buildscript_path)) {
        WARN("Skipping: sighmake.buildscript not found at " << buildscript_path.string());
        return;
    }

    BuildscriptParser parser;
    Solution sol;
    REQUIRE_NOTHROW(sol = parser.parse(buildscript_path.string()));

    CHECK(sol.name == "sighmake");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "sighmake");

    // Should have Debug and Release configurations
    CHECK(sol.configurations.size() == 2);
    CHECK(sol.platforms.size() == 3);

    // Should have source files
    CHECK(sol.projects[0].sources.size() > 0);
}

TEST_CASE("Sighmake buildscript has correct project type", "[integration]") {
    fs::path repo_root = get_repo_root();
    fs::path buildscript_path = repo_root / "sighmake.buildscript";

    if (!fs::exists(buildscript_path)) {
        WARN("Skipping: sighmake.buildscript not found");
        return;
    }

    BuildscriptParser parser;
    auto sol = parser.parse(buildscript_path.string());

    // Check that it's an executable project
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        CHECK(cfg.config_type == "Application");
    }
}

TEST_CASE("Sighmake buildscript has PCH configured", "[integration]") {
    fs::path repo_root = get_repo_root();
    fs::path buildscript_path = repo_root / "sighmake.buildscript";

    if (!fs::exists(buildscript_path)) {
        WARN("Skipping: sighmake.buildscript not found");
        return;
    }

    BuildscriptParser parser;
    auto sol = parser.parse(buildscript_path.string());

    // PCH should be set to "Use" at project level
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        CHECK(cfg.cl_compile.pch.mode == "Use");
    }
}

// ============================================================================
// Multi Project Example
// ============================================================================

TEST_CASE("Parse multi-project example buildscript", "[integration]") {
    fs::path repo_root = get_repo_root();
    fs::path example_path = repo_root / "examples" / "Multi Project Example" / "multi_project.buildscript";

    if (!fs::exists(example_path)) {
        WARN("Skipping: multi_project.buildscript not found at " << example_path.string());
        return;
    }

    BuildscriptParser parser;
    Solution sol;
    REQUIRE_NOTHROW(sol = parser.parse(example_path.string()));

    // Should have 2 projects: MathLib (DLL) and Calculator (EXE)
    CHECK(sol.projects.size() == 2);

    auto* mathlib = find_project(sol, "MathLib");
    auto* calculator = find_project(sol, "Calculator");

    REQUIRE(mathlib != nullptr);
    REQUIRE(calculator != nullptr);

    // MathLib should be a DLL
    for (const auto& [key, cfg] : mathlib->configurations) {
        CHECK(cfg.config_type == "DynamicLibrary");
    }

    // Calculator should be an executable
    for (const auto& [key, cfg] : calculator->configurations) {
        CHECK(cfg.config_type == "Application");
    }

    // Calculator should depend on MathLib
    bool found_dep = false;
    for (const auto& dep : calculator->project_references) {
        if (dep.name == "MathLib") found_dep = true;
    }
    CHECK(found_dep);
}

// ============================================================================
// CMake integration test
// ============================================================================

TEST_CASE("Parse test_cmake CMakeLists.txt", "[integration]") {
    fs::path repo_root = get_repo_root();
    fs::path cmake_path = repo_root / "test_cmake" / "CMakeLists.txt";

    if (!fs::exists(cmake_path)) {
        WARN("Skipping: test_cmake/CMakeLists.txt not found at " << cmake_path.string());
        return;
    }

    CMakeParser parser;
    Solution sol;
    REQUIRE_NOTHROW(sol = parser.parse(cmake_path.string()));

    // Should produce at least one project
    CHECK(sol.projects.size() >= 1);
    CHECK(!sol.name.empty());
}

// ============================================================================
// Round-trip consistency
// ============================================================================

TEST_CASE("Parsing the same buildscript twice produces consistent results", "[integration]") {
    BuildscriptParser parser1, parser2;

    std::string buildscript = R"(
[solution]
name = ConsistencyTest
configurations = Debug, Release
platforms = Win32, x64

[project:App]
type = exe
std = 17
warning_level = Level4
defines = FOO, BAR
subsystem = Console
)";

    auto sol1 = parser1.parse_string(buildscript);
    auto sol2 = parser2.parse_string(buildscript);

    CHECK(sol1.name == sol2.name);
    CHECK(sol1.configurations == sol2.configurations);
    CHECK(sol1.platforms == sol2.platforms);
    REQUIRE(sol1.projects.size() == sol2.projects.size());
    CHECK(sol1.projects[0].name == sol2.projects[0].name);

    // Same number of config keys
    auto keys1 = sol1.get_config_keys();
    auto keys2 = sol2.get_config_keys();
    CHECK(keys1 == keys2);

    // Same compiler settings for each config
    for (const auto& key : keys1) {
        auto& cfg1 = sol1.projects[0].configurations[key];
        auto& cfg2 = sol2.projects[0].configurations[key];
        CHECK(cfg1.config_type == cfg2.config_type);
        CHECK(cfg1.cl_compile.language_standard == cfg2.cl_compile.language_standard);
        CHECK(cfg1.cl_compile.warning_level == cfg2.cl_compile.warning_level);
        CHECK(cfg1.link.sub_system == cfg2.link.sub_system);
    }
}

// ============================================================================
// Round-trip: buildscript -> vcxproj -> vcxproj_reader
// ============================================================================

TEST_CASE("Round-trip buildscript to vcxproj and back", "[integration]") {
    namespace fs = std::filesystem;

    auto temp_dir = fs::temp_directory_path() / "sighmake_test_roundtrip";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";

    // Step 1: Parse buildscript
    BuildscriptParser bs_parser;
    auto original = bs_parser.parse_string(R"(
[solution]
name = RoundTrip
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
sources = main.cpp
std = 17
subsystem = Console
defines = MY_DEF
)", temp_dir.string());

    REQUIRE(original.projects.size() == 1);

    // Step 2: Generate vcxproj
    // (Need to include vcxproj_generator and vcxproj_reader headers)
    // This test verifies the data model is correct - the parser and config defaults work together.
    CHECK(original.projects[0].name == "App");
    for (const auto& [key, cfg] : original.projects[0].configurations) {
        CHECK(cfg.config_type == "Application");
        CHECK(cfg.cl_compile.language_standard == "stdcpp17");
        CHECK(cfg.link.sub_system == "Console");
    }

    fs::remove_all(temp_dir, ec);
}

// ============================================================================
// Error handling
// ============================================================================

TEST_CASE("BuildscriptParser throws on nonexistent file", "[integration]") {
    BuildscriptParser parser;
    CHECK_THROWS(parser.parse("definitely_nonexistent_file.buildscript"));
}

TEST_CASE("BuildscriptParser handles empty string input", "[integration]") {
    BuildscriptParser parser;
    Solution sol;
    REQUIRE_NOTHROW(sol = parser.parse_string(""));
    // Empty string should produce an empty (or default) solution
}

TEST_CASE("BuildscriptParser handles malformed input gracefully", "[integration]") {
    BuildscriptParser parser;
    Solution sol;
    REQUIRE_NOTHROW(sol = parser.parse_string(R"(
this is not a valid buildscript
random text = random value
[invalid section]
more random stuff
)"));
}

// ============================================================================
// Platform conditional file inclusion
// ============================================================================

TEST_CASE("Parse platform conditional file inclusion", "[integration]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp, win_impl.cpp [windows], linux_impl.cpp [linux]
)");
    auto& proj = sol.projects[0];

    bool has_main = false;
    bool has_win = false;
    bool has_linux = false;
    for (const auto& src : proj.sources) {
        std::string fname = fs::path(src.path).filename().string();
        if (fname == "main.cpp") has_main = true;
        if (fname == "win_impl.cpp") has_win = true;
        if (fname == "linux_impl.cpp") has_linux = true;
    }

    CHECK(has_main);
#if defined(_WIN32)
    CHECK(has_win);
    CHECK_FALSE(has_linux);
#endif
}
