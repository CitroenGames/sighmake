#include "pch.h"
#include "catch_amalgamated.hpp"
#include "common/project_types.hpp"

using namespace vcxproj;

// ============================================================================
// get_file_type tests
// ============================================================================

TEST_CASE("get_file_type identifies C++ source files", "[project_types]") {
    CHECK(get_file_type("main.cpp") == FileType::ClCompile);
    CHECK(get_file_type("util.cc") == FileType::ClCompile);
    CHECK(get_file_type("helper.cxx") == FileType::ClCompile);
    CHECK(get_file_type("legacy.c") == FileType::ClCompile);
}

TEST_CASE("get_file_type identifies header files", "[project_types]") {
    CHECK(get_file_type("types.h") == FileType::ClInclude);
    CHECK(get_file_type("types.hpp") == FileType::ClInclude);
    CHECK(get_file_type("types.hxx") == FileType::ClInclude);
    CHECK(get_file_type("types.hh") == FileType::ClInclude);
}

TEST_CASE("get_file_type identifies resource files", "[project_types]") {
    CHECK(get_file_type("app.rc") == FileType::ResourceCompile);
}

TEST_CASE("get_file_type identifies MASM files", "[project_types]") {
    CHECK(get_file_type("code.asm") == FileType::MASM);
    CHECK(get_file_type("code.masm") == FileType::MASM);
}

TEST_CASE("get_file_type returns None for unknown extensions", "[project_types]") {
    CHECK(get_file_type("readme.txt") == FileType::None);
    CHECK(get_file_type("data.json") == FileType::None);
    CHECK(get_file_type("Makefile") == FileType::None);
}

TEST_CASE("get_file_type is case insensitive", "[project_types]") {
    CHECK(get_file_type("Main.CPP") == FileType::ClCompile);
    CHECK(get_file_type("Types.HPP") == FileType::ClInclude);
    CHECK(get_file_type("App.RC") == FileType::ResourceCompile);
    CHECK(get_file_type("Code.ASM") == FileType::MASM);
}

TEST_CASE("get_file_type handles paths with directories", "[project_types]") {
    CHECK(get_file_type("src/main.cpp") == FileType::ClCompile);
    CHECK(get_file_type("include/types.hpp") == FileType::ClInclude);
    CHECK(get_file_type("res/app.rc") == FileType::ResourceCompile);
}

// ============================================================================
// parse_config_key tests
// ============================================================================

TEST_CASE("parse_config_key splits config|platform", "[project_types]") {
    auto [config, platform] = parse_config_key("Debug|Win32");
    CHECK(config == "Debug");
    CHECK(platform == "Win32");
}

TEST_CASE("parse_config_key handles x64 platform", "[project_types]") {
    auto [config, platform] = parse_config_key("Release|x64");
    CHECK(config == "Release");
    CHECK(platform == "x64");
}

TEST_CASE("parse_config_key defaults platform to Win32 when no pipe", "[project_types]") {
    auto [config, platform] = parse_config_key("Debug");
    CHECK(config == "Debug");
    CHECK(platform == "Win32");
}

// ============================================================================
// to_lower tests
// ============================================================================

TEST_CASE("to_lower converts strings to lowercase", "[project_types]") {
    CHECK(to_lower("Hello") == "hello");
    CHECK(to_lower("UPPER") == "upper");
    CHECK(to_lower("already") == "already");
    CHECK(to_lower("MiXeD123") == "mixed123");
    CHECK(to_lower("") == "");
}

// ============================================================================
// is_windows_platform / is_linux_platform tests
// ============================================================================

TEST_CASE("is_windows_platform recognizes Windows platforms", "[project_types]") {
    CHECK(is_windows_platform("Win32") == true);
    CHECK(is_windows_platform("x64") == true);
    CHECK(is_windows_platform("x86") == true);
    CHECK(is_windows_platform("ARM") == true);
    CHECK(is_windows_platform("ARM64") == true);
}

TEST_CASE("is_windows_platform is case insensitive", "[project_types]") {
    CHECK(is_windows_platform("win32") == true);
    CHECK(is_windows_platform("X64") == true);
}

TEST_CASE("is_windows_platform rejects non-Windows platforms", "[project_types]") {
    CHECK(is_windows_platform("Linux") == false);
    CHECK(is_windows_platform("MacOS") == false);
}

TEST_CASE("is_linux_platform recognizes Linux", "[project_types]") {
    CHECK(is_linux_platform("Linux") == true);
    CHECK(is_linux_platform("linux") == true);
}

TEST_CASE("is_linux_platform rejects non-Linux platforms", "[project_types]") {
    CHECK(is_linux_platform("Win32") == false);
    CHECK(is_linux_platform("x64") == false);
}

TEST_CASE("is_unix_platform recognizes Linux and macOS platforms", "[project_types]") {
    CHECK(is_unix_platform("Linux") == true);
    CHECK(is_unix_platform("linux") == true);
    CHECK(is_unix_platform("macOS") == true);
    CHECK(is_unix_platform("macos") == true);
    CHECK(is_unix_platform("Darwin") == true);
    CHECK(is_unix_platform("darwin") == true);
    CHECK(is_unix_platform("osx") == true);
    CHECK(is_unix_platform("OSX") == true);
}

TEST_CASE("is_unix_platform rejects Windows platforms", "[project_types]") {
    CHECK(is_unix_platform("Win32") == false);
    CHECK(is_unix_platform("x64") == false);
    CHECK(is_unix_platform("ARM64") == false);
}

// ============================================================================
// parse_visibility / visibility_to_string tests
// ============================================================================

TEST_CASE("parse_visibility parses visibility keywords", "[project_types]") {
    CHECK(parse_visibility("PUBLIC") == DependencyVisibility::PUBLIC);
    CHECK(parse_visibility("PRIVATE") == DependencyVisibility::PRIVATE);
    CHECK(parse_visibility("INTERFACE") == DependencyVisibility::INTERFACE);
}

TEST_CASE("parse_visibility defaults to PUBLIC for unknown", "[project_types]") {
    CHECK(parse_visibility("unknown") == DependencyVisibility::PUBLIC);
    CHECK(parse_visibility("") == DependencyVisibility::PUBLIC);
}

TEST_CASE("visibility_to_string converts enum to string", "[project_types]") {
    CHECK(visibility_to_string(DependencyVisibility::PUBLIC) == "PUBLIC");
    CHECK(visibility_to_string(DependencyVisibility::PRIVATE) == "PRIVATE");
    CHECK(visibility_to_string(DependencyVisibility::INTERFACE) == "INTERFACE");
}

TEST_CASE("parse_visibility and visibility_to_string round-trip", "[project_types]") {
    for (auto vis : {DependencyVisibility::PUBLIC, DependencyVisibility::PRIVATE, DependencyVisibility::INTERFACE}) {
        CHECK(parse_visibility(visibility_to_string(vis)) == vis);
    }
}

// ============================================================================
// detect_project_language tests
// ============================================================================

TEST_CASE("detect_project_language returns explicit language", "[project_types]") {
    Project proj;
    proj.language = "C";
    CHECK(detect_project_language(proj) == "C");

    proj.language = "C++";
    CHECK(detect_project_language(proj) == "C++");
}

TEST_CASE("detect_project_language auto-detects C++ from .cpp files", "[project_types]") {
    Project proj;
    proj.sources.push_back(SourceFile{"main.cpp"});
    proj.sources.push_back(SourceFile{"util.c"});
    CHECK(detect_project_language(proj) == "C++");
}

TEST_CASE("detect_project_language auto-detects C from only .c files", "[project_types]") {
    Project proj;
    proj.sources.push_back(SourceFile{"main.c"});
    proj.sources.push_back(SourceFile{"util.c"});
    CHECK(detect_project_language(proj) == "C");
}

TEST_CASE("detect_project_language defaults to C++ for empty project", "[project_types]") {
    Project proj;
    CHECK(detect_project_language(proj) == "C++");
}

TEST_CASE("detect_project_language recognizes .cc and .cxx as C++", "[project_types]") {
    Project proj;
    proj.sources.push_back(SourceFile{"main.cc"});
    CHECK(detect_project_language(proj) == "C++");

    Project proj2;
    proj2.sources.push_back(SourceFile{"main.cxx"});
    CHECK(detect_project_language(proj2) == "C++");
}

// ============================================================================
// Solution::get_config_keys tests
// ============================================================================

TEST_CASE("get_config_keys returns cartesian product", "[project_types]") {
    Solution sol;
    sol.configurations = {"Debug", "Release"};
    sol.platforms = {"Win32", "x64"};

    auto keys = sol.get_config_keys();
    REQUIRE(keys.size() == 4);
    CHECK(keys[0] == "Debug|Win32");
    CHECK(keys[1] == "Debug|x64");
    CHECK(keys[2] == "Release|Win32");
    CHECK(keys[3] == "Release|x64");
}

TEST_CASE("get_config_keys with single config and platform", "[project_types]") {
    Solution sol;
    sol.configurations = {"Release"};
    sol.platforms = {"x64"};

    auto keys = sol.get_config_keys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "Release|x64");
}

TEST_CASE("get_config_keys with empty produces empty", "[project_types]") {
    Solution sol;
    auto keys = sol.get_config_keys();
    CHECK(keys.empty());
}

// ============================================================================
// ProjectDependency tests
// ============================================================================

TEST_CASE("ProjectDependency default visibility is PUBLIC", "[project_types]") {
    ProjectDependency dep;
    CHECK(dep.visibility == DependencyVisibility::PUBLIC);
}

TEST_CASE("ProjectDependency constructor with name", "[project_types]") {
    ProjectDependency dep("MyLib");
    CHECK(dep.name == "MyLib");
    CHECK(dep.visibility == DependencyVisibility::PUBLIC);
}

TEST_CASE("ProjectDependency constructor with name and visibility", "[project_types]") {
    ProjectDependency dep("MyLib", DependencyVisibility::PRIVATE);
    CHECK(dep.name == "MyLib");
    CHECK(dep.visibility == DependencyVisibility::PRIVATE);
}
