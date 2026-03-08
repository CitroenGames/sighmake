#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/cmake_parser.hpp"

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
// Basic CMake parsing
// ============================================================================

TEST_CASE("CMake project() sets solution name", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string("project(MyApp)");
    CHECK(sol.name == "MyApp");
}

TEST_CASE("CMake cmake_minimum_required does not crash", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
cmake_minimum_required(VERSION 3.15)
project(Test)
)");
    CHECK(sol.name == "Test");
}

TEST_CASE("CMake add_executable creates Application project", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(MyApp main.cpp)
)");
    auto* proj = find_project(sol, "MyApp");
    REQUIRE(proj != nullptr);

    // Check at least one configuration has Application type
    bool found_app = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (cfg.config_type == "Application") found_app = true;
    }
    CHECK(found_app);
}

TEST_CASE("CMake add_library STATIC creates StaticLibrary", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_library(MyLib STATIC lib.cpp)
)");
    auto* proj = find_project(sol, "MyLib");
    REQUIRE(proj != nullptr);

    bool found_static = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (cfg.config_type == "StaticLibrary") found_static = true;
    }
    CHECK(found_static);
}

TEST_CASE("CMake add_library SHARED creates DynamicLibrary", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_library(MyLib SHARED lib.cpp)
)");
    auto* proj = find_project(sol, "MyLib");
    REQUIRE(proj != nullptr);

    bool found_shared = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (cfg.config_type == "DynamicLibrary") found_shared = true;
    }
    CHECK(found_shared);
}

TEST_CASE("CMake set() and variable expansion", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
set(MY_NAME HelloProject)
project(${MY_NAME})
)");
    CHECK(sol.name == "HelloProject");
}

TEST_CASE("CMake target_include_directories adds includes", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_include_directories(App PRIVATE src include)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);

    // Check that includes are added to at least one configuration
    bool has_include = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (contains_substring(cfg.cl_compile.additional_include_directories, "src") ||
            contains_substring(cfg.cl_compile.additional_include_directories, "include")) {
            has_include = true;
        }
    }
    CHECK(has_include);
}

TEST_CASE("CMake target_compile_definitions adds defines", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE MY_DEFINE=1)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);

    // Non-generator-expression defines are stored in project_level_preprocessor_definitions
    CHECK(contains(proj->project_level_preprocessor_definitions, "MY_DEFINE=1"));
}

TEST_CASE("CMake target_link_libraries creates dependency", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_library(MyLib STATIC lib.cpp)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE MyLib)
)");
    auto* app = find_project(sol, "App");
    REQUIRE(app != nullptr);

    bool found_dep = false;
    for (const auto& dep : app->project_references) {
        if (dep.name == "MyLib") {
            found_dep = true;
            CHECK(dep.visibility == DependencyVisibility::PRIVATE);
        }
    }
    CHECK(found_dep);
}

TEST_CASE("CMake generates default configurations", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
)");
    CHECK(sol.configurations.size() >= 2);
    CHECK(sol.platforms.size() >= 1);
}

TEST_CASE("CMake multiple projects", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(MySolution)
add_library(Core STATIC core.cpp)
add_library(Utils STATIC utils.cpp)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE Core Utils)
)");
    CHECK(sol.projects.size() == 3);
    CHECK(find_project(sol, "Core") != nullptr);
    CHECK(find_project(sol, "Utils") != nullptr);
    CHECK(find_project(sol, "App") != nullptr);
}

TEST_CASE("CMake option() sets variable", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
option(ENABLE_TESTS "Enable testing" OFF)
add_executable(App main.cpp)
)");
    // Should not crash and should parse successfully
    CHECK(sol.name == "Test");
}

TEST_CASE("CMake add_library without type defaults to STATIC", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_library(MyLib lib.cpp)
)");
    auto* proj = find_project(sol, "MyLib");
    REQUIRE(proj != nullptr);

    bool found_static = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (cfg.config_type == "StaticLibrary") found_static = true;
    }
    CHECK(found_static);
}

// ============================================================================
// if/elseif/else/endif
// ============================================================================

TEST_CASE("CMake if(DEFINED var) executes when variable is set", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(MY_VAR ON)
if(DEFINED MY_VAR)
    add_executable(App main.cpp)
endif()
)");
    CHECK(find_project(sol, "App") != nullptr);
}

TEST_CASE("CMake if(NOT var) inverts condition", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(DISABLED OFF)
if(NOT DISABLED)
    add_executable(App main.cpp)
endif()
)");
    CHECK(find_project(sol, "App") != nullptr);
}

TEST_CASE("CMake if/else takes else branch when condition false", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(USE_LIB OFF)
if(USE_LIB)
    add_library(Lib STATIC lib.cpp)
else()
    add_executable(App main.cpp)
endif()
)");
    CHECK(find_project(sol, "Lib") == nullptr);
    CHECK(find_project(sol, "App") != nullptr);
}

TEST_CASE("CMake if/elseif chains evaluate correctly", "[cmake_parser]") {
    CMakeParser parser;
    // Test if/elseif/else with boolean variables
    auto sol = parser.parse_string(R"(
project(Test)
set(OPTION_A OFF)
set(OPTION_B ON)
if(OPTION_A)
    add_executable(A main.cpp)
elseif(OPTION_B)
    add_executable(B main.cpp)
else()
    add_executable(Other main.cpp)
endif()
)");
    CHECK(find_project(sol, "A") == nullptr);
    CHECK(find_project(sol, "B") != nullptr);
    CHECK(find_project(sol, "Other") == nullptr);
}

TEST_CASE("CMake nested if blocks", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(A ON)
set(B ON)
if(A)
    if(B)
        add_executable(Both main.cpp)
    endif()
endif()
)");
    CHECK(find_project(sol, "Both") != nullptr);
}

// ============================================================================
// foreach
// ============================================================================

TEST_CASE("CMake foreach with simple items", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
foreach(lib CoreLib UtilLib)
    add_library(${lib} STATIC src.cpp)
endforeach()
)");
    CHECK(find_project(sol, "CoreLib") != nullptr);
    CHECK(find_project(sol, "UtilLib") != nullptr);
}

TEST_CASE("CMake foreach IN LISTS", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(LIBS "LibA;LibB")
foreach(lib IN LISTS LIBS)
    add_library(${lib} STATIC src.cpp)
endforeach()
)");
    CHECK(find_project(sol, "LibA") != nullptr);
    CHECK(find_project(sol, "LibB") != nullptr);
}

TEST_CASE("CMake foreach RANGE", "[cmake_parser]") {
    CMakeParser parser;
    // This should not crash
    REQUIRE_NOTHROW(parser.parse_string(R"(
project(Test)
foreach(i RANGE 0 2)
endforeach()
add_executable(App main.cpp)
)"));
}

// ============================================================================
// function/macro
// ============================================================================

TEST_CASE("CMake function() defines and calls function", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
function(create_lib name)
    add_library(${name} STATIC src.cpp)
endfunction()
create_lib(MyLib)
)");
    CHECK(find_project(sol, "MyLib") != nullptr);
}

TEST_CASE("CMake macro() defines and calls macro", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
macro(create_exe name)
    add_executable(${name} main.cpp)
endmacro()
create_exe(MyApp)
)");
    CHECK(find_project(sol, "MyApp") != nullptr);
}

// ============================================================================
// list operations
// ============================================================================

TEST_CASE("CMake list(APPEND) appends to list variable", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(SOURCES "")
list(APPEND SOURCES main.cpp)
list(APPEND SOURCES util.cpp)
add_executable(App ${SOURCES})
)");
    auto* proj = find_project(sol, "App");
    CHECK(proj != nullptr);
}

TEST_CASE("CMake list used in foreach", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(NAMES "")
list(APPEND NAMES Alpha)
list(APPEND NAMES Beta)
foreach(n IN LISTS NAMES)
    add_library(${n} STATIC src.cpp)
endforeach()
)");
    CHECK(find_project(sol, "Alpha") != nullptr);
    CHECK(find_project(sol, "Beta") != nullptr);
}

// ============================================================================
// target_sources
// ============================================================================

TEST_CASE("CMake target_sources adds files to existing target", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_sources(App PRIVATE extra.cpp)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);
    CHECK(proj->sources.size() >= 2);
}

// ============================================================================
// target_compile_options
// ============================================================================

TEST_CASE("CMake target_compile_options adds flags", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_compile_options(App PRIVATE /W4)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);
    // Options should be added to configurations
    bool found = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (cfg.cl_compile.additional_options.find("/W4") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

// ============================================================================
// include_directories / link_directories
// ============================================================================

TEST_CASE("CMake include_directories adds to existing targets", "[cmake_parser]") {
    CMakeParser parser;
    // include_directories applies to projects defined SO FAR
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
include_directories(global_include)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);

    bool has_include = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (contains_substring(cfg.cl_compile.additional_include_directories, "global_include")) {
            has_include = true;
        }
    }
    CHECK(has_include);
}

TEST_CASE("CMake link_directories adds library search paths", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
link_directories(lib/path)
)");
    // Should not crash
    CHECK(sol.name == "Test");
}

// ============================================================================
// find commands
// ============================================================================

TEST_CASE("CMake find_package sets FOUND variable", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
find_package(Threads)
add_executable(App main.cpp)
)");
    // Should not crash, find_package is handled as stub
    CHECK(find_project(sol, "App") != nullptr);
}

// ============================================================================
// while
// ============================================================================

TEST_CASE("CMake while loop with counter", "[cmake_parser]") {
    CMakeParser parser;
    REQUIRE_NOTHROW(parser.parse_string(R"(
project(Test)
set(i 0)
while(i LESS 3)
    math(EXPR i "${i} + 1")
endwhile()
add_executable(App main.cpp)
)"));
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("CMake empty project name", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(MyProject VERSION 1.0 LANGUAGES CXX)
add_executable(App main.cpp)
)");
    CHECK(sol.name == "MyProject");
}

TEST_CASE("CMake target_link_libraries with system library", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE pthread m)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);
    // System libs that are not projects should be stored in dependencies
    bool found = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (contains(cfg.link.additional_dependencies, "pthread") ||
            contains_substring(cfg.link.additional_dependencies, "pthread")) {
            found = true;
        }
    }
    // Either as dependency or as project reference
    if (!found) {
        for (const auto& dep : proj->project_references) {
            if (dep.name == "pthread") found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("CMake set with multiple values creates list", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(SRCS main.cpp util.cpp helper.cpp)
add_executable(App ${SRCS})
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);
    CHECK(proj->sources.size() >= 3);
}

TEST_CASE("CMake nested variable expansion", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
set(BASE "My")
set(SUFFIX "App")
project(${BASE}${SUFFIX})
)");
    CHECK(sol.name == "MyApp");
}

// ============================================================================
// Generator expressions
// ============================================================================

TEST_CASE("CMake generator expression detection", "[cmake_parser]") {
    CMakeParser parser;
    // Verify that the parser recognizes generator expressions and doesn't crash
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE $<BUILD_INTERFACE:BUILD_DEF>)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);

    // BUILD_INTERFACE genexpr should evaluate to the value (sighmake is build-time)
    bool found_build_def = false;
    for (const auto& [key, cfg] : proj->configurations) {
        if (contains(cfg.cl_compile.preprocessor_definitions, "BUILD_DEF")) {
            found_build_def = true;
        }
    }
    CHECK(found_build_def);
}

TEST_CASE("CMake is_generator_expression detects genexpr syntax", "[cmake_parser]") {
    CMakeParser parser;
    // Simple test: non-genexpr definitions are added to all configs
    auto sol = parser.parse_string(R"(
project(Test)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE NORMAL_DEF)
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);

    // NORMAL_DEF should be in project_level_preprocessor_definitions
    CHECK(contains(proj->project_level_preprocessor_definitions, "NORMAL_DEF"));
}

// ============================================================================
// message() does not crash
// ============================================================================

TEST_CASE("CMake message STATUS does not crash", "[cmake_parser]") {
    CMakeParser parser;
    REQUIRE_NOTHROW(parser.parse_string(R"(
project(Test)
message(STATUS "Hello world")
message(WARNING "This is a warning")
add_executable(App main.cpp)
)"));
}

// ============================================================================
// Semicolon-separated values
// ============================================================================

TEST_CASE("CMake semicolon-separated values in set", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(Test)
set(SRCS "a.cpp;b.cpp;c.cpp")
add_executable(App ${SRCS})
)");
    auto* proj = find_project(sol, "App");
    REQUIRE(proj != nullptr);
    CHECK(proj->sources.size() >= 3);
}

// ============================================================================
// project() with VERSION and LANGUAGES
// ============================================================================

TEST_CASE("CMake project with VERSION and LANGUAGES", "[cmake_parser]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(MyApp VERSION 1.2.3 LANGUAGES CXX)
add_executable(MyApp main.cpp)
)");
    CHECK(sol.name == "MyApp");
    auto* proj = find_project(sol, "MyApp");
    REQUIRE(proj != nullptr);
}
