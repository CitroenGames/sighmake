#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/cmake_parser.hpp"
#include "generators/cmake_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct CMakeResult {
    fs::path temp_dir;
    std::string project_content;

    ~CMakeResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

// Writes `files` (relative path -> content) into a fresh temp dir, parses the buildscript there and
// runs the CMake generator into the same directory.
CMakeResult generate_cmake(const std::string& buildscript,
                           const std::vector<std::pair<std::string, std::string>>& files,
                           const std::string& project_name = "App") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_bugfix_cmake_gen";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    for (const auto& [rel, content] : files) {
        fs::path p = result.temp_dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    BuildscriptParser parser;
    Solution solution = parser.parse_string(buildscript, result.temp_dir.string());
    CMakeGenerator generator;
    generator.generate(solution, result.temp_dir.string());

    result.project_content = read_file(result.temp_dir / project_name / "CMakeLists.txt");
    return result;
}

const Project* find_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

} // namespace

// ============================================================================
// CMake reader: generator expressions
// ============================================================================

static bool config_has_define(const Project* p, const std::string& key, const std::string& def) {
    return contains(p->configurations.at(key).cl_compile.preprocessor_definitions, def);
}

TEST_CASE("bugfix_cmake: conditional generator expression applies its value to the matching config",
          "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE $<$<CONFIG:Debug>:DEBUG_ONLY>)
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(config_has_define(p, "Debug|x64", "DEBUG_ONLY"));
    CHECK_FALSE(config_has_define(p, "Release|x64", "DEBUG_ONLY"));
}

TEST_CASE("bugfix_cmake: $<AND>/$<OR> split nested generator expressions into operands", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_compile_options(App PRIVATE
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<CONFIG:Release>>>:-fand>
    $<$<OR:$<CONFIG:Nope>,$<CONFIG:Debug>>:-for>)
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    const auto& dbg = p->configurations.at("Debug|x64").cl_compile.additional_options;
    const auto& rel = p->configurations.at("Release|x64").cl_compile.additional_options;
    CHECK(dbg.find("-fand") != std::string::npos);
    CHECK(dbg.find("-for") != std::string::npos);
    CHECK(rel.find("-fand") == std::string::npos);
    CHECK(rel.find("-for") == std::string::npos);
}

TEST_CASE("bugfix_cmake: $<CONFIG:...> is case-insensitive and accepts a list", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE $<$<CONFIG:debug>:LOWER> $<$<CONFIG:Foo,Release>:LISTED>)
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(config_has_define(p, "Debug|x64", "LOWER"));
    CHECK_FALSE(config_has_define(p, "Release|x64", "LOWER"));
    CHECK(config_has_define(p, "Release|x64", "LISTED"));
    CHECK_FALSE(config_has_define(p, "Debug|x64", "LISTED"));
}

// ============================================================================
// CMake reader: tokenizer
// ============================================================================

TEST_CASE("bugfix_cmake: bracket comments are skipped entirely", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
#[[ disabled:
add_executable(Ghost ghost.cpp)
]]
#[==[ also disabled ]] still comment
add_library(Ghost2 ghost2.cpp)
]==]
add_executable(App main.cpp)
)");
    CHECK(find_project(sol, "Ghost") == nullptr);
    CHECK(find_project(sol, "Ghost2") == nullptr);
    CHECK(find_project(sol, "App") != nullptr);
}

TEST_CASE("bugfix_cmake: bracket arguments are read verbatim as one argument", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE [[MSG=a (b) "c"]] [=[X=]]]=])
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(contains(p->project_level_preprocessor_definitions, "MSG=a (b) \"c\""));
    CHECK(contains(p->project_level_preprocessor_definitions, "X=]]"));
}

TEST_CASE("bugfix_cmake: escape sequences in quoted arguments", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
set(DIR "C:\\sdk\\")
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE "QUOTED=\"x\"" "D=${DIR}")
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(contains(p->project_level_preprocessor_definitions, "QUOTED=\"x\""));
    CHECK(contains(p->project_level_preprocessor_definitions, "D=C:\\sdk\\"));
}

// ============================================================================
// CMake reader: variable references and lists
// ============================================================================

TEST_CASE("bugfix_cmake: an unquoted list variable expands to separate arguments", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
set(LIBS m dl)
set(DEFS ONE TWO)
target_link_libraries(App PRIVATE ${LIBS})
target_compile_definitions(App PRIVATE ${DEFS} "QUOTED;LIST")
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    const auto& libs = p->configurations.at("Debug|x64").link.additional_dependencies;
    CHECK(contains(libs, "m.lib"));
    CHECK(contains(libs, "dl.lib"));
    CHECK_FALSE(contains(libs, "m;dl.lib"));
    CHECK(contains(p->project_level_preprocessor_definitions, "ONE"));
    CHECK(contains(p->project_level_preprocessor_definitions, "TWO"));
    // A quoted argument stays one argument
    CHECK(contains(p->project_level_preprocessor_definitions, "QUOTED;LIST"));
}

TEST_CASE("bugfix_cmake: nested variable references are evaluated inside-out", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
set(NAME Lib)
set(Lib_ROOT /opt/lib)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE "ROOT=${${NAME}_ROOT}" "PAIR=${NAME}-${Lib_ROOT}")
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(contains(p->project_level_preprocessor_definitions, "ROOT=/opt/lib"));
    CHECK(contains(p->project_level_preprocessor_definitions, "PAIR=Lib-/opt/lib"));
}

TEST_CASE("bugfix_cmake: a self-referencing value inside a nested reference terminates", "[bugfix_cmake]") {
    CMakeParser parser;
    REQUIRE_NOTHROW(parser.parse_string(R"(
set(b [[${b}]])
project(${a${b}})
)"));
}

// ============================================================================
// CMake reader: if() conditions
// ============================================================================

TEST_CASE("bugfix_cmake: parenthesized if() conditions", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
set(V 10)
if((V LESS 5) OR (V EQUAL 10))
    target_compile_definitions(App PRIVATE OR_GROUPS)
endif()
if(NOT (V EQUAL 10 OR V EQUAL 11) AND V GREATER 1)
    target_compile_definitions(App PRIVATE NOT_GROUP_WRONG)
endif()
if(V GREATER 1 AND (V LESS 3 OR (V GREATER 9 AND V LESS 11)))
    target_compile_definitions(App PRIVATE NESTED)
endif()
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    const auto& defs = p->project_level_preprocessor_definitions;
    CHECK(contains(defs, "OR_GROUPS"));
    CHECK_FALSE(contains(defs, "NOT_GROUP_WRONG"));
    CHECK(contains(defs, "NESTED"));
}

TEST_CASE("bugfix_cmake: if() comparison operators with _EQUAL variants", "[bugfix_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
set(V 10)
set(VER 3.20.1)
if(V LESS_EQUAL 5)
    target_compile_definitions(App PRIVATE LESS_EQUAL_WRONG)
endif()
if(V GREATER_EQUAL 10)
    target_compile_definitions(App PRIVATE GREATER_EQUAL_OK)
endif()
if(VER VERSION_GREATER_EQUAL 4.0)
    target_compile_definitions(App PRIVATE VERSION_GE_WRONG)
endif()
if(VER VERSION_LESS_EQUAL 3.20.1)
    target_compile_definitions(App PRIVATE VERSION_LE_OK)
endif()
if(abc STRLESS abd)
    target_compile_definitions(App PRIVATE STRLESS_OK)
endif()
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    const auto& defs = p->project_level_preprocessor_definitions;
    CHECK_FALSE(contains(defs, "LESS_EQUAL_WRONG"));
    CHECK(contains(defs, "GREATER_EQUAL_OK"));
    CHECK_FALSE(contains(defs, "VERSION_GE_WRONG"));
    CHECK(contains(defs, "VERSION_LE_OK"));
    CHECK(contains(defs, "STRLESS_OK"));
}

// ============================================================================
// CMake generator: quoting
// ============================================================================

TEST_CASE("bugfix_cmake: paths containing spaces are quoted", "[bugfix_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = src dir/main.cpp
includes = inc dir
libdirs = lib dir
src dir/main.cpp:defines = FILEDEF

[config:Debug|x64]
includes = dbg inc
)", {{"src dir/main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("\n    \"../src dir/main.cpp\"\n") != std::string::npos);
    CHECK(c.find("\n        \"../inc dir\"\n") != std::string::npos);
    CHECK(c.find("\n        \"../lib dir\"\n") != std::string::npos);
    CHECK(c.find("\"$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/../dbg inc>\"") != std::string::npos);
    CHECK(c.find("set_source_files_properties(\"../src dir/main.cpp\" PROPERTIES") != std::string::npos);
}

TEST_CASE("bugfix_cmake: define values with spaces or quotes stay one definition", "[bugfix_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
defines = MSG=hello world
main.cpp:defines = NAME="x y"

[config:Release|x64]
defines = REL=a b
)", {{"main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("\n        \"MSG=hello world\"\n") != std::string::npos);
    CHECK(c.find("\"$<$<CONFIG:Release>:REL=a b>\"") != std::string::npos);
    CHECK(c.find("COMPILE_DEFINITIONS \"NAME=\\\"x y\\\"\"") != std::string::npos);
}

TEST_CASE("bugfix_cmake: per-configuration include and library directories are emitted with absolute paths",
          "[bugfix_cmake]") {
    // CMake rejects relative paths that come out of a generator expression in INCLUDE_DIRECTORIES
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp

[config:Debug|x64]
includes = dbginc
libdirs = dbglib
)", {{"main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("target_include_directories(App PRIVATE\n    $<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/../dbginc>")
          != std::string::npos);
    CHECK(c.find("target_link_directories(App PRIVATE\n    $<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/../dbglib>")
          != std::string::npos);
}
