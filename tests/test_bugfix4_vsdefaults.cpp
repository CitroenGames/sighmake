#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/vcxproj_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

const char* kNoSections = R"(
[solution]
configurations = Debug, Release, Profile
platforms = Win32, x64, ARM64

[project:App]
type = exe
sources = main.cpp
)";

const char* kWithSections = R"(
[solution]
configurations = Debug, Release, Profile
platforms = Win32, x64, ARM64

[project:App]
type = exe
sources = main.cpp

[config:Profile|x64]
defines = PROFILING
)";

Solution parse(const char* text) {
    BuildscriptParser parser;
    return parser.parse_string(text);
}

std::string read_vcxproj(const Solution& sol) {
    fs::path dir = fs::temp_directory_path() / "sighmake_bugfix4_vsdefaults";
    fs::create_directories(dir);
    fs::path file = dir / "App.vcxproj";
    VcxprojGenerator gen;
    REQUIRE(gen.generate_vcxproj(sol.projects[0], sol, file.string()));
    std::ifstream in(file, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

}  // namespace

TEST_CASE("Release defaults do not depend on [config:] sections", "[bugfix4_vsdefaults]") {
    const Solution without = parse(kNoSections);
    const Solution with = parse(kWithSections);
    for (const char* platform : {"Win32", "x64", "ARM64"}) {
        const std::string key = std::string("Release|") + platform;
        const auto& a = without.projects[0].configurations.at(key);
        const auto& b = with.projects[0].configurations.at(key);
        INFO(key);
        CHECK(a.link.generate_debug_info);
        CHECK(b.link.generate_debug_info);
        CHECK(b.link.optimize_references.value_or(false));
        CHECK(b.link.enable_comdat_folding.value_or(false));
        CHECK(b.cl_compile.function_level_linking.value_or(false));
        CHECK(b.cl_compile.intrinsic_functions.value_or(false));
        CHECK_FALSE(b.link_incremental);
        CHECK(a.cl_compile.optimization == b.cl_compile.optimization);
        CHECK(a.cl_compile.runtime_library == b.cl_compile.runtime_library);
        CHECK(a.cl_compile.debug_information_format == b.cl_compile.debug_information_format);
    }
}

TEST_CASE("Debug defaults do not depend on [config:] sections", "[bugfix4_vsdefaults]") {
    const Solution without = parse(kNoSections);
    const Solution with = parse(kWithSections);
    for (const char* platform : {"Win32", "x64", "ARM64"}) {
        const std::string key = std::string("Debug|") + platform;
        const auto& a = without.projects[0].configurations.at(key);
        const auto& b = with.projects[0].configurations.at(key);
        INFO(key);
        CHECK(a.link_incremental);
        CHECK(b.link_incremental);
        CHECK(b.link.generate_debug_info);
        CHECK(b.use_debug_libraries);
        CHECK(b.cl_compile.optimization == a.cl_compile.optimization);
        CHECK(b.cl_compile.runtime_library == a.cl_compile.runtime_library);
        CHECK(b.cl_compile.debug_information_format == a.cl_compile.debug_information_format);
    }
}

TEST_CASE("Custom configuration names get no Debug/Release specific linker defaults", "[bugfix4_vsdefaults]") {
    const Solution without = parse(kNoSections);
    const Solution with = parse(kWithSections);
    for (const Solution* sol : {&without, &with}) {
        const auto& cfg = sol->projects[0].configurations.at("Profile|Win32");
        CHECK_FALSE(cfg.link.generate_debug_info);
        CHECK_FALSE(cfg.link_incremental);
        CHECK_FALSE(cfg.use_debug_libraries);
    }
}

TEST_CASE("Explicit false still wins over Debug/Release defaults with sections", "[bugfix4_vsdefaults]") {
    const Solution sol = parse(R"(
[solution]
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp

[config:Release|x64]
generate_debug_info = false

[config:Debug|x64]
link_incremental = false
)");
    const auto& cfgs = sol.projects[0].configurations;
    CHECK_FALSE(cfgs.at("Release|x64").link.generate_debug_info);
    CHECK_FALSE(cfgs.at("Debug|x64").link_incremental);
    CHECK(cfgs.at("Debug|x64").link.generate_debug_info);
}

TEST_CASE("Generated vcxproj emits a pdb for Release with and without sections", "[bugfix4_vsdefaults]") {
    const std::string with = read_vcxproj(parse(kWithSections));
    const std::string without = read_vcxproj(parse(kNoSections));
    auto count = [](const std::string& s, const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    // Debug and Release are generated for 3 platforms each
    CHECK(count(with, "<LinkIncremental Condition") == count(without, "<LinkIncremental Condition"));
    CHECK(count(with, "<GenerateDebugInformation>true<") >= 6);
    CHECK(count(with, "<GenerateDebugInformation>true<") == count(without, "<GenerateDebugInformation>true<"));
}
