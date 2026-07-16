#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/vcproj_reader.hpp"
#include "parsers/vcxproj_reader.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// RAII temp dir with a .vcproj (and optional side files) for testing
struct TempVcproj {
    fs::path temp_dir;
    fs::path vcproj_path;

    TempVcproj(const std::string& xml_content) {
        temp_dir = fs::temp_directory_path() / "sighmake_test_vcproj_reader";
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);

        vcproj_path = temp_dir / "test.vcproj";
        std::ofstream f(vcproj_path);
        f << xml_content;
    }

    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream f(temp_dir / filename);
        f << content;
    }

    ~TempVcproj() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static const std::string LEGACY_VCPROJ = R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject
	ProjectType="Visual C++"
	Version="9.00"
	Name="LegacyApp"
	ProjectGUID="{ABCDEF12-3456-7890-ABCD-EF1234567890}"
	RootNamespace="LegacyApp"
	Keyword="Win32Proj"
	>
	<Platforms>
		<Platform Name="Win32"/>
	</Platforms>
	<Configurations>
		<Configuration
			Name="Debug|Win32"
			OutputDirectory="$(SolutionDir)$(ConfigurationName)"
			IntermediateDirectory="$(ConfigurationName)"
			ConfigurationType="1"
			CharacterSet="1"
			>
			<Tool
				Name="VCCLCompilerTool"
				Optimization="0"
				AdditionalIncludeDirectories="..\include;&quot;..\third party\inc&quot;"
				PreprocessorDefinitions="WIN32;_DEBUG,_CONSOLE"
				MinimalRebuild="true"
				BasicRuntimeChecks="3"
				RuntimeLibrary="3"
				UsePrecompiledHeader="2"
				PrecompiledHeaderThrough="stdafx.h"
				WarningLevel="3"
				DebugInformationFormat="4"
				RuntimeTypeInfo="false"
				ExceptionHandling="1"
				DisableSpecificWarnings="4996;4267"
			/>
			<Tool
				Name="VCLinkerTool"
				AdditionalDependencies="ws2_32.lib &quot;my lib.lib&quot;"
				LinkIncremental="2"
				AdditionalLibraryDirectories="..\lib"
				GenerateDebugInformation="true"
				SubSystem="1"
				OptimizeReferences="2"
				TargetMachine="1"
			/>
			<Tool
				Name="VCPostBuildEventTool"
				CommandLine="copy $(TargetPath) ..\bin"
				Description="Copying output"
			/>
		</Configuration>
		<Configuration
			Name="Release|Win32"
			ConfigurationType="1"
			CharacterSet="1"
			WholeProgramOptimization="1"
			>
			<Tool
				Name="VCCLCompilerTool"
				Optimization="2"
				RuntimeLibrary="2"
				WarningLevel="3"
			/>
			<Tool
				Name="VCLinkerTool"
				SubSystem="1"
			/>
		</Configuration>
	</Configurations>
	<References>
		<ProjectReference
			ReferencedProjectIdentifier="{11111111-2222-3333-4444-555555555555}"
			RelativePathToProject=".\core\CoreLib.vcproj"
		/>
	</References>
	<Files>
		<Filter
			Name="Source Files"
			Filter="cpp;c;cc;cxx"
			>
			<File RelativePath=".\src\main.cpp">
			</File>
			<File RelativePath=".\src\util.cpp">
				<FileConfiguration Name="Debug|Win32">
					<Tool
						Name="VCCLCompilerTool"
						PreprocessorDefinitions="UTIL_DEBUG"
						UsePrecompiledHeader="0"
					/>
				</FileConfiguration>
			</File>
			<File RelativePath=".\src\generated.cpp">
				<FileConfiguration Name="Debug|Win32" ExcludedFromBuild="true">
				</FileConfiguration>
			</File>
		</Filter>
		<Filter Name="Header Files">
			<File RelativePath=".\src\util.h">
			</File>
		</Filter>
		<File RelativePath=".\app.rc">
		</File>
		<File RelativePath=".\data\shader.fx">
			<FileConfiguration Name="Debug|Win32">
				<Tool
					Name="VCCustomBuildTool"
					CommandLine="fxc /Fo $(OutDir)\$(InputName).fxo $(InputPath)"
					Description="Compiling shader"
					Outputs="$(OutDir)\$(InputName).fxo"
				/>
			</FileConfiguration>
		</File>
	</Files>
	<Globals>
	</Globals>
</VisualStudioProject>)XML";

static const SourceFile* find_source(const Project& proj, const std::string& path) {
    for (const auto& src : proj.sources) {
        if (src.path == path) return &src;
    }
    return nullptr;
}

// ============================================================================
// VcprojReader tests
// ============================================================================

TEST_CASE("VcprojReader reads project identity", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    CHECK(proj.project_name == "LegacyApp");
    CHECK(proj.name == "LegacyApp");
    CHECK(proj.uuid == "ABCDEF12-3456-7890-ABCD-EF1234567890");
    CHECK(proj.root_namespace == "LegacyApp");
}

TEST_CASE("VcprojReader maps configuration attributes", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    REQUIRE(proj.configurations.count("Debug|Win32"));
    REQUIRE(proj.configurations.count("Release|Win32"));

    auto& debug = proj.configurations["Debug|Win32"];
    CHECK(debug.config_type == "Application");
    CHECK(debug.character_set == "Unicode");
    // $(ConfigurationName)/$(PlatformName) are translated to MSBuild macros
    CHECK(debug.out_dir == "$(SolutionDir)$(Configuration)");
    CHECK(debug.int_dir == "$(Configuration)");

    auto& release = proj.configurations["Release|Win32"];
    CHECK(release.whole_program_optimization);
}

TEST_CASE("VcprojReader maps compiler tool enums", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cl = proj.configurations["Debug|Win32"].cl_compile;
    CHECK(cl.optimization == "Disabled");
    CHECK(cl.runtime_library == "MultiThreadedDebugDLL");
    CHECK(cl.warning_level == "Level3");
    CHECK(cl.debug_information_format == "EditAndContinue");
    CHECK(cl.basic_runtime_checks == "EnableFastChecks");
    CHECK(cl.exception_handling == "Sync");
    CHECK(cl.minimal_rebuild);
    CHECK_FALSE(cl.runtime_type_info);
    CHECK(cl.pch.mode == "Use");
    CHECK(cl.pch.header == "stdafx.h");

    REQUIRE(cl.disable_specific_warnings.size() == 2);
    CHECK(cl.disable_specific_warnings[0] == "4996");
    CHECK(cl.disable_specific_warnings[1] == "4267");

    auto& release_cl = proj.configurations["Release|Win32"].cl_compile;
    CHECK(release_cl.optimization == "MaxSpeed");
    CHECK(release_cl.runtime_library == "MultiThreadedDLL");
}

TEST_CASE("VcprojReader splits list attributes on semicolons and commas", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cl = proj.configurations["Debug|Win32"].cl_compile;
    REQUIRE(cl.preprocessor_definitions.size() == 3);
    CHECK(cl.preprocessor_definitions[0] == "WIN32");
    CHECK(cl.preprocessor_definitions[1] == "_DEBUG");
    CHECK(cl.preprocessor_definitions[2] == "_CONSOLE");

    // Quoted entries have their quotes stripped
    REQUIRE(cl.additional_include_directories.size() == 2);
    CHECK(cl.additional_include_directories[0] == "..\\include");
    CHECK(cl.additional_include_directories[1] == "..\\third party\\inc");
}

TEST_CASE("VcprojReader maps linker tool settings", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cfg = proj.configurations["Debug|Win32"];
    auto& link = cfg.link;

    // vcproj AdditionalDependencies are space-separated with quoting
    REQUIRE(link.additional_dependencies.size() == 2);
    CHECK(link.additional_dependencies[0] == "ws2_32.lib");
    CHECK(link.additional_dependencies[1] == "my lib.lib");

    REQUIRE(link.additional_library_directories.size() == 1);
    CHECK(link.additional_library_directories[0] == "..\\lib");

    CHECK(link.sub_system == "Console");
    CHECK(link.generate_debug_info);
    CHECK(link.target_machine == "MachineX86");
    REQUIRE(link.optimize_references.has_value());
    CHECK(link.optimize_references.value());
    CHECK(cfg.link_incremental);
}

TEST_CASE("VcprojReader reads build events", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cfg = proj.configurations["Debug|Win32"];
    CHECK(cfg.post_build_event.command == "copy $(TargetPath) ..\\bin");
    CHECK(cfg.post_build_event.message == "Copying output");
    CHECK(cfg.post_build_event.use_in_build);
}

TEST_CASE("VcprojReader reads files with filters and types", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    const auto* main_cpp = find_source(proj, "src\\main.cpp");
    REQUIRE(main_cpp);
    CHECK(main_cpp->type == FileType::ClCompile);
    CHECK(main_cpp->filter == "Source Files");

    const auto* util_h = find_source(proj, "src\\util.h");
    REQUIRE(util_h);
    CHECK(util_h->type == FileType::ClInclude);
    CHECK(util_h->filter == "Header Files");

    const auto* rc = find_source(proj, "app.rc");
    REQUIRE(rc);
    CHECK(rc->type == FileType::ResourceCompile);
    CHECK(rc->filter.empty());
}

TEST_CASE("VcprojReader reads per-file configuration overrides", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    const auto* util_cpp = find_source(proj, "src\\util.cpp");
    REQUIRE(util_cpp);
    REQUIRE(util_cpp->settings.preprocessor_defines.count("Debug|Win32"));
    REQUIRE(util_cpp->settings.preprocessor_defines.at("Debug|Win32").size() == 1);
    CHECK(util_cpp->settings.preprocessor_defines.at("Debug|Win32")[0] == "UTIL_DEBUG");
    REQUIRE(util_cpp->settings.pch.count("Debug|Win32"));
    CHECK(util_cpp->settings.pch.at("Debug|Win32").mode == "NotUsing");

    const auto* generated = find_source(proj, "src\\generated.cpp");
    REQUIRE(generated);
    REQUIRE(generated->settings.excluded.count("Debug|Win32"));
    CHECK(generated->settings.excluded.at("Debug|Win32"));
}

TEST_CASE("VcprojReader converts custom build steps and file macros", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    const auto* shader = find_source(proj, "data\\shader.fx");
    REQUIRE(shader);
    CHECK(shader->type == FileType::CustomBuild);
    REQUIRE(shader->custom_command.count("Debug|Win32"));
    // $(InputName)/$(InputPath) translate to MSBuild item metadata
    CHECK(shader->custom_command.at("Debug|Win32") ==
          "fxc /Fo $(OutDir)\\%(Filename).fxo %(FullPath)");
    REQUIRE(shader->custom_outputs.count("Debug|Win32"));
    CHECK(shader->custom_outputs.at("Debug|Win32") == "$(OutDir)\\%(Filename).fxo");
    REQUIRE(shader->custom_message.count("Debug|Win32"));
    CHECK(shader->custom_message.at("Debug|Win32") == "Compiling shader");
}

TEST_CASE("VcprojReader reads project references", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    REQUIRE(proj.project_references.size() == 1);
    CHECK(proj.project_references[0].name == "CoreLib");
}

TEST_CASE("VcprojReader detects C++ language", "[vcproj_reader]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());
    CHECK(proj.language == "C++");
}

TEST_CASE("VcprojReader maps VS2003 precompiled header enum", "[vcproj_reader]") {
    TempVcproj temp(R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="7.10" Name="Old" ProjectGUID="{00000000-0000-0000-0000-000000000001}">
	<Configurations>
		<Configuration Name="Debug|Win32" ConfigurationType="1">
			<Tool Name="VCCLCompilerTool" UsePrecompiledHeader="3" PrecompiledHeaderThrough="stdafx.h" ExceptionHandling="TRUE"/>
		</Configuration>
	</Configurations>
	<Files>
		<File RelativePath=".\main.cpp"/>
	</Files>
</VisualStudioProject>)XML");

    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cl = proj.configurations["Debug|Win32"].cl_compile;
    // VS2003: 3 = use (/Yu); bool-style ExceptionHandling maps to Sync
    CHECK(cl.pch.mode == "Use");
    CHECK(cl.pch.header == "stdafx.h");
    CHECK(cl.exception_handling == "Sync");
}

TEST_CASE("VcprojReader flags MASM sources", "[vcproj_reader]") {
    TempVcproj temp(R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="9.00" Name="AsmProj" ProjectGUID="{00000000-0000-0000-0000-000000000002}">
	<Configurations>
		<Configuration Name="Debug|Win32" ConfigurationType="4"/>
	</Configurations>
	<Files>
		<File RelativePath=".\math\fastmath.asm"/>
		<File RelativePath=".\lib.c"/>
	</Files>
</VisualStudioProject>)XML");

    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    CHECK(proj.configurations["Debug|Win32"].config_type == "StaticLibrary");
    CHECK(proj.has_masm_files);
    const auto* asm_file = find_source(proj, "math\\fastmath.asm");
    REQUIRE(asm_file);
    CHECK(asm_file->type == FileType::MASM);
    CHECK(proj.language == "C");
}

TEST_CASE("VcprojReader inherits .vsprops property sheet settings", "[vcproj_reader]") {
    TempVcproj temp(R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="9.00" Name="SheetUser" ProjectGUID="{00000000-0000-0000-0000-000000000003}">
	<Configurations>
		<Configuration Name="Debug|Win32" ConfigurationType="1" InheritedPropertySheets=".\common.vsprops">
			<Tool Name="VCCLCompilerTool" PreprocessorDefinitions="LOCAL_DEFINE"/>
		</Configuration>
	</Configurations>
	<Files>
		<File RelativePath=".\main.cpp"/>
	</Files>
</VisualStudioProject>)XML");

    temp.write_file("common.vsprops", R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioPropertySheet ProjectType="Visual C++" Version="8.00" Name="Common">
	<Tool Name="VCCLCompilerTool" AdditionalIncludeDirectories="..\shared\include" PreprocessorDefinitions="SHARED_DEFINE"/>
	<Tool Name="VCLinkerTool" AdditionalDependencies="shared.lib"/>
</VisualStudioPropertySheet>)XML");

    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());

    auto& cfg = proj.configurations["Debug|Win32"];
    REQUIRE(cfg.cl_compile.preprocessor_definitions.size() == 2);
    CHECK(cfg.cl_compile.preprocessor_definitions[0] == "LOCAL_DEFINE");
    CHECK(cfg.cl_compile.preprocessor_definitions[1] == "SHARED_DEFINE");
    REQUIRE(cfg.cl_compile.additional_include_directories.size() == 1);
    CHECK(cfg.cl_compile.additional_include_directories[0] == "..\\shared\\include");
    REQUIRE(cfg.link.additional_dependencies.size() == 1);
    CHECK(cfg.link.additional_dependencies[0] == "shared.lib");
}

TEST_CASE("VcprojReader throws on invalid file", "[vcproj_reader]") {
    VcprojReader reader;
    CHECK_THROWS(reader.read_vcproj("nonexistent_file.vcproj"));
}

// ============================================================================
// SlnReader integration with .vcproj projects and tricky config names
// ============================================================================

// RAII temp dir holding a .sln plus project files
struct TempSln {
    fs::path temp_dir;
    fs::path sln_path;

    TempSln() {
        temp_dir = fs::temp_directory_path() / "sighmake_test_vcproj_sln";
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);
        sln_path = temp_dir / "Legacy.sln";
    }

    ~TempSln() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream f(temp_dir / filename);
        f << content;
    }
};

TEST_CASE("SlnReader loads .vcproj projects referenced by a VS2008 solution", "[vcproj_reader][sln]") {
    TempSln temp;
    temp.write_file("App.vcproj", R"XML(<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="9.00" Name="App" ProjectGUID="{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}">
	<Configurations>
		<Configuration Name="Debug|Win32" ConfigurationType="1">
			<Tool Name="VCLinkerTool" SubSystem="1"/>
		</Configuration>
	</Configurations>
	<Files>
		<File RelativePath=".\main.cpp"/>
	</Files>
</VisualStudioProject>)XML");

    temp.write_file("Legacy.sln",
        "Microsoft Visual Studio Solution File, Format Version 10.00\n"
        "# Visual Studio 2008\n"
        "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"App\", \"App.vcproj\", \"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\"\n"
        "EndProject\n"
        "Global\n"
        "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
        "\t\tDebug|Win32 = Debug|Win32\n"
        "\t\tRelease|Win32 = Release|Win32\n"
        "\tEndGlobalSection\n"
        "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
        "\t\t{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}.Debug|Win32.ActiveCfg = Debug|Win32\n"
        "\t\t{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}.Debug|Win32.Build.0 = Debug|Win32\n"
        "\tEndGlobalSection\n"
        "EndGlobal\n");

    SlnReader reader;
    auto solution = reader.read_sln(temp.sln_path.string());

    REQUIRE(solution.projects.size() == 1);
    const auto& proj = solution.projects[0];
    CHECK(proj.name == "App");
    CHECK(proj.vcxproj_path == "App.vcproj");
    REQUIRE(proj.configurations.count("Debug|Win32"));
    CHECK(proj.configurations.at("Debug|Win32").config_type == "Application");
    CHECK(proj.configurations.at("Debug|Win32").link.sub_system == "Console");
    REQUIRE(proj.sources.size() == 1);
    CHECK(proj.sources[0].path == "main.cpp");

    REQUIRE(solution.configurations.size() == 2);
    CHECK(solution.configurations[0] == "Debug");
    CHECK(solution.configurations[1] == "Release");
    REQUIRE(solution.platforms.size() == 1);
    CHECK(solution.platforms[0] == "Win32");
}

TEST_CASE("SlnReader parses configuration names containing spaces", "[vcproj_reader][sln]") {
    TempSln temp;
    temp.write_file("Legacy.sln",
        "Microsoft Visual Studio Solution File, Format Version 12.00\n"
        "# Visual Studio Version 17\n"
        "Global\n"
        "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
        "\t\tDebug DLL|Win32 = Debug DLL|Win32\n"
        "\t\tRelease DLL|Win32 = Release DLL|Win32\n"
        "\t\tDebug|Any CPU = Debug|Any CPU\n"
        "\t\tRelease|Mixed Platforms = Release|Mixed Platforms\n"
        "\tEndGlobalSection\n"
        "EndGlobal\n");

    SlnReader reader;
    auto solution = reader.read_sln(temp.sln_path.string());

    auto has_config = [&](const std::string& name) {
        return std::find(solution.configurations.begin(), solution.configurations.end(), name)
               != solution.configurations.end();
    };
    CHECK(has_config("Debug DLL"));
    CHECK(has_config("Release DLL"));
    CHECK(has_config("Debug"));
    CHECK(has_config("Release"));
    CHECK_FALSE(has_config("DLL")); // the old regex mangled "Debug DLL" into "DLL"

    // Pseudo-platforms contributed by C#/VB projects are skipped
    auto has_platform = [&](const std::string& name) {
        return std::find(solution.platforms.begin(), solution.platforms.end(), name)
               != solution.platforms.end();
    };
    CHECK(has_platform("Win32"));
    CHECK_FALSE(has_platform("Any CPU"));
    CHECK_FALSE(has_platform("Mixed Platforms"));
}

TEST_CASE("BuildscriptWriter converts a vcproj project end to end", "[vcproj_reader][buildscript_writer]") {
    TempVcproj temp(LEGACY_VCPROJ);
    VcprojReader reader;
    auto proj = reader.read_vcproj(temp.vcproj_path.string());
    proj.vcxproj_path = "test.vcproj";

    fs::path out_path = temp.temp_dir / "LegacyApp.buildscript";
    BuildscriptWriter writer;
    REQUIRE(writer.write_buildscript(proj, out_path.string()));

    std::ifstream f(out_path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    CHECK(content.find("[project:LegacyApp]") != std::string::npos);
    CHECK(content.find("type = exe") != std::string::npos);
    CHECK(content.find("src\\main.cpp") != std::string::npos);
    CHECK(content.find("subsystem = Console") != std::string::npos);
    CHECK(content.find("[config:Debug|Win32]") != std::string::npos);
    CHECK(content.find("[config:Release|Win32]") != std::string::npos);
}
