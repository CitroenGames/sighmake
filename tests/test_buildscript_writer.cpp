#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/vcxproj_reader.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper to read file content
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// RAII temp dir for writer tests
struct WriterResult {
    fs::path temp_dir;
    fs::path buildscript_path;
    std::string content;

    ~WriterResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static WriterResult write_project(const Project& project) {
    WriterResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_writer";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    result.buildscript_path = result.temp_dir / (project.name + ".buildscript");

    BuildscriptWriter writer;
    writer.write_buildscript(project, result.buildscript_path.string());

    if (fs::exists(result.buildscript_path)) {
        result.content = read_file(result.buildscript_path);
    }

    return result;
}

// ============================================================================
// BuildscriptWriter tests
// ============================================================================

TEST_CASE("BuildscriptWriter writes project type exe", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";

    auto result = write_project(proj);
    CHECK(result.content.find("[project:App]") != std::string::npos);
    CHECK(result.content.find("type = exe") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes project type lib", "[buildscript_writer]") {
    Project proj;
    proj.name = "MyLib";
    proj.configurations["Debug|Win32"].config_type = "StaticLibrary";

    auto result = write_project(proj);
    CHECK(result.content.find("type = lib") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes project type dll", "[buildscript_writer]") {
    Project proj;
    proj.name = "MyDll";
    proj.configurations["Debug|Win32"].config_type = "DynamicLibrary";

    auto result = write_project(proj);
    CHECK(result.content.find("type = dll") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes sources", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";

    SourceFile src1;
    src1.path = "main.cpp";
    src1.type = FileType::ClCompile;
    proj.sources.push_back(src1);

    SourceFile src2;
    src2.path = "util.cpp";
    src2.type = FileType::ClCompile;
    proj.sources.push_back(src2);

    auto result = write_project(proj);
    CHECK(result.content.find("sources = ") != std::string::npos);
    CHECK(result.content.find("main.cpp") != std::string::npos);
    CHECK(result.content.find("util.cpp") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes headers", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";

    SourceFile h;
    h.path = "types.h";
    h.type = FileType::ClInclude;
    proj.sources.push_back(h);

    auto result = write_project(proj);
    CHECK(result.content.find("headers = ") != std::string::npos);
    CHECK(result.content.find("types.h") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes includes", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].cl_compile.additional_include_directories = {"src", "include"};

    auto result = write_project(proj);
    CHECK(result.content.find("includes = ") != std::string::npos);
    CHECK(result.content.find("src") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes defines", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].cl_compile.preprocessor_definitions = {"MY_DEF", "FOO"};

    auto result = write_project(proj);
    CHECK(result.content.find("defines = ") != std::string::npos);
    CHECK(result.content.find("MY_DEF") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes C++ standard", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].cl_compile.language_standard = "stdcpp17";

    auto result = write_project(proj);
    // Should strip stdcpp prefix: "std = 17"
    CHECK(result.content.find("std = 17") != std::string::npos);
}

TEST_CASE("BuildscriptWriter writes dependencies", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.project_references.push_back({"LibA", DependencyVisibility::PUBLIC});

    auto result = write_project(proj);
    CHECK(result.content.find("target_link_libraries(LibA)") != std::string::npos);
}

TEST_CASE("BuildscriptWriter round-trip with BuildscriptParser", "[buildscript_writer]") {
    // Create a project
    Project proj;
    proj.name = "RoundTrip";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].cl_compile.language_standard = "stdcpp17";
    proj.configurations["Debug|Win32"].cl_compile.warning_level = "Level4";
    proj.configurations["Debug|Win32"].link.sub_system = "Console";

    SourceFile src;
    src.path = "main.cpp";
    src.type = FileType::ClCompile;
    proj.sources.push_back(src);

    // Write it
    auto result = write_project(proj);
    CHECK(!result.content.empty());

    // Parse it back
    BuildscriptParser parser;
    auto sol = parser.parse_string(result.content, result.temp_dir.string());

    // Verify key properties survived the round-trip
    REQUIRE(sol.projects.size() >= 1);
    CHECK(sol.projects[0].name == "RoundTrip");
}
