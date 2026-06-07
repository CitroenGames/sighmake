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

TEST_CASE("BuildscriptWriter writes project type sys", "[buildscript_writer]") {
    Project proj;
    proj.name = "MyDriver";
    proj.configurations["Debug|Win32"].config_type = "Driver";

    auto result = write_project(proj);
    CHECK(result.content.find("type = sys") != std::string::npos);
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

TEST_CASE("BuildscriptWriter converts VS directory macros to buildscript-relative paths", "[buildscript_writer]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_writer_macros";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir / "App");

    Project proj;
    proj.name = "App";
    proj.vcxproj_path = "App/App.vcxproj";

    auto& cfg = proj.configurations["Debug|Win32"];
    cfg.config_type = "Application";
    cfg.out_dir = "$(SolutionDir)bin\\$(Configuration)\\";
    cfg.int_dir = "$(SolutionDir)obj\\$(Configuration)\\";
    cfg.cl_compile.additional_include_directories = {"$(SolutionDir)", "$(ProjectDir)include"};

    const auto buildscript_path = temp_dir / "App" / "App.buildscript";
    BuildscriptWriter writer;
    REQUIRE(writer.write_buildscript(proj, buildscript_path.string()));

    const auto content = read_file(buildscript_path);
    CHECK(content.find("includes = .., include") != std::string::npos);
    CHECK(content.find("outdir = ..\\bin\\$(Configuration)\\") != std::string::npos);
    CHECK(content.find("intdir = ..\\obj\\$(Configuration)\\") != std::string::npos);

    fs::remove_all(temp_dir, ec);
}

TEST_CASE("BuildscriptWriter resolves MSBuildProjectName using original vcxproj stem", "[buildscript_writer]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_writer_project_name_macro";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir / "App");

    Project proj;
    proj.name = "GeneratedApp";
    proj.vcxproj_path = "App/App.vcxproj";

    auto& cfg = proj.configurations["Debug|Win32"];
    cfg.config_type = "Application";
    cfg.out_dir = "$(SolutionDir)$(Configuration)\\$(MSBuildProjectName)\\";
    cfg.int_dir = "$(ProjectDir)obj\\$(MSBuildProjectName)\\";

    const auto buildscript_path = temp_dir / "App" / "GeneratedApp.buildscript";
    BuildscriptWriter writer;
    REQUIRE(writer.write_buildscript(proj, buildscript_path.string()));

    const auto content = read_file(buildscript_path);
    CHECK(content.find("outdir = ..\\$(Configuration)\\App\\") != std::string::npos);
    CHECK(content.find("intdir = obj\\App\\") != std::string::npos);

    fs::remove_all(temp_dir, ec);
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

TEST_CASE("BuildscriptWriter writes build-order-only dependencies", "[buildscript_writer]") {
    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.project_references.push_back(ProjectDependency("AssetDll", DependencyVisibility::PUBLIC, false, false));

    auto result = write_project(proj);
    CHECK(result.content.find("depends = AssetDll") != std::string::npos);
    CHECK(result.content.find("target_link_libraries(AssetDll)") == std::string::npos);

    BuildscriptParser parser;
    auto sol = parser.parse_string(result.content, result.temp_dir.string());
    REQUIRE(sol.projects.size() >= 1);
    REQUIRE(sol.projects[0].project_references.size() == 1);
    CHECK_FALSE(sol.projects[0].project_references[0].link_library_dependencies);
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

TEST_CASE("BuildscriptWriter preserves config-specific Unicode charset", "[buildscript_writer]") {
    Project proj;
    proj.name = "MixedCharset";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].character_set = "MultiByte";
    proj.configurations["Release|x64"].config_type = "Application";
    proj.configurations["Release|x64"].character_set = "Unicode";

    auto result = write_project(proj);
    CHECK(result.content.find("charset = MultiByte") != std::string::npos);
    CHECK(result.content.find("[config:Release|x64]") != std::string::npos);
    CHECK(result.content.find("charset = Unicode") != std::string::npos);

    BuildscriptParser parser;
    auto sol = parser.parse_string(result.content, result.temp_dir.string());
    REQUIRE(sol.projects.size() >= 1);
    CHECK(sol.projects[0].configurations["Debug|Win32"].character_set == "MultiByte");
    CHECK(sol.projects[0].configurations["Release|x64"].character_set == "Unicode");
}

TEST_CASE("BuildscriptWriter preserves config-specific x64 linker and language settings", "[buildscript_writer]") {
    Project proj;
    proj.name = "MixedSettings";
    proj.configurations["Debug|Win32"].config_type = "Application";
    proj.configurations["Debug|Win32"].platform_toolset = "v145";
    proj.configurations["Debug|Win32"].cl_compile.language_standard = "stdcpp20";
    proj.configurations["Debug|Win32"].link.sub_system = "Console";

    auto& x64 = proj.configurations["Debug|x64"];
    x64.config_type = "DynamicLibrary";
    x64.platform_toolset = "v143";
    x64.cl_compile.language_standard = "stdcpplatest";
    x64.cl_compile.additional_include_directories = {"..\\deps\\include", "..\\vkr\\"};
    x64.cl_compile.multi_processor_compilation = true;
    x64.link.sub_system = "Windows";
    x64.link.additional_library_directories = {"..\\deps\\lib\\", "..\\deps\\dbg", "$(OutDir)"};
    x64.link.additional_dependencies = {"glfw3.lib", "vkr.lib"};

    auto result = write_project(proj);
    CHECK(result.content.find("[config:Debug|x64]") != std::string::npos);
    CHECK(result.content.find("type = dll") != std::string::npos);
    CHECK(result.content.find("toolset = v143") != std::string::npos);
    CHECK(result.content.find("std = latest") != std::string::npos);
    CHECK(result.content.find("includes = ..\\deps\\include, ..\\vkr\\") != std::string::npos);
    CHECK(result.content.find("libdirs = ..\\deps\\lib\\, ..\\deps\\dbg, $(OutDir)") != std::string::npos);
    CHECK(result.content.find("libs = glfw3.lib, vkr.lib") != std::string::npos);
    CHECK(result.content.find("subsystem = Windows") != std::string::npos);

    BuildscriptParser parser;
    auto sol = parser.parse_string(result.content, result.temp_dir.string());
    REQUIRE(sol.projects.size() >= 1);

    auto& parsed = sol.projects[0].configurations["Debug|x64"];
    CHECK(parsed.config_type == "DynamicLibrary");
    CHECK(parsed.platform_toolset == "v143");
    CHECK(parsed.cl_compile.language_standard == "stdcpplatest");
    CHECK(parsed.link.sub_system == "Windows");
    CHECK(std::find(parsed.link.additional_dependencies.begin(),
                    parsed.link.additional_dependencies.end(),
                    "glfw3.lib") != parsed.link.additional_dependencies.end());
    CHECK(std::find(parsed.link.additional_library_directories.begin(),
                    parsed.link.additional_library_directories.end(),
                    "$(OutDir)") != parsed.link.additional_library_directories.end());
}
