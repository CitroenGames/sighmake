#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

bool bf2_contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool bf2_has_suffix(const std::vector<std::string>& values, const std::string& suffix) {
    for (const auto& v : values) {
        if (v.size() >= suffix.size() && v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

struct Bf2CerrCapture {
    std::streambuf* old;
    std::ostringstream buffer;
    Bf2CerrCapture() : old(std::cerr.rdbuf(buffer.rdbuf())) {}
    ~Bf2CerrCapture() { std::cerr.rdbuf(old); }
    std::string str() const { return buffer.str(); }
};

const SourceFile* bf2_source(const Project& proj, const std::string& filename) {
    for (const auto& src : proj.sources) {
        if (fs::path(src.path).filename().string() == filename) return &src;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("Project-level libs with a path reach the per-config link dependencies", "[bugfix2_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe
libs = deps/libfoo.a, pthread
)", "/tmp/bf2/sub");
    REQUIRE(sol.projects.size() == 1);
    const auto& deps = sol.projects[0].configurations.at("Debug|Linux").link.additional_dependencies;
    CHECK(bf2_contains(deps, "pthread"));
    CHECK(bf2_contains(deps, fs::path("/tmp/bf2/sub/deps/libfoo.a").lexically_normal().string()));
}

TEST_CASE("A later unconditional type does not clobber a platform-conditional type", "[bugfix2_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux, Android

[project:App]
if(Android) {
    type = dll
}
type = exe
)");
    REQUIRE(sol.projects.size() == 1);
    const Project& app = sol.projects[0];
    CHECK(app.configurations.at("Debug|Android").config_type == "DynamicLibrary");
    CHECK(app.configurations.at("Debug|Linux").config_type == "Application");
}

TEST_CASE("A project-level key inside a config section warns with line and key", "[bugfix2_parser]") {
    Bf2CerrCapture cerr_capture;
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug
platforms = Linux

[project:App]
type = exe

[config:Debug|Linux]
sources = late.cpp
)");
    const std::string err = cerr_capture.str();
    CHECK(err.find("sources") != std::string::npos);
    CHECK(err.find("line 10") != std::string::npos);
}

TEST_CASE("A wildcard config section forms the platform dimension like a literal one", "[bugfix2_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug

[project:App]
type = exe

[config:*|Linux]
defines = ON_LINUX
)");
    REQUIRE(sol.platforms.size() == 1);
    CHECK(sol.platforms[0] == "Linux");
    CHECK(sol.projects.at(0).configurations.count("Debug|Linux") == 1);
}

TEST_CASE("Per-file settings inside a config section apply to that configuration only", "[bugfix2_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(R"(
[solution]
configurations = Debug, Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp

[config:Debug|Linux]
main.cpp:defines = ONLY_DEBUG
)");
    REQUIRE(sol.projects.size() == 1);
    const SourceFile* main = bf2_source(sol.projects[0], "main.cpp");
    REQUIRE(main != nullptr);
    const auto& defs = main->settings.preprocessor_defines;
    CHECK(defs.count("*") == 0);
    REQUIRE(defs.count("Debug|Linux") == 1);
    CHECK(bf2_contains(defs.at("Debug|Linux"), "ONLY_DEBUG"));
    CHECK(defs.count("Release|Linux") == 0);
}

TEST_CASE("Text after an opening triple quote is part of the multi-line value", "[bugfix2_parser]") {
    BuildscriptParser parser;
    const Solution sol = parser.parse_string(
        "[solution]\nconfigurations = Debug\nplatforms = Linux\n\n"
        "[project:App]\ntype = exe\nprebuild = \"\"\"touch a\ntouch b\"\"\"\n");
    REQUIRE(sol.projects.size() == 1);
    const std::string& cmd = sol.projects[0].configurations.at("Debug|Linux").pre_build_event.command;
    CHECK(cmd.find("touch a") != std::string::npos);
    CHECK(cmd.find("touch b") != std::string::npos);
}

