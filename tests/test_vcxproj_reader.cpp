#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/vcxproj_reader.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// RAII temp vcxproj for testing
struct TempVcxproj {
    fs::path temp_dir;
    fs::path vcxproj_path;

    TempVcxproj(const std::string& xml_content) {
        temp_dir = fs::temp_directory_path() / "sighmake_test_reader";
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);

        vcxproj_path = temp_dir / "test.vcxproj";
        std::ofstream f(vcxproj_path);
        f << xml_content;
    }

    ~TempVcxproj() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static const std::string MINIMAL_VCXPROJ = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestProject</ProjectName>
    <RootNamespace>TestProject</RootNamespace>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>MultiByte</CharacterSet>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <Optimization>Disabled</Optimization>
      <RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>
      <WarningLevel>Level3</WarningLevel>
      <PreprocessorDefinitions>WIN32;_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <LanguageStandard>stdcpp17</LanguageStandard>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="util.cpp" />
    <ClInclude Include="types.h" />
  </ItemGroup>
</Project>)";

// ============================================================================
// VcxprojReader tests
// ============================================================================

TEST_CASE("VcxprojReader reads project name from Globals", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    CHECK(proj.project_name == "TestProject");
}

TEST_CASE("VcxprojReader reads GUID from Globals", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    // Braces should be stripped
    CHECK(proj.uuid == "12345678-1234-1234-1234-123456789012");
}

TEST_CASE("VcxprojReader reads RootNamespace", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    CHECK(proj.root_namespace == "TestProject");
}

TEST_CASE("VcxprojReader reads ConfigurationType", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    REQUIRE(proj.configurations.count("Debug|Win32"));
    CHECK(proj.configurations["Debug|Win32"].config_type == "Application");
}

TEST_CASE("VcxprojReader reads PlatformToolset", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    CHECK(proj.configurations["Debug|Win32"].platform_toolset == "v143");
}

TEST_CASE("VcxprojReader reads compiler settings", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    auto& cfg = proj.configurations["Debug|Win32"];
    CHECK(cfg.cl_compile.optimization == "Disabled");
    CHECK(cfg.cl_compile.runtime_library == "MultiThreadedDebug");
    CHECK(cfg.cl_compile.warning_level == "Level3");
    CHECK(cfg.cl_compile.language_standard == "stdcpp17");
}

TEST_CASE("VcxprojReader reads preprocessor definitions", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    auto& defs = proj.configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    bool has_win32 = std::find(defs.begin(), defs.end(), "WIN32") != defs.end();
    bool has_debug = std::find(defs.begin(), defs.end(), "_DEBUG") != defs.end();
    CHECK(has_win32);
    CHECK(has_debug);
}

TEST_CASE("VcxprojReader reads linker settings", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    auto& cfg = proj.configurations["Debug|Win32"];
    CHECK(cfg.link.sub_system == "Console");
    CHECK(cfg.link.generate_debug_info == true);
}

TEST_CASE("VcxprojReader reads source files", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    int compile_count = 0;
    for (const auto& src : proj.sources) {
        if (src.type == FileType::ClCompile) compile_count++;
    }
    CHECK(compile_count == 2);
}

TEST_CASE("VcxprojReader reads header files", "[vcxproj_reader]") {
    TempVcxproj temp(MINIMAL_VCXPROJ);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    int include_count = 0;
    for (const auto& src : proj.sources) {
        if (src.type == FileType::ClInclude) include_count++;
    }
    CHECK(include_count == 1);
}

TEST_CASE("VcxprojReader reads StaticLibrary type", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}</ProjectGuid>
    <ProjectName>MyLib</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    REQUIRE(proj.configurations.count("Release|x64"));
    CHECK(proj.configurations["Release|x64"].config_type == "StaticLibrary");
}

TEST_CASE("VcxprojReader throws on invalid file", "[vcxproj_reader]") {
    VcxprojReader reader;
    CHECK_THROWS(reader.read_vcxproj("nonexistent_file.vcxproj"));
}

// ============================================================================
// Per-file settings
// ============================================================================

TEST_CASE("VcxprojReader reads per-file PCH setting", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestPCH</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <PrecompiledHeader>Use</PrecompiledHeader>
      <PrecompiledHeaderFile>pch.h</PrecompiledHeaderFile>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="pch.cpp">
      <PrecompiledHeader Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">Create</PrecompiledHeader>
    </ClCompile>
  </ItemGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    // Verify project-level PCH
    REQUIRE(proj.configurations.count("Debug|Win32"));
    CHECK(proj.configurations["Debug|Win32"].cl_compile.pch.mode == "Use");

    // Verify per-file PCH Create on pch.cpp
    bool found_create = false;
    for (const auto& src : proj.sources) {
        if (src.path.find("pch.cpp") != std::string::npos) {
            for (const auto& [key, pch_setting] : src.settings.pch) {
                if (pch_setting.mode == "Create") found_create = true;
            }
        }
    }
    CHECK(found_create);
}

// ============================================================================
// Resource files
// ============================================================================

TEST_CASE("VcxprojReader reads ResourceCompile files", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestRC</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ResourceCompile Include="app.rc" />
  </ItemGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    bool found_rc = false;
    for (const auto& src : proj.sources) {
        if (src.type == FileType::ResourceCompile) {
            found_rc = true;
        }
    }
    CHECK(found_rc);
}

// ============================================================================
// Build events
// ============================================================================

TEST_CASE("VcxprojReader reads PostBuildEvent", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestEvents</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <PostBuildEvent>
      <Command>echo done</Command>
      <Message>Running post-build</Message>
    </PostBuildEvent>
  </ItemDefinitionGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    REQUIRE(proj.configurations.count("Debug|Win32"));
    CHECK(proj.configurations["Debug|Win32"].post_build_event.command == "echo done");
}

// ============================================================================
// Multiple configurations
// ============================================================================

TEST_CASE("VcxprojReader reads multiple configurations", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>MultiConfig</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <Optimization>Disabled</Optimization>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <Optimization>MaxSpeed</Optimization>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    REQUIRE(proj.configurations.count("Debug|Win32"));
    REQUIRE(proj.configurations.count("Release|x64"));
    CHECK(proj.configurations["Debug|Win32"].cl_compile.optimization == "Disabled");
    CHECK(proj.configurations["Release|x64"].cl_compile.optimization == "MaxSpeed");
}

// ============================================================================
// DynamicLibrary type
// ============================================================================

TEST_CASE("VcxprojReader reads DynamicLibrary type", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}</ProjectGuid>
    <ProjectName>MyDll</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    REQUIRE(proj.configurations.count("Release|x64"));
    CHECK(proj.configurations["Release|x64"].config_type == "DynamicLibrary");
}

// ============================================================================
// File exclusion
// ============================================================================

TEST_CASE("VcxprojReader reads file ExcludedFromBuild", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestExclude</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="old.cpp">
      <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">true</ExcludedFromBuild>
    </ClCompile>
  </ItemGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    bool found_excluded = false;
    for (const auto& src : proj.sources) {
        if (src.path.find("old.cpp") != std::string::npos) {
            for (const auto& [key, val] : src.settings.excluded) {
                if (val) found_excluded = true;
            }
        }
    }
    CHECK(found_excluded);
}

TEST_CASE("VcxprojReader skips conditional import when file does not exist", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestConditionalImport</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <Import Project="optional.props" Condition="exists('optional.props')" />
  </ImportGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <PreprocessorDefinitions>BASE_DEF;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
  </ItemGroup>
</Project>)";

    TempVcxproj temp(xml);
    // optional.props does NOT exist in temp_dir
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    CHECK(proj.project_name == "TestConditionalImport");
    // Should load without errors - no props settings applied
    auto& defs = proj.configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    CHECK(std::find(defs.begin(), defs.end(), "BASE_DEF") != defs.end());
}

TEST_CASE("VcxprojReader loads conditional import when file exists", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestConditionalImportExists</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <Import Project="optional.props" Condition="exists('optional.props')" />
  </ImportGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <PreprocessorDefinitions>BASE_DEF;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
  </ItemGroup>
</Project>)";

    TempVcxproj temp(xml);

    // Create the optional.props file so the condition is true
    {
        std::ofstream props_file(temp.temp_dir / "optional.props");
        props_file << R"(<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <PreprocessorDefinitions>FROM_PROPS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>)";
    }

    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    auto& defs = proj.configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    CHECK(std::find(defs.begin(), defs.end(), "FROM_PROPS") != defs.end());
}
