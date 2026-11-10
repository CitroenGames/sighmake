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
