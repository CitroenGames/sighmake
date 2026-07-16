#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/vcxproj_reader.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

struct ScopedCurrentPath {
    fs::path old_path;

    explicit ScopedCurrentPath(const fs::path& new_path)
        : old_path(fs::current_path()) {
        fs::current_path(new_path);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        fs::current_path(old_path, ec);
    }
};

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
    std::string xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
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
</Project>)xml";

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
    std::string xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
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
</Project>)xml";

    TempVcxproj temp(xml);

    // Create the optional.props file so the condition is true
    {
        std::ofstream props_file(temp.temp_dir / "optional.props");
        props_file << R"xml(<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <PreprocessorDefinitions>FROM_PROPS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>)xml";
    }

    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());
    auto& defs = proj.configurations["Debug|Win32"].cl_compile.preprocessor_definitions;
    CHECK(std::find(defs.begin(), defs.end(), "FROM_PROPS") != defs.end());
}

TEST_CASE("VcxprojReader inherits property sheet include directories without project override", "[vcxproj_reader]") {
    std::string xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>TestInheritedProps</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <Import Project="common.props" />
  </ImportGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <PreprocessorDefinitions>LOCAL_DEF;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
  </ItemGroup>
</Project>)xml";

    TempVcxproj temp(xml);

    {
        std::ofstream props_file(temp.temp_dir / "common.props");
        props_file << R"xml(<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>..\..\TerathonCode;..\..\SlugCode</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>FROM_COMMON;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <LanguageStandard>stdcpp17</LanguageStandard>
    </ClCompile>
    <Link>
      <AdditionalDependencies>opengl32.lib;ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <SubSystem>Windows</SubSystem>
    </Link>
  </ItemDefinitionGroup>
</Project>)xml";
    }

    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    const auto& cfg = proj.configurations["Debug|x64"];
    const auto& includes = cfg.cl_compile.additional_include_directories;
    CHECK(std::find(includes.begin(), includes.end(), "..\\..\\TerathonCode") != includes.end());
    CHECK(std::find(includes.begin(), includes.end(), "..\\..\\SlugCode") != includes.end());

    const auto& defs = cfg.cl_compile.preprocessor_definitions;
    CHECK(std::find(defs.begin(), defs.end(), "LOCAL_DEF") != defs.end());
    CHECK(std::find(defs.begin(), defs.end(), "FROM_COMMON") != defs.end());
    CHECK(cfg.cl_compile.language_standard == "stdcpp17");

    const auto& deps = cfg.link.additional_dependencies;
    CHECK(std::find(deps.begin(), deps.end(), "opengl32.lib") != deps.end());
    CHECK(std::find(deps.begin(), deps.end(), "ws2_32.lib") != deps.end());
    CHECK(cfg.link.sub_system == "Windows");
}

TEST_CASE("VcxprojReader drops default OutDir/IntDir on readback", "[vcxproj_reader]") {
    // If the generator emitted the exact default paths, readback should NOT
    // populate out_dir/int_dir — so a regenerated buildscript stays clean.
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectName>App</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <OutDir>bin\x64\Debug\</OutDir>
    <IntDir>obj\x64\Debug\App\</IntDir>
  </PropertyGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    auto& cfg = proj.configurations["Debug|x64"];
    CHECK(cfg.out_dir.empty());
    CHECK(cfg.int_dir.empty());
}

TEST_CASE("VcxprojReader keeps non-default OutDir/IntDir on readback", "[vcxproj_reader]") {
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectName>App</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <OutDir>custom\out\</OutDir>
    <IntDir>custom\int\</IntDir>
  </PropertyGroup>
</Project>)";

    TempVcxproj temp(xml);
    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    auto& cfg = proj.configurations["Debug|x64"];
    CHECK_FALSE(cfg.out_dir.empty());
    CHECK_FALSE(cfg.int_dir.empty());
    CHECK(cfg.out_dir.find("custom") != std::string::npos);
    CHECK(cfg.int_dir.find("custom") != std::string::npos);
}

struct TempSlnxSolution {
    fs::path temp_dir;
    fs::path slnx_path;
    fs::path sln_path;

    TempSlnxSolution() {
        temp_dir = fs::temp_directory_path() / "sighmake_test_slnx_reader";
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);
        slnx_path = temp_dir / "Engine.slnx";
        sln_path = temp_dir / "Engine.sln";
    }

    ~TempSlnxSolution() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void write_project(const std::string& filename,
                       const std::string& project_name,
                       const std::string& guid,
                       const std::string& config_type,
                       const std::vector<std::string>& config_keys = {"Debug|x64"},
                       const std::vector<std::string>& link_dependencies = {},
                       const std::vector<std::string>& sources = {}) {
        fs::create_directories((temp_dir / filename).parent_path());
        std::ofstream project_file(temp_dir / filename);
        project_file << R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
)";
        for (const auto& config_key : config_keys) {
            auto [config, platform] = parse_config_key(config_key);
            project_file << R"(    <ProjectConfiguration Include=")" << config_key << R"(">
      <Configuration>)" << config << R"(</Configuration>
      <Platform>)" << platform << R"(</Platform>
    </ProjectConfiguration>
)";
        }
        project_file << R"(  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{)" << guid << R"(}</ProjectGuid>
    <ProjectName>)" << project_name << R"(</ProjectName>
  </PropertyGroup>
)";
        for (const auto& config_key : config_keys) {
            project_file << R"(  <PropertyGroup Condition="'$(Configuration)|$(Platform)'==')" << config_key << R"('" Label="Configuration">
    <ConfigurationType>)" << config_type << R"(</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
)";
        }
        if (!link_dependencies.empty()) {
            for (const auto& config_key : config_keys) {
                project_file << R"(  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'==')" << config_key << R"('">
    <Link>
      <AdditionalDependencies>)";
                for (size_t i = 0; i < link_dependencies.size(); ++i) {
                    if (i > 0) project_file << ";";
                    project_file << link_dependencies[i];
                }
                project_file << R"(;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
)";
            }
        }
        if (!sources.empty()) {
            project_file << R"(  <ItemGroup>
)";
            for (const auto& source : sources) {
                project_file << R"(    <ClCompile Include=")" << source << R"(" />
)";
            }
            project_file << R"(  </ItemGroup>
)";
        }
        project_file << R"(</Project>)";
    }
};

TEST_CASE("SlnReader reads slnx projects configurations and dependencies", "[vcxproj_reader][slnx]") {
    TempSlnxSolution temp;
    temp.write_project("Lib.vcxproj", "Lib", "11111111-1111-1111-1111-111111111111", "StaticLibrary");
    temp.write_project("App.vcxproj", "App", "22222222-2222-2222-2222-222222222222", "Application");

    {
        std::ofstream slnx(temp.slnx_path);
        slnx << R"(<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <BuildType Name="Debug" />
    <BuildType Name="Release" />
    <Platform Name="x64" />
  </Configurations>
  <Project Path="Lib.vcxproj" Type="8bc9ceb8-8b4a-11d0-8d11-00a0c91bc942" Id="11111111-1111-1111-1111-111111111111" />
  <Project Path="App.vcxproj" Type="8bc9ceb8-8b4a-11d0-8d11-00a0c91bc942" Id="22222222-2222-2222-2222-222222222222">
    <BuildDependency Project="Lib.vcxproj" />
  </Project>
</Solution>)";
    }

    SlnReader reader;
    auto solution = reader.read_slnx(temp.slnx_path.string());

    CHECK(solution.name == "Engine");
    CHECK(std::find(solution.configurations.begin(), solution.configurations.end(), "Debug") != solution.configurations.end());
    CHECK(std::find(solution.configurations.begin(), solution.configurations.end(), "Release") != solution.configurations.end());
    CHECK(std::find(solution.platforms.begin(), solution.platforms.end(), "x64") != solution.platforms.end());
    REQUIRE(solution.projects.size() == 2);

    auto app_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                               [](const Project& project) { return project.name == "App"; });
    REQUIRE(app_it != solution.projects.end());
    CHECK(app_it->vcxproj_path == "App.vcxproj");
    REQUIRE(app_it->project_references.size() == 1);
    CHECK(app_it->project_references[0].name == "Lib");
    CHECK_FALSE(app_it->project_references[0].link_library_dependencies);
}

TEST_CASE("SlnReader converts bare slnx absolute project paths to relative root includes", "[vcxproj_reader][slnx][buildscript_writer]") {
    TempSlnxSolution temp;
    temp.write_project("Launcher/GameLauncher.vcxproj", "GameLauncher", "11111111-1111-1111-1111-111111111111", "Application");
    temp.write_project("engine/hyMain.vcxproj", "hyMain", "22222222-2222-2222-2222-222222222222", "DynamicLibrary");

    const std::string launcher_path = (temp.temp_dir / "Launcher" / "GameLauncher.vcxproj").generic_string();
    const std::string engine_path = (temp.temp_dir / "engine" / "hyMain.vcxproj").generic_string();

    {
        std::ofstream slnx(temp.slnx_path);
        slnx << R"(<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <BuildType Name="Debug" />
    <Platform Name="x64" />
  </Configurations>
  <Project Path=")" << launcher_path << R"(" Id="11111111-1111-1111-1111-111111111111" />
  <Project Path=")" << engine_path << R"(" Id="22222222-2222-2222-2222-222222222222" />
</Solution>)";
    }

    Solution solution;
    {
        ScopedCurrentPath cwd(temp.temp_dir);
        SlnReader reader;
        solution = reader.read_slnx(temp.slnx_path.filename().string());
    }

    REQUIRE(solution.projects.size() == 2);

    auto launcher_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                                    [](const Project& project) { return project.name == "GameLauncher"; });
    REQUIRE(launcher_it != solution.projects.end());
    CHECK(fs::path(launcher_it->vcxproj_path) == fs::path("Launcher") / "GameLauncher.vcxproj");

    auto engine_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                                  [](const Project& project) { return project.name == "hyMain"; });
    REQUIRE(engine_it != solution.projects.end());
    CHECK(fs::path(engine_it->vcxproj_path) == fs::path("engine") / "hyMain.vcxproj");

    BuildscriptWriter writer;
    REQUIRE(writer.write_solution_buildscripts(solution, temp.temp_dir.string()));

    const std::string root_content = read_file(temp.temp_dir / "Engine.buildscript");
    CHECK(root_content.find("include = Launcher/GameLauncher.buildscript") != std::string::npos);
    CHECK(root_content.find("include = engine/hyMain.buildscript") != std::string::npos);
    CHECK(root_content.find((temp.temp_dir / "Launcher" / "GameLauncher.buildscript").generic_string()) == std::string::npos);
    CHECK(root_content.find((temp.temp_dir / "engine" / "hyMain.buildscript").generic_string()) == std::string::npos);
}

TEST_CASE("SlnReader converts bare sln absolute project paths to relative root includes", "[vcxproj_reader][buildscript_writer]") {
    TempSlnxSolution temp;
    temp.write_project("client/client.vcxproj", "client", "11111111-1111-1111-1111-111111111111", "Application");
    temp.write_project("vkr/vkr.vcxproj", "vkr", "22222222-2222-2222-2222-222222222222", "StaticLibrary");

    const std::string client_path = (temp.temp_dir / "client" / "client.vcxproj").generic_string();
    const std::string vkr_path = (temp.temp_dir / "vkr" / "vkr.vcxproj").generic_string();

    {
        std::ofstream sln(temp.sln_path);
        sln << R"(Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
VisualStudioVersion = 17.0.31903.59
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "client", ")" << client_path << R"(", "{11111111-1111-1111-1111-111111111111}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "vkr", ")" << vkr_path << R"(", "{22222222-2222-2222-2222-222222222222}"
EndProject
Global
    GlobalSection(SolutionConfigurationPlatforms) = preSolution
        Debug|x64 = Debug|x64
    EndGlobalSection
EndGlobal
)";
    }

    Solution solution;
    {
        ScopedCurrentPath cwd(temp.temp_dir);
        SlnReader reader;
        solution = reader.read_sln(temp.sln_path.filename().string());
    }

    REQUIRE(solution.projects.size() == 2);

    auto client_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                                  [](const Project& project) { return project.name == "client"; });
    REQUIRE(client_it != solution.projects.end());
    CHECK(fs::path(client_it->vcxproj_path) == fs::path("client") / "client.vcxproj");

    auto vkr_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                               [](const Project& project) { return project.name == "vkr"; });
    REQUIRE(vkr_it != solution.projects.end());
    CHECK(fs::path(vkr_it->vcxproj_path) == fs::path("vkr") / "vkr.vcxproj");

    BuildscriptWriter writer;
    REQUIRE(writer.write_solution_buildscripts(solution, temp.temp_dir.string()));

    const std::string root_content = read_file(temp.temp_dir / "Engine.buildscript");
    CHECK(root_content.find("include = client/client.buildscript") != std::string::npos);
    CHECK(root_content.find("include = vkr/vkr.buildscript") != std::string::npos);
    CHECK(root_content.find((temp.temp_dir / "client" / "client.buildscript").generic_string()) == std::string::npos);
    CHECK(root_content.find((temp.temp_dir / "vkr" / "vkr.buildscript").generic_string()) == std::string::npos);
}

TEST_CASE("SlnReader infers solution-local deps include directory from source includes", "[vcxproj_reader][slnx]") {
    TempSlnxSolution temp;
    fs::create_directories(temp.temp_dir / "deps" / "include" / "GLFW");
    fs::create_directories(temp.temp_dir / "deps" / "include" / "RmlUi" / "Core");
    {
        std::ofstream header(temp.temp_dir / "deps" / "include" / "GLFW" / "glfw3.h");
        header << "#pragma once\n";
    }
    {
        std::ofstream header(temp.temp_dir / "deps" / "include" / "RmlUi" / "Core" / "Input.h");
        header << "#pragma once\n";
    }
    fs::create_directories(temp.temp_dir / "vkr");
    {
        std::ofstream source(temp.temp_dir / "vkr" / "vkr.cpp");
        source << "#include <GLFW/glfw3.h>\n"
               << "#include <RmlUi/Core/Input.h>\n";
    }

    temp.write_project("vkr/vkr.vcxproj", "vkr", "11111111-1111-1111-1111-111111111111", "StaticLibrary",
                       {"Debug|x64"}, {}, {"vkr.cpp"});

    {
        std::ofstream slnx(temp.slnx_path);
        slnx << R"(<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <BuildType Name="Debug" />
    <Platform Name="x64" />
  </Configurations>
  <Project Path="vkr/vkr.vcxproj" Id="11111111-1111-1111-1111-111111111111" />
</Solution>)";
    }

    SlnReader reader;
    auto solution = reader.read_slnx(temp.slnx_path.string());

    REQUIRE(solution.projects.size() == 1);
    auto& cfg = solution.projects[0].configurations.at("Debug|x64");
    CHECK(std::find(cfg.cl_compile.additional_include_directories.begin(),
                    cfg.cl_compile.additional_include_directories.end(),
                    std::string("..\\deps\\include")) != cfg.cl_compile.additional_include_directories.end());
}

TEST_CASE("SlnReader rebases stale absolute slnx paths and infers missing build types", "[vcxproj_reader][slnx]") {
    TempSlnxSolution temp;
    temp.write_project("Lib/Lib.vcxproj", "Lib", "11111111-1111-1111-1111-111111111111", "StaticLibrary",
                       {"Debug|Win32", "Development|x64"});
    temp.write_project("App/App.vcxproj", "App", "22222222-2222-2222-2222-222222222222", "Application",
                       {"Debug|Win32", "Development|x64"});

    {
        std::ofstream slnx(temp.slnx_path);
        slnx << R"(<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <Platform Name="x86" />
    <Platform Name="x64" />
  </Configurations>
  <Folder Name="/engine/">
    <Project Path="Z:/stale/root/Lib/Lib.vcxproj" Id="11111111-1111-1111-1111-111111111111" />
    <Project Path="App/App.vcxproj" Id="22222222-2222-2222-2222-222222222222">
      <BuildDependency Project="Z:/stale/root/Lib/Lib.vcxproj" />
    </Project>
  </Folder>
</Solution>)";
    }

    SlnReader reader;
    auto solution = reader.read_slnx(temp.slnx_path.string());

    CHECK(std::find(solution.configurations.begin(), solution.configurations.end(), "Debug") != solution.configurations.end());
    CHECK(std::find(solution.configurations.begin(), solution.configurations.end(), "Development") != solution.configurations.end());
    CHECK(std::find(solution.platforms.begin(), solution.platforms.end(), "Win32") != solution.platforms.end());
    CHECK(std::find(solution.platforms.begin(), solution.platforms.end(), "x64") != solution.platforms.end());
    REQUIRE(solution.projects.size() == 2);

    auto lib_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                               [](const Project& project) { return project.name == "Lib"; });
    REQUIRE(lib_it != solution.projects.end());
    CHECK(fs::path(lib_it->vcxproj_path) == fs::path("Lib") / "Lib.vcxproj");

    auto app_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                               [](const Project& project) { return project.name == "App"; });
    REQUIRE(app_it != solution.projects.end());
    REQUIRE(app_it->project_references.size() == 1);
    CHECK(app_it->project_references[0].name == "Lib");
    CHECK_FALSE(app_it->project_references[0].link_library_dependencies);
    CHECK(app_it->solution_folder == "engine");
}

TEST_CASE("SlnReader infers project dependency from matching linked lib", "[vcxproj_reader][slnx]") {
    TempSlnxSolution temp;
    temp.write_project("Lib/Lib.vcxproj", "Lib", "11111111-1111-1111-1111-111111111111", "StaticLibrary");
    temp.write_project("App/App.vcxproj", "App", "22222222-2222-2222-2222-222222222222", "Application",
                       {"Debug|x64"}, {"Lib.lib"});

    {
        std::ofstream slnx(temp.slnx_path);
        slnx << R"(<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <BuildType Name="Debug" />
    <Platform Name="x64" />
  </Configurations>
  <Project Path="Lib/Lib.vcxproj" Id="11111111-1111-1111-1111-111111111111" />
  <Project Path="App/App.vcxproj" Id="22222222-2222-2222-2222-222222222222" />
</Solution>)";
    }

    SlnReader reader;
    auto solution = reader.read_slnx(temp.slnx_path.string());

    auto app_it = std::find_if(solution.projects.begin(), solution.projects.end(),
                               [](const Project& project) { return project.name == "App"; });
    REQUIRE(app_it != solution.projects.end());
    REQUIRE(app_it->project_references.size() == 1);
    CHECK(app_it->project_references[0].name == "Lib");
    CHECK(app_it->project_references[0].link_library_dependencies);
}

TEST_CASE("VcxprojReader reads MASM items", "[vcxproj_reader]") {
    TempVcxproj temp(R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{12345678-1234-1234-1234-123456789012}</ProjectGuid>
    <ProjectName>AsmProject</ProjectName>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
  </PropertyGroup>
  <ItemGroup>
    <ClCompile Include="lib.cpp" />
    <MASM Include="fastmath.asm" />
  </ItemGroup>
</Project>)");

    VcxprojReader reader;
    auto proj = reader.read_vcxproj(temp.vcxproj_path.string());

    CHECK(proj.has_masm_files);
    auto asm_it = std::find_if(proj.sources.begin(), proj.sources.end(),
                               [](const SourceFile& src) { return src.path == "fastmath.asm"; });
    REQUIRE(asm_it != proj.sources.end());
    CHECK(asm_it->type == FileType::MASM);
}
