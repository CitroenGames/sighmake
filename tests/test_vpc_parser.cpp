#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/vpc_parser.hpp"

using namespace vcxproj;

// Helper to find a project by name
static const Project* find_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

// Helper to check if a vector contains a string
static bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

// Helper to check if any string in a vector contains a substring
static bool contains_substring(const std::vector<std::string>& vec, const std::string& sub) {
    for (const auto& s : vec) {
        if (s.find(sub) != std::string::npos) return true;
    }
    return false;
}

// ============================================================================
// Macro Resolution
// ============================================================================

TEST_CASE("VPC $Macro defines and resolves macros", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Macro PROJNAME "MyApp"
$Project "$PROJNAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "MyApp");
}

TEST_CASE("VPC nested macro resolution", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Macro BASE "Hello"
$Macro FULL "$BASE_World"
$Project "$FULL"
{
}
)");
    // $BASE_WORLD won't resolve because the macro name is BASE_WORLD not BASE
    // This tests the macro resolution boundary behavior
    REQUIRE(sol.projects.size() == 1);
}

TEST_CASE("VPC $Macro with condition", "[vpc_parser]") {
    VpcParser parser;
    // Default target platforms are WIN32, WIN64
    auto sol = parser.parse_string(R"(
$Macro WINNAME "WinApp" [$WIN32]
$Macro LINUXNAME "LinuxApp" [$LINUX64]
$Project "$WINNAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "WinApp");
}

TEST_CASE("VPC default macros SRCDIR and QUOTE are set", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "TestApp"
{
    $Folder "Source"
    {
    }
}
)");
    // Should not crash - SRCDIR and QUOTE macros are used internally
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "TestApp");
}

TEST_CASE("VPC $MacroRequired is silently skipped", "[vpc_parser]") {
    VpcParser parser;
    REQUIRE_NOTHROW(parser.parse_string(R"(
$MacroRequired SOME_MACRO
$MacroRequiredAllowEmpty ANOTHER_MACRO
$Project "App"
{
}
)"));
}

// ============================================================================
// Conditional Evaluation
// ============================================================================

TEST_CASE("VPC $Conditional sets bool value", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Conditional MYFEATURE "1"
$Macro APPNAME "FeatureApp" [$MYFEATURE]
$Project "$APPNAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "FeatureApp");
}

TEST_CASE("VPC $WIN32 is true by default", "[vpc_parser]") {
    VpcParser parser;
    // Default target platforms include WIN32
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Folder "Source" [$WIN32]
    {
    }
}
)");
    // Should not skip the folder
    REQUIRE(sol.projects.size() == 1);
}

TEST_CASE("VPC evaluate_condition with negation", "[vpc_parser]") {
    VpcParser parser;
    // !$LINUX should be true since LINUX is not in default target platforms
    auto sol = parser.parse_string(R"(
$Macro NAME "NotLinux" [!$LINUXALL]
$Project "$NAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "NotLinux");
}

TEST_CASE("VPC evaluate_condition with OR operator", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Macro NAME "OrTest" [$WIN32 || $LINUX64]
$Project "$NAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "OrTest");
}

TEST_CASE("VPC evaluate_condition with AND operator", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Macro NAME "AndTest" [$WIN32 && $WINDOWS]
$Project "$NAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "AndTest");
}

TEST_CASE("VPC false condition skips macro", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Macro NAME "Default"
$Macro NAME "Override" [$LINUX64]
$Project "$NAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "Default");
}

// ============================================================================
// $Project Handling
// ============================================================================

TEST_CASE("VPC $Project creates project with name", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "MyApp"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "MyApp");
}

TEST_CASE("VPC $Project with false condition skips project block", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "LinuxOnly" [$LINUX64]
{
}
$Project "WinApp"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "WinApp");
}

TEST_CASE("VPC multiple projects", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "AppA"
{
}
$Project "AppB"
{
}
)");
    REQUIRE(sol.projects.size() == 2);
    CHECK(find_project(sol, "AppA") != nullptr);
    CHECK(find_project(sol, "AppB") != nullptr);
}

TEST_CASE("VPC duplicate project name reuses project", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
}
$Project "App"
{
}
)");
    // Should merge into one project, not create two
    CHECK(sol.projects.size() == 1);
}

TEST_CASE("VPC solution name defaults to first project name", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "FirstProject"
{
}
)");
    CHECK(sol.name == "FirstProject");
}

// ============================================================================
// $Folder and $File Handling
// ============================================================================

TEST_CASE("VPC $File adds source to project", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $File "main.cpp"
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].sources.size() >= 1);
}

TEST_CASE("VPC $File with false condition is skipped", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $File "main.cpp"
    $File "linux_only.cpp" [$LINUX64]
}
)");
    REQUIRE(sol.projects.size() == 1);
    // Only main.cpp should be added, linux_only.cpp condition is false
    CHECK(sol.projects[0].sources.size() == 1);
}

TEST_CASE("VPC $Folder creates folder context", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Folder "Source"
    {
        $File "main.cpp"
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].sources.size() >= 1);
}

TEST_CASE("VPC nested folders", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Folder "Source"
    {
        $Folder "Core"
        {
            $File "engine.cpp"
        }
        $File "main.cpp"
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].sources.size() >= 2);
}

TEST_CASE("VPC $Folder with false condition skips contents", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $File "main.cpp"
    $Folder "LinuxSource" [$LINUX64]
    {
        $File "linux_main.cpp"
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].sources.size() == 1);
}

// ============================================================================
// $Configuration, $Compiler, $Linker
// ============================================================================

TEST_CASE("VPC $Configuration creates config", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Debug"
    {
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    // After finalization, should have configurations
    CHECK(!sol.projects[0].configurations.empty());
}

TEST_CASE("VPC $Compiler $PreprocessorDefinitions adds defines", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration
    {
        $Compiler
        {
            $PreprocessorDefinitions "WIN32;_DEBUG;_CONSOLE"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    // Check that at least one config has the defines
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (contains(cfg.cl_compile.preprocessor_definitions, "WIN32") ||
            contains(cfg.cl_compile.preprocessor_definitions, "_DEBUG")) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("VPC $Compiler $AdditionalIncludeDirectories adds includes", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration
    {
        $Compiler
        {
            $AdditionalIncludeDirectories "src;include"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (contains_substring(cfg.cl_compile.additional_include_directories, "src") ||
            contains_substring(cfg.cl_compile.additional_include_directories, "include")) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("VPC $Compiler $Optimization sets optimization", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Debug"
    {
        $Compiler
        {
            $Optimization "Disabled"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.cl_compile.optimization == "Disabled") found = true;
    }
    CHECK(found);
}

TEST_CASE("VPC $Linker $SubSystem sets subsystem", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration
    {
        $Linker
        {
            $SubSystem "Console"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.link.sub_system == "Console") found = true;
    }
    CHECK(found);
}

TEST_CASE("VPC $Linker $AdditionalDependencies adds libs", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration
    {
        $Linker
        {
            $AdditionalDependencies "kernel32.lib user32.lib"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (contains(cfg.link.additional_dependencies, "kernel32.lib") ||
            contains(cfg.link.additional_dependencies, "user32.lib")) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("VPC $PreBuildEvent sets command", "[vpc_parser]") {
    VpcParser parser;
    // Build events are applied to existing configs, so define compiler first
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Debug"
    {
        $Compiler
        {
            $Optimization "Disabled"
        }
        $PreBuildEvent
        {
            $CommandLine "echo prebuild"
            $Description "Running prebuild"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.pre_build_event.command == "echo prebuild") found = true;
    }
    CHECK(found);
}

TEST_CASE("VPC $PostBuildEvent sets command", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Debug"
    {
        $Compiler
        {
            $Optimization "Disabled"
        }
        $PostBuildEvent
        {
            $CommandLine "echo postbuild"
            $Description "Running postbuild"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.post_build_event.command == "echo postbuild") found = true;
    }
    CHECK(found);
}

// ============================================================================
// Finalization
// ============================================================================

TEST_CASE("VPC finalize_solution sets default configurations", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
}
)");
    CHECK(sol.configurations.size() >= 2);
    CHECK(contains(sol.configurations, "Debug"));
    CHECK(contains(sol.configurations, "Release"));
}

TEST_CASE("VPC finalize_solution sets default platforms", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
}
)");
    CHECK(sol.platforms.size() >= 2);
    CHECK(contains(sol.platforms, "Win32"));
    CHECK(contains(sol.platforms, "x64"));
}

TEST_CASE("VPC finalize_solution sets default toolset", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
}
)");
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        CHECK(cfg.platform_toolset == "v143");
    }
}

TEST_CASE("VPC finalize sets Debug optimization to Disabled", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
}
)");
    CHECK(sol.projects[0].configurations["Debug|Win32"].cl_compile.optimization == "Disabled");
    CHECK(sol.projects[0].configurations["Release|Win32"].cl_compile.optimization == "MaxSpeed");
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("VPC empty input produces empty solution", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string("");
    CHECK(sol.projects.empty());
}

TEST_CASE("VPC comments are ignored", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
// This is a comment
$Project "App" // inline comment
{
    /* Block comment */
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "App");
}

TEST_CASE("VPC unknown keywords are skipped gracefully", "[vpc_parser]") {
    VpcParser parser;
    REQUIRE_NOTHROW(parser.parse_string(R"(
$UnknownKeyword "value"
$Project "App"
{
    $UnknownBlock
    {
    }
}
)"));
}

TEST_CASE("VPC set_target_platforms changes platform conditionals", "[vpc_parser]") {
    VpcParser parser;
    parser.set_target_platforms({"LINUX64"});
    auto sol = parser.parse_string(R"(
$Macro NAME "LinuxApp" [$LINUX64]
$Project "$NAME"
{
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].name == "LinuxApp");
}

TEST_CASE("VPC $Compiler $RuntimeLibrary sets runtime library", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration "Debug"
    {
        $Compiler
        {
            $RuntimeLibrary "MultiThreadedDebugDLL"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.cl_compile.runtime_library == "MultiThreadedDebugDLL") found = true;
    }
    CHECK(found);
}

TEST_CASE("VPC $Compiler $ExceptionHandling sets exception handling", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Configuration
    {
        $Compiler
        {
            $ExceptionHandling "Sync"
        }
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    bool found = false;
    for (const auto& [key, cfg] : sol.projects[0].configurations) {
        if (cfg.cl_compile.exception_handling == "Sync") found = true;
    }
    CHECK(found);
}

TEST_CASE("VPC $Lib adds library to project", "[vpc_parser]") {
    VpcParser parser;
    auto sol = parser.parse_string(R"(
$Project "App"
{
    $Folder "Link Libraries"
    {
        $Lib "tier0"
    }
}
)");
    REQUIRE(sol.projects.size() == 1);
    CHECK(sol.projects[0].libraries.size() >= 1);
}
