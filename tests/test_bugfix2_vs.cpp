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

struct Bugfix2VsTempDir {
    fs::path path;
    explicit Bugfix2VsTempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("sighmake_bugfix2_vs_" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~Bugfix2VsTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void bugfix2_vs_write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

struct Bugfix2VsGenerated {
    Solution solution;
    pugi::xml_document doc;
    fs::path vcxproj_path;
};

void bugfix2_vs_generate(const Bugfix2VsTempDir& dir, const std::string& buildscript, Bugfix2VsGenerated& out) {
    BuildscriptParser parser;
    out.solution = parser.parse_string(buildscript, dir.path.string());
    REQUIRE(!out.solution.projects.empty());
    fs::create_directories(dir.path / "build");
    out.vcxproj_path = dir.path / "build" / (out.solution.projects[0].name + ".vcxproj");
    VcxprojGenerator generator;
    REQUIRE(generator.generate_vcxproj(out.solution.projects[0], out.solution, out.vcxproj_path.string()));
    REQUIRE(out.doc.load_file(out.vcxproj_path.string().c_str()));
}

std::string bugfix2_vs_item_definition(const pugi::xml_document& doc, const std::string& tool,
                                       const std::string& element, const std::string& config_platform) {
    const std::string want = "'$(Configuration)|$(Platform)'=='" + config_platform + "'";
    for (auto idg : doc.child("Project").children("ItemDefinitionGroup")) {
        if (want != idg.attribute("Condition").as_string()) continue;
        if (auto node = idg.child(tool.c_str()).child(element.c_str())) return node.text().as_string();
    }
    return "<absent>";
}

} // namespace

TEST_CASE("vcxproj: c_standard 89/90/99 never emit an invalid LanguageStandard_C", "[bugfix2_vs]") {
    for (const char* std_value : {"89", "90", "99"}) {
        Bugfix2VsTempDir dir("cstd");
        Bugfix2VsGenerated gen;
        bugfix2_vs_generate(dir, std::string(R"(
[solution]
name = Test
configurations = Debug
platforms = x64

[project:app]
type = exe
files = main.c
c_standard = )") + std_value + "\n", gen);
        CHECK(bugfix2_vs_item_definition(gen.doc, "ClCompile", "LanguageStandard_C", "Debug|x64") == "<absent>");
    }
}

