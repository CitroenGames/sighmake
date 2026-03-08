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

// Helper to check if a vector contains a substring
static bool contains_substring(const std::vector<std::string>& vec, const std::string& sub) {
    for (const auto& s : vec) {
        if (s.find(sub) != std::string::npos) return true;
    }
    return false;
}

// Helper to check if a vector contains an exact string
static bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

// ============================================================================
// PUBLIC dependency propagation
// ============================================================================

TEST_CASE("PUBLIC dependency propagates public_includes", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:CoreLib]
type = lib
public_includes = core_include

[project:App]
type = exe
target_link_libraries(PUBLIC CoreLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    // App's configurations should have CoreLib's public_includes propagated
    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());
    CHECK(contains_substring(it->second.cl_compile.additional_include_directories, "core_include"));
}

TEST_CASE("PUBLIC dependency propagates public_defines", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:CoreLib]
type = lib
public_defines = USE_CORE

[project:App]
type = exe
target_link_libraries(PUBLIC CoreLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "USE_CORE"));
}

TEST_CASE("PUBLIC dependency propagates public_libs", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:CoreLib]
type = lib
public_libs = kernel32.lib

[project:App]
type = exe
target_link_libraries(PUBLIC CoreLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());
    CHECK(contains(it->second.link.additional_dependencies, "kernel32.lib"));
}

// ============================================================================
// PRIVATE dependency does NOT propagate transitively
// ============================================================================

TEST_CASE("PRIVATE dependency does not propagate transitively", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:InternalLib]
type = lib
public_includes = internal_include
public_defines = INTERNAL_DEF

[project:MiddleLib]
type = lib
target_link_libraries(PRIVATE InternalLib)

[project:App]
type = exe
target_link_libraries(PUBLIC MiddleLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());

    // App should NOT have InternalLib's includes/defines (PRIVATE boundary)
    CHECK_FALSE(contains_substring(it->second.cl_compile.additional_include_directories, "internal_include"));
    CHECK_FALSE(contains(it->second.cl_compile.preprocessor_definitions, "INTERNAL_DEF"));
}

// ============================================================================
// INTERFACE dependency
// ============================================================================

TEST_CASE("INTERFACE dependency propagates to dependents", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:HeaderLib]
type = interface
public_includes = header_include
public_defines = HEADER_DEF

[project:App]
type = exe
target_link_libraries(PUBLIC HeaderLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());
    CHECK(contains_substring(it->second.cl_compile.additional_include_directories, "header_include"));
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "HEADER_DEF"));
}

// ============================================================================
// Transitive PUBLIC chain
// ============================================================================

TEST_CASE("Transitive PUBLIC chain propagates through multiple levels", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:BaseLib]
type = lib
public_includes = base_include
public_defines = BASE_DEF

[project:MiddleLib]
type = lib
target_link_libraries(PUBLIC BaseLib)
public_includes = middle_include

[project:App]
type = exe
target_link_libraries(PUBLIC MiddleLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());

    // App should have both MiddleLib and BaseLib public properties
    CHECK(contains_substring(it->second.cl_compile.additional_include_directories, "middle_include"));
    CHECK(contains_substring(it->second.cl_compile.additional_include_directories, "base_include"));
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "BASE_DEF"));
}

// ============================================================================
// Mixed visibility
// ============================================================================

TEST_CASE("Mixed visibility: PUBLIC then PRIVATE stops propagation", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:DeepLib]
type = lib
public_defines = DEEP_DEF

[project:MiddleLib]
type = lib
target_link_libraries(PRIVATE DeepLib)
public_defines = MIDDLE_DEF

[project:App]
type = exe
target_link_libraries(PUBLIC MiddleLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());

    // App gets MiddleLib's public defines (through PUBLIC)
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "MIDDLE_DEF"));
    // App does NOT get DeepLib's public defines (blocked by PRIVATE in MiddleLib)
    CHECK_FALSE(contains(it->second.cl_compile.preprocessor_definitions, "DEEP_DEF"));
}

// ============================================================================
// Multiple dependencies
// ============================================================================

TEST_CASE("Multiple PUBLIC dependencies all propagate", "[dependency_propagation]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib
public_defines = DEF_A

[project:LibB]
type = lib
public_defines = DEF_B

[project:App]
type = exe
target_link_libraries(
    PUBLIC
        LibA
        LibB
)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    auto it = app->configurations.find("Debug|Win32");
    REQUIRE(it != app->configurations.end());
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "DEF_A"));
    CHECK(contains(it->second.cl_compile.preprocessor_definitions, "DEF_B"));
}
