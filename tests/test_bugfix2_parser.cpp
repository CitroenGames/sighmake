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

