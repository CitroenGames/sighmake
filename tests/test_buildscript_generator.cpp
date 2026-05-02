#include "pch.h"
#include "catch_amalgamated.hpp"
#include "generators/buildscript_generator.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/cmake_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

struct BuildscriptGeneratorResult {
    fs::path temp_dir;
    fs::path output_dir;

    ~BuildscriptGeneratorResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

TEST_CASE("BuildscriptGenerator writes buildscripts from parsed CMake", "[buildscript_generator]") {
    BuildscriptGeneratorResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_buildscript_generator";
    result.output_dir = result.temp_dir / "out";

    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    CMakeParser parser;
    auto solution = parser.parse_string(R"(
project(Demo)
add_library(Core STATIC core.cpp)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE Core)
)", result.temp_dir.string());

    BuildscriptGenerator generator;
    REQUIRE(generator.generate(solution, result.output_dir.string()));

    CHECK(fs::exists(result.output_dir / "Demo.buildscript"));
    CHECK(fs::exists(result.output_dir / "Core.buildscript"));
    CHECK(fs::exists(result.output_dir / "App.buildscript"));

    BuildscriptParser round_trip;
    auto reparsed = round_trip.parse((result.output_dir / "Demo.buildscript").string());
    CHECK(reparsed.name == "Demo");
    CHECK(reparsed.projects.size() == 2);
}

TEST_CASE("BuildscriptGenerator preserves merged root project includes", "[buildscript_generator]") {
    BuildscriptGeneratorResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_buildscript_generator_merged";
    result.output_dir = result.temp_dir / "out";

    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    CMakeParser parser;
    auto solution = parser.parse_string(R"(
project(Demo)
add_executable(Demo main.cpp)
add_library(Core STATIC core.cpp)
target_link_libraries(Demo PRIVATE Core)
)", result.temp_dir.string());

    BuildscriptGenerator generator;
    REQUIRE(generator.generate(solution, result.output_dir.string()));

    auto root_path = result.output_dir / "Demo.buildscript";
    REQUIRE(fs::exists(root_path));

    std::ifstream root(root_path);
    std::string content((std::istreambuf_iterator<char>(root)),
                        std::istreambuf_iterator<char>());

    CHECK(content.find("[project:Demo]") != std::string::npos);
    CHECK(content.find("include = Core.buildscript") != std::string::npos);
}
