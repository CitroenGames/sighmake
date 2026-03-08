#include "pch.h"
#include "catch_amalgamated.hpp"
#include "generators/deps_exporter.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper to read file content
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// RAII temp dir for deps exporter tests
struct DepsExporterResult {
    fs::path temp_dir;
    std::string html_content;
    bool success = false;

    ~DepsExporterResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static DepsExporterResult export_deps(const Solution& solution) {
    DepsExporterResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_deps";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    result.success = export_dependencies_html(solution, result.temp_dir.string());

    if (result.success) {
        fs::path html_path = result.temp_dir / (solution.name + "_dependencies.html");
        if (fs::exists(html_path)) {
            result.html_content = read_file(html_path);
        }
    }

    return result;
}

// ============================================================================
// DepsExporter tests
// ============================================================================

TEST_CASE("DepsExporter creates HTML file", "[deps_exporter]") {
    Solution sol;
    sol.name = "TestSolution";
    sol.configurations = {"Debug", "Release"};
    sol.platforms = {"Win32"};

    Project proj;
    proj.name = "App";
    proj.configurations["Debug|Win32"].config_type = "Application";
    sol.projects.push_back(proj);

    auto result = export_deps(sol);
    CHECK(result.success);
    CHECK(!result.html_content.empty());
}

TEST_CASE("DepsExporter HTML contains project names", "[deps_exporter]") {
    Solution sol;
    sol.name = "TestSolution";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project app;
    app.name = "MyApp";
    app.configurations["Debug|Win32"].config_type = "Application";
    sol.projects.push_back(app);

    Project lib;
    lib.name = "MyLib";
    lib.configurations["Debug|Win32"].config_type = "StaticLibrary";
    sol.projects.push_back(lib);

    auto result = export_deps(sol);
    CHECK(result.html_content.find("MyApp") != std::string::npos);
    CHECK(result.html_content.find("MyLib") != std::string::npos);
}

TEST_CASE("DepsExporter HTML contains dependency relationships", "[deps_exporter]") {
    Solution sol;
    sol.name = "TestSolution";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project lib;
    lib.name = "CoreLib";
    lib.configurations["Debug|Win32"].config_type = "StaticLibrary";
    sol.projects.push_back(lib);

    Project app;
    app.name = "App";
    app.configurations["Debug|Win32"].config_type = "Application";
    app.project_references.push_back({"CoreLib", DependencyVisibility::PUBLIC});
    sol.projects.push_back(app);

    auto result = export_deps(sol);
    CHECK(result.html_content.find("CoreLib") != std::string::npos);
    CHECK(result.html_content.find("App") != std::string::npos);
}

TEST_CASE("DepsExporter HTML contains visibility tags", "[deps_exporter]") {
    Solution sol;
    sol.name = "VisSolution";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project lib;
    lib.name = "Lib";
    lib.configurations["Debug|Win32"].config_type = "StaticLibrary";
    sol.projects.push_back(lib);

    Project app;
    app.name = "App";
    app.configurations["Debug|Win32"].config_type = "Application";
    app.project_references.push_back({"Lib", DependencyVisibility::PUBLIC});
    sol.projects.push_back(app);

    auto result = export_deps(sol);
    CHECK(result.html_content.find("PUBLIC") != std::string::npos);
}

TEST_CASE("DepsExporter with single project omits matrix", "[deps_exporter]") {
    Solution sol;
    sol.name = "SingleProject";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project proj;
    proj.name = "OnlyApp";
    proj.configurations["Debug|Win32"].config_type = "Application";
    sol.projects.push_back(proj);

    auto result = export_deps(sol);
    CHECK(result.success);
    // Matrix header should not be present for single project
    CHECK(result.html_content.find("Dependency Matrix") == std::string::npos);
}

TEST_CASE("DepsExporter with multiple projects includes matrix", "[deps_exporter]") {
    Solution sol;
    sol.name = "MultiProject";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project lib;
    lib.name = "Lib";
    lib.configurations["Debug|Win32"].config_type = "StaticLibrary";
    sol.projects.push_back(lib);

    Project app;
    app.name = "App";
    app.configurations["Debug|Win32"].config_type = "Application";
    app.project_references.push_back({"Lib", DependencyVisibility::PRIVATE});
    sol.projects.push_back(app);

    auto result = export_deps(sol);
    CHECK(result.html_content.find("Dependency Matrix") != std::string::npos);
    CHECK(result.html_content.find("dep-matrix") != std::string::npos);
}

TEST_CASE("DepsExporter handles empty solution", "[deps_exporter]") {
    Solution sol;
    sol.name = "EmptySolution";

    auto result = export_deps(sol);
    CHECK(result.success);
    CHECK(result.html_content.find("EmptySolution") != std::string::npos);
    CHECK(result.html_content.find("0 projects") != std::string::npos);
}

TEST_CASE("DepsExporter HTML escapes special characters", "[deps_exporter]") {
    Solution sol;
    sol.name = "Test&Solution";
    sol.configurations = {"Debug"};
    sol.platforms = {"Win32"};

    Project proj;
    proj.name = "App<1>";
    proj.configurations["Debug|Win32"].config_type = "Application";
    sol.projects.push_back(proj);

    auto result = export_deps(sol);
    // Special characters should be escaped in HTML
    CHECK(result.html_content.find("&amp;") != std::string::npos);
    CHECK(result.html_content.find("&lt;") != std::string::npos);
    CHECK(result.html_content.find("&gt;") != std::string::npos);
}
