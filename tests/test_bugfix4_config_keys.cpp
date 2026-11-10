#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/vcxproj_reader.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {
bool bugfix4_contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
}  // namespace

TEST_CASE("disable_warnings and large_address_aware are accepted in config sections", "[bugfix4_config_keys]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
disable_warnings = 4100
large_address_aware = true

[config:Release|x64]
disable_warnings = 4100, 4996
large_address_aware = false
)");
    REQUIRE(sol.projects.size() == 1);
    const auto& cfgs = sol.projects[0].configurations;
    const auto& debug = cfgs.at("Debug|x64");
    const auto& release = cfgs.at("Release|x64");
    CHECK(debug.cl_compile.disable_specific_warnings == std::vector<std::string>{"4100"});
    CHECK(bugfix4_contains(release.cl_compile.disable_specific_warnings, "4996"));
    CHECK(release.cl_compile.disable_specific_warnings.size() == 2);
    CHECK(debug.link.large_address_aware);
    CHECK_FALSE(release.link.large_address_aware);
}

TEST_CASE("buildscript writer keeps per-config disable_warnings and large_address_aware", "[bugfix4_config_keys]") {
    const fs::path dir = fs::temp_directory_path() / "sighmake_bugfix4_config_keys_writer";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    Project project;
    project.name = "App";
    auto& debug = project.configurations["Debug|x64"];
    auto& release = project.configurations["Release|x64"];
    debug.config_type = release.config_type = "Application";
    debug.cl_compile.disable_specific_warnings = {"4100"};
    release.cl_compile.disable_specific_warnings = {"4100", "4996"};
    debug.link.large_address_aware = false;
    release.link.large_address_aware = true;
    SourceFile src;
    src.path = "main.cpp";
    src.type = FileType::ClCompile;
    project.sources.push_back(src);

    const fs::path script = dir / "App.buildscript";
    BuildscriptWriter writer;
    REQUIRE(writer.write_buildscript(project, script.string(), {"Debug", "Release"}, {"x64"}));
    BuildscriptParser parser;
    const Solution sol = parser.parse(script.string());
    REQUIRE(sol.projects.size() == 1);
    const auto& cfgs = sol.projects[0].configurations;
    CHECK(cfgs.at("Debug|x64").cl_compile.disable_specific_warnings == std::vector<std::string>{"4100"});
    CHECK(cfgs.at("Release|x64").cl_compile.disable_specific_warnings.size() == 2);
    CHECK_FALSE(cfgs.at("Debug|x64").link.large_address_aware);
    CHECK(cfgs.at("Release|x64").link.large_address_aware);
    fs::remove_all(dir, ec);
}
