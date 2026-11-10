#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/vcxproj_reader.hpp"
#include "parsers/vcproj_reader.hpp"
#include "parsers/vpc_parser.hpp"
#include "generators/vcxproj_generator.hpp"
#include "pugixml.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

// Fresh temp directory removed on scope exit.
struct BugfixVsTempDir {
    fs::path path;
    explicit BugfixVsTempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("sighmake_bugfix_vs_" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~BugfixVsTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void bugfix_vs_write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

// Parses a buildscript and writes the first project's .vcxproj directly (generate() needs
// a detected Visual Studio install; generate_vcxproj() does not).
struct BugfixVsGenerated {
    Solution solution;
    pugi::xml_document doc;
    fs::path vcxproj_path;
};

void bugfix_vs_generate(const BugfixVsTempDir& dir, const std::string& buildscript, BugfixVsGenerated& out) {
    BuildscriptParser parser;
    out.solution = parser.parse_string(buildscript, dir.path.string());
    REQUIRE(!out.solution.projects.empty());
    fs::create_directories(dir.path / "build");
    out.vcxproj_path = dir.path / "build" / (out.solution.projects[0].name + ".vcxproj");
    VcxprojGenerator generator;
    REQUIRE(generator.generate_vcxproj(out.solution.projects[0], out.solution, out.vcxproj_path.string()));
    REQUIRE(out.doc.load_file(out.vcxproj_path.string().c_str()));
}

std::string bugfix_vs_item_definition(const pugi::xml_document& doc, const std::string& tool,
                                      const std::string& element, const std::string& config_platform) {
    const std::string want = "'$(Configuration)|$(Platform)'=='" + config_platform + "'";
    for (auto idg : doc.child("Project").children("ItemDefinitionGroup")) {
        if (want != idg.attribute("Condition").as_string()) continue;
        if (auto node = idg.child(tool.c_str()).child(element.c_str())) return node.text().as_string();
    }
    return "<absent>";
}

const SourceFile* bugfix_vs_find_source(const Project& project, const std::string& filename) {
    for (const auto& src : project.sources) {
        if (fs::path(src.path).filename().string() == filename) return &src;
    }
    return nullptr;
}

} // namespace

TEST_CASE("vcxproj: rtti = false and force_conformance = false reach the compiler", "[bugfix_vs]") {
    // MSVC enables /GR and /Zc:forScope by default, so leaving the elements out keeps them on.
    BugfixVsTempDir dir("rtti");
    BugfixVsGenerated gen;
    bugfix_vs_generate(dir, R"(
[solution]
name = Test
configurations = Debug
platforms = x64

[project:App]
type = exe
sources = main.cpp
rtti = false
force_conformance = false
)", gen);
    CHECK(bugfix_vs_item_definition(gen.doc, "ClCompile", "RuntimeTypeInfo", "Debug|x64") == "false");
    CHECK(bugfix_vs_item_definition(gen.doc, "ClCompile", "ForceConformanceInForLoopScope", "Debug|x64") == "false");
}

TEST_CASE("vcxproj: /FIXED:NO does not disable ASLR", "[bugfix_vs]") {
    BugfixVsTempDir dir("fixed_no");
    BugfixVsGenerated gen;
    bugfix_vs_generate(dir, R"(
[solution]
name = Test
configurations = Debug
platforms = x64

[project:App]
type = exe
sources = main.cpp
ldflags = /FIXED:NO
)", gen);
    CHECK(bugfix_vs_item_definition(gen.doc, "Link", "RandomizedBaseAddress", "Debug|x64") == "<absent>");

    BugfixVsTempDir dir2("fixed_yes");
    BugfixVsGenerated gen2;
    bugfix_vs_generate(dir2, R"(
[solution]
name = Test
configurations = Debug
platforms = x64

[project:App]
type = exe
sources = main.cpp
ldflags = /FIXED
)", gen2);
    CHECK(bugfix_vs_item_definition(gen2.doc, "Link", "RandomizedBaseAddress", "Debug|x64") == "false");
}

TEST_CASE("vcxproj: c_standard = 17 is emitted as LanguageStandard_C stdc17", "[bugfix_vs]") {
    BugfixVsTempDir dir("cstd17");
    BugfixVsGenerated gen;
    bugfix_vs_generate(dir, R"(
[solution]
name = Test
configurations = Debug
platforms = x64

[project:App]
type = exe
sources = main.c
c_standard = 17
)", gen);
    CHECK(bugfix_vs_item_definition(gen.doc, "ClCompile", "LanguageStandard_C", "Debug|x64") == "stdc17");
}

