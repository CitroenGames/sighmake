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

TEST_CASE("vcxproj reader: WindowsTargetPlatformVersion in Globals is imported", "[bugfix_vs]") {
    // Visual Studio writes the SDK version in the Globals group, which is also where the generator puts it.
    BugfixVsTempDir dir("globals_sdk");
    bugfix_vs_write_file(dir.path / "App.vcxproj", R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{11111111-2222-3333-4444-555555555555}</ProjectGuid>
    <WindowsTargetPlatformVersion>10.0.22621.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
</Project>
)");
    VcxprojReader reader;
    Project project = reader.read_vcxproj((dir.path / "App.vcxproj").string());
    REQUIRE(project.configurations.count("Debug|x64"));
    CHECK(project.configurations.at("Debug|x64").windows_target_platform_version == "10.0.22621.0");
}

TEST_CASE("vcxproj reader: conditions with spaces around == are understood", "[bugfix_vs]") {
    BugfixVsTempDir dir("cond_spaces");
    bugfix_vs_write_file(dir.path / "App.vcxproj", R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{11111111-2222-3333-4444-555555555555}</ProjectGuid>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)' == 'Release|x64'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
  </PropertyGroup>
  <ItemDefinitionGroup Condition=" '$(Configuration)|$(Platform)' == 'Release|x64' ">
    <ClCompile><PreprocessorDefinitions>FOO;%(PreprocessorDefinitions)</PreprocessorDefinitions></ClCompile>
  </ItemDefinitionGroup>
</Project>
)");
    VcxprojReader reader;
    Project project = reader.read_vcxproj((dir.path / "App.vcxproj").string());
    REQUIRE(project.configurations.count("Release|x64"));
    const auto& cfg = project.configurations.at("Release|x64");
    CHECK(cfg.config_type == "StaticLibrary");
    CHECK(cfg.cl_compile.preprocessor_definitions == std::vector<std::string>{"FOO"});
}

TEST_CASE("vcxproj reader: per-file %(AdditionalOptions) is not imported as an option", "[bugfix_vs]") {
    BugfixVsTempDir dir("file_opts");
    bugfix_vs_write_file(dir.path / "App.vcxproj", R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{11111111-2222-3333-4444-555555555555}</ProjectGuid>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ItemGroup>
    <ClCompile Include="big.cpp">
      <AdditionalOptions Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">/bigobj %(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
  </ItemGroup>
</Project>
)");
    VcxprojReader reader;
    Project project = reader.read_vcxproj((dir.path / "App.vcxproj").string());
    const SourceFile* src = bugfix_vs_find_source(project, "big.cpp");
    REQUIRE(src);
    REQUIRE(src->settings.additional_options.count("Debug|x64"));
    CHECK(src->settings.additional_options.at("Debug|x64") == std::vector<std::string>{"/bigobj"});
}

TEST_CASE("buildscript writer: per-file optimization and custom build inputs survive conversion", "[bugfix_vs]") {
    BugfixVsTempDir dir("writer_file_settings");
    Project project;
    project.name = "App";
    project.configurations["Debug|x64"].config_type = "Application";
    SourceFile slow;
    slow.path = "slow.cpp";
    slow.type = FileType::ClCompile;
    slow.settings.optimization["Debug|x64"] = "MaxSpeed";
    project.sources.push_back(slow);
    SourceFile gen;
    gen.path = "shader.hlsl";
    gen.type = FileType::CustomBuild;
    gen.custom_command["Debug|x64"] = "fxc shader.hlsl";
    gen.custom_outputs["Debug|x64"] = "shader.h";
    gen.custom_inputs["Debug|x64"] = "common.hlsli";
    project.sources.push_back(gen);

    const fs::path script = dir.path / "App.buildscript";
    BuildscriptWriter writer;
    REQUIRE(writer.write_buildscript(project, script.string()));

    BuildscriptParser parser;
    Solution solution = parser.parse(script.string());
    REQUIRE(solution.projects.size() == 1);
    const SourceFile* reparsed_slow = bugfix_vs_find_source(solution.projects[0], "slow.cpp");
    REQUIRE(reparsed_slow);
    CHECK(reparsed_slow->settings.optimization.count("Debug|x64"));
    if (reparsed_slow->settings.optimization.count("Debug|x64"))
        CHECK(reparsed_slow->settings.optimization.at("Debug|x64") == "MaxSpeed");
    const SourceFile* reparsed_gen = bugfix_vs_find_source(solution.projects[0], "shader.hlsl");
    REQUIRE(reparsed_gen);
    CHECK(reparsed_gen->custom_inputs.count("Debug|x64"));
    if (reparsed_gen->custom_inputs.count("Debug|x64"))
        CHECK(reparsed_gen->custom_inputs.at("Debug|x64") == "common.hlsli");
}

TEST_CASE("vcproj reader: MIDL TargetEnvironment maps to valid MSBuild values", "[bugfix_vs]") {
    // VCProjectEngine midlTargetEnvironment: 1 = Win32, 2 = Win64 (Itanium), 3 = AMD64.
    // MSBuild's Midl TargetEnvironment accepts Win32, Itanium, X64 (there is no "Win64").
    BugfixVsTempDir dir("vcproj_midl");
    bugfix_vs_write_file(dir.path / "App.vcproj", R"(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="9.00" Name="App" ProjectGUID="{11111111-2222-3333-4444-555555555555}">
  <Configurations>
    <Configuration Name="Debug|x64" ConfigurationType="1">
      <Tool Name="VCMIDLTool" TargetEnvironment="3"/>
    </Configuration>
    <Configuration Name="Debug|Itanium" ConfigurationType="1">
      <Tool Name="VCMIDLTool" TargetEnvironment="2"/>
    </Configuration>
  </Configurations>
  <Files/>
</VisualStudioProject>
)");
    VcprojReader reader;
    Project project = reader.read_vcproj((dir.path / "App.vcproj").string());
    REQUIRE(project.configurations.count("Debug|x64"));
    CHECK(project.configurations.at("Debug|x64").midl.target_environment == "X64");
    REQUIRE(project.configurations.count("Debug|Itanium"));
    CHECK(project.configurations.at("Debug|Itanium").midl.target_environment == "Itanium");
}

TEST_CASE("VPC: -$File removes a file instead of adding it", "[bugfix_vs]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Folder "Source"
    {
        $File "main.cpp"
        $File "unwanted.cpp"
    }
    -$File "unwanted.cpp"
    -$File "never_added.cpp"
}
)");
    REQUIRE(sol.projects.size() == 1);
    const auto& project = sol.projects[0];
    CHECK(bugfix_vs_find_source(project, "main.cpp") != nullptr);
    CHECK(bugfix_vs_find_source(project, "unwanted.cpp") == nullptr);
    CHECK(bugfix_vs_find_source(project, "never_added.cpp") == nullptr);
}

TEST_CASE("VPC: build events inside $Configuration apply to that configuration only", "[bugfix_vs]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Release"
    {
        $Compiler
        {
            $Optimization "MaxSpeed"
        }
    }
    $Configuration "Debug"
    {
        $PreBuildEvent
        {
            $CommandLine "echo debug-pre"
        }
        $PostBuildEvent
        {
            $CommandLine "echo debug-post"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    const auto& cfgs = sol.projects[0].configurations;
    for (const char* key : {"Debug|Win32", "Debug|x64"}) {
        REQUIRE(cfgs.count(key));
        CHECK(cfgs.at(key).pre_build_event.command == "echo debug-pre");
        CHECK(cfgs.at(key).post_build_event.command == "echo debug-post");
    }
    for (const char* key : {"Release|Win32", "Release|x64"}) {
        REQUIRE(cfgs.count(key));
        CHECK(cfgs.at(key).pre_build_event.command.empty());
        CHECK(cfgs.at(key).post_build_event.command.empty());
    }
}
