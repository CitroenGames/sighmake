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
