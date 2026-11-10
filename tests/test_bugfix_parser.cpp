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
