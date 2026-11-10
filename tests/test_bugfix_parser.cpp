#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

const Project* find_parser_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

const SourceFile* find_parser_source(const Project& proj, const std::string& filename) {
    for (const auto& src : proj.sources) {
        if (fs::path(src.path).filename().string() == filename) return &src;
    }
    return nullptr;
}

bool parser_contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

fs::path make_parser_temp_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("sighmake_bugfix_parser_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_parser_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

struct ParserScopedCwd {
    fs::path previous;
    explicit ParserScopedCwd(const fs::path& dir) : previous(fs::current_path()) { fs::current_path(dir); }
    ~ParserScopedCwd() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

}  // namespace

TEST_CASE("Wildcard sources expand when the buildscript is given by a relative path", "[bugfix_parser]") {
    const fs::path root = make_parser_temp_dir("relative_wildcard");
    write_parser_file(root / "proj" / "src" / "a.cpp", "");
    write_parser_file(root / "proj" / "src" / "b.cpp", "");
    write_parser_file(root / "proj" / "p.buildscript",
                      "[project:P]\ntype = exe\nsources = src/*.cpp\n");

    ParserScopedCwd cwd(root);
    BuildscriptParser parser;
    const Solution sol = parser.parse("proj/p.buildscript");
    REQUIRE(sol.projects.size() == 1);
    CHECK(find_parser_source(sol.projects[0], "a.cpp") != nullptr);
    CHECK(find_parser_source(sol.projects[0], "b.cpp") != nullptr);
}

TEST_CASE("A commented-out brace list does not swallow the following lines", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe
# sources = {
#     old.cpp
# }
sources = main.cpp
defines = KEEP_ME
)");
    REQUIRE(sol.projects.size() == 1);
    const Project& app = sol.projects[0];
    CHECK(find_parser_source(app, "main.cpp") != nullptr);
    CHECK(find_parser_source(app, "old.cpp") == nullptr);
    CHECK(parser_contains(app.configurations.at("Debug|Linux").cl_compile.preprocessor_definitions, "KEEP_ME"));
}

TEST_CASE("A brace list entry ending in a ${VAR} reference does not close the list", "[bugfix_parser]") {
    BuildscriptParser parser;
    parser.set_variables({{"EXTRA_DEF", "FROM_VAR"}});
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe
if(Android) {
    defines = ANDROID_ONLY
}
defines = {
    FIRST
    ${EXTRA_DEF}
}
defines = AFTER
)");
    REQUIRE(sol.projects.size() == 1);
    const auto& defines = sol.projects[0].configurations.at("Debug|Linux").cl_compile.preprocessor_definitions;
    CHECK(parser_contains(defines, "FIRST"));
    CHECK(parser_contains(defines, "FROM_VAR"));
    CHECK(parser_contains(defines, "AFTER"));
    CHECK_FALSE(parser_contains(defines, "ANDROID_ONLY"));
}

TEST_CASE("A second top-level folder after a project is a solution folder", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

folder("Libraries") {
    [project:Core]
    type = lib
}
folder("Apps") {
    [project:App]
    type = exe
}
)");
    const Project* core = find_parser_project(sol, "Core");
    const Project* app = find_parser_project(sol, "App");
    REQUIRE(core);
    REQUIRE(app);
    CHECK(core->solution_folder == "Libraries");
    CHECK(app->solution_folder == "Apps");
}

TEST_CASE("A closing brace closes the innermost block, folder inside if()", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

if(Android) {
    folder("Libs") {
        [project:Core]
        type = lib
    }
    [project:App]
    type = exe
}
)");
    const Project* core = find_parser_project(sol, "Core");
    const Project* app = find_parser_project(sol, "App");
    REQUIRE(core);
    REQUIRE(app);
    CHECK(core->solution_folder == "Libs");
    CHECK(app->solution_folder.empty());
}

TEST_CASE("Settings after an include that declares a project go to the including project", "[bugfix_parser]") {
    const fs::path root = make_parser_temp_dir("include_project");
    write_parser_file(root / "lib" / "lib.buildscript", "[project:Lib]\ntype = lib\n");
    write_parser_file(root / "main.buildscript", R"([solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe
include = lib/lib.buildscript
defines = APP_DEFINE
)");
    BuildscriptParser parser;
    const Solution sol = parser.parse((root / "main.buildscript").string());
    const Project* app = find_parser_project(sol, "App");
    const Project* lib = find_parser_project(sol, "Lib");
    REQUIRE(app);
    REQUIRE(lib);
    CHECK(parser_contains(app->configurations.at("Debug|Linux").cl_compile.preprocessor_definitions, "APP_DEFINE"));
    CHECK_FALSE(parser_contains(lib->configurations.at("Debug|Linux").cl_compile.preprocessor_definitions, "APP_DEFINE"));
}

TEST_CASE("A dependency reached publicly is not hidden by an earlier INTERFACE edge", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:Base]
type = lib
public_defines = BASE_API

[project:Mid]
type = lib
target_link_libraries(Base PUBLIC)

[project:App]
type = exe
target_link_libraries(Mid)
target_link_libraries(INTERFACE Base)
)");
    const Project* app = find_parser_project(sol, "App");
    REQUIRE(app);
    CHECK(parser_contains(app->configurations.at("Debug|Linux").cl_compile.preprocessor_definitions, "BASE_API"));
}

TEST_CASE("Platform selectors normalize x86 and android like the solution matrix", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug, Release
platforms = x86, x64, Android

[project:App]
type = exe
defines[x86] = ON_X86
defines[android] = ON_ANDROID
optimization[Release|x86] = MinSpace
src/a.cpp:defines[Debug|android] = FILE_ANDROID
)");
    REQUIRE(sol.projects.size() == 1);
    const Project& app = sol.projects[0];
    CHECK(parser_contains(app.configurations.at("Debug|Win32").cl_compile.preprocessor_definitions, "ON_X86"));
    CHECK(parser_contains(app.configurations.at("Release|Win32").cl_compile.preprocessor_definitions, "ON_X86"));
    CHECK(parser_contains(app.configurations.at("Debug|Android").cl_compile.preprocessor_definitions, "ON_ANDROID"));
    CHECK_FALSE(parser_contains(app.configurations.at("Debug|x64").cl_compile.preprocessor_definitions, "ON_X86"));
    CHECK(app.configurations.at("Release|Win32").cl_compile.optimization == "MinSpace");
    CHECK(app.configurations.count("Debug|x86") == 0);
    CHECK(app.configurations.count("Debug|android") == 0);
    const SourceFile* file = find_parser_source(app, "a.cpp");
    REQUIRE(file);
    CHECK(file->settings.preprocessor_defines.count("Debug|Android") == 1);
}

TEST_CASE("masm[x86] keeps the files for Win32", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = x86, x64

[project:App]
type = exe
masm[x86] = asm/fast32.asm
)");
    REQUIRE(sol.projects.size() == 1);
    const SourceFile* file = find_parser_source(sol.projects[0], "fast32.asm");
    REQUIRE(file);
    CHECK_FALSE(file->settings.excluded.count("Debug|Win32"));
    CHECK(file->settings.excluded.count("Debug|x64"));
}

TEST_CASE("Single-line set_file_properties applies its settings and ends", "[bugfix_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe
sources = src/main.cpp, src/legacy.c
set_file_properties(src/legacy.c, compile_as = CompileAsC)
defines = PROJECT_WIDE
set_file_properties(src/main.cpp,
    defines = A, B
)
)");
    REQUIRE(sol.projects.size() == 1);
    const Project& app = sol.projects[0];
    const SourceFile* legacy = find_parser_source(app, "legacy.c");
    const SourceFile* main_cpp = find_parser_source(app, "main.cpp");
    REQUIRE(legacy);
    REQUIRE(main_cpp);
    CHECK(legacy->settings.compile_as.count("*") == 1);
    CHECK(legacy->settings.preprocessor_defines.empty());
    CHECK(parser_contains(app.configurations.at("Debug|Linux").cl_compile.preprocessor_definitions, "PROJECT_WIDE"));
    REQUIRE(main_cpp->settings.preprocessor_defines.count("*") == 1);
    CHECK(main_cpp->settings.preprocessor_defines.at("*") == std::vector<std::string>{"A", "B"});
}
