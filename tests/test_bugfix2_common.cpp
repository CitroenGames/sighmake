#include "pch.h"
#include "catch_amalgamated.hpp"
#include "common/build_runner.hpp"
#include "common/build_cache.hpp"
#include "common/output_manifest.hpp"
#include "common/target_os.hpp"
#include "generators/deps_exporter.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/cmake_generator.hpp"
#include "generators/vcxproj_generator.hpp"
#include "parsers/buildscript_parser.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

struct Bugfix2TempDir {
    fs::path path;
    explicit Bugfix2TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
        std::ofstream(path / "main.cpp") << "int main() { return 0; }\n";
    }
    ~Bugfix2TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Bugfix2TargetOs {
    std::optional<TargetOS> previous = target_os_override();
    explicit Bugfix2TargetOs(TargetOS os) { set_target_os(os); }
    ~Bugfix2TargetOs() { set_target_os(previous); }
};

std::string read_text2(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

struct GenRun {
    StaleOutputReport report;
    std::string out;
    std::string err;
};

GenRun generate_again(Generator& generator, const fs::path& dir, const std::string& script,
                      StaleOutputOptions options = {}) {
    BuildscriptParser parser;
    Solution solution = parser.parse_string(script, dir.string());
    reset_generated_outputs();
    REQUIRE(generator.generate(solution, dir.string()));
    auto scope = generator.output_manifest_scope(dir.string());
    REQUIRE(scope.has_value());
    std::ostringstream out, err;
    GenRun run;
    run.report = reconcile_generated_outputs(*scope, generated_outputs(), options, out, err);
    run.out = out.str();
    run.err = err.str();
    return run;
}

std::string two_libs(bool second) {
    std::string text = "[solution]\nname = S\nconfigurations = Debug, Release\nplatforms = Linux\n\n"
                       "[project:Core]\ntype = lib\nsources = main.cpp\n";
    if (second) text += "\n[project:Gone]\ntype = lib\nsources = main.cpp\n";
    return text;
}

#ifndef _WIN32
struct Env2Guard {
    std::string name;
    std::optional<std::string> previous;
    Env2Guard(const std::string& n, const std::string& value) : name(n) {
        if (const char* old = std::getenv(name.c_str())) previous = old;
        setenv(name.c_str(), value.c_str(), 1);
    }
    ~Env2Guard() {
        if (previous) setenv(name.c_str(), previous->c_str(), 1);
        else unsetenv(name.c_str());
    }
};

void write_logging_tool2(const fs::path& script, const fs::path& log) {
    std::ofstream(script) << "#!/bin/sh\necho \"$@\" >> \"" << log.string() << "\"\nexit 0\n";
    fs::permissions(script, fs::perms::owner_all, fs::perm_options::add);
}
#endif

} // namespace

TEST_CASE("Stale outputs: a generated file the user replaced is not deleted", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_replaced");
    Bugfix2TargetOs os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";

    generate_again(generator, dir.path, two_libs(true));
    REQUIRE(fs::exists(build / "Gone.Release"));
    // The user replaced the generated project Makefile with their own (no sighmake header).
    std::ofstream(build / "Gone.Release", std::ios::trunc) << "all:\n\techo mine\n";

    auto second = generate_again(generator, dir.path, two_libs(false));
    CHECK_FALSE(has(second.report.removed, "Gone.Release"));
    CHECK(has(second.report.removed, "Gone.Debug"));
    CHECK(fs::exists(build / "Gone.Release"));
    CHECK(read_text2(build / "Gone.Release") == "all:\n\techo mine\n");
    CHECK_FALSE(fs::exists(build / "Gone.Debug"));
    CHECK(second.err.find("Gone.Release") != std::string::npos);
    // The kept file is no longer claimed by the manifest.
    CHECK(read_text2(build / kOutputManifestFilename).find("Gone.Release") == std::string::npos);
}

TEST_CASE("Stale outputs: a header-less generated file edited by the user is not deleted", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_edited_vcxproj");
    Bugfix2TargetOs os(TargetOS::Windows);
    const fs::path build = dir.path / "build";
    fs::create_directories(build);
    const fs::path file = build / "Gone.vcxproj";
    std::ofstream(file) << "<Project>generated</Project>\n";

    // Manifest written by this sighmake version: records a content hash for the file.
    OutputManifestScope scope;
    scope.directory = build;
    scope.key = "vcxproj windows";
    std::ostringstream out, err;
    reconcile_generated_outputs(scope, {file}, {}, out, err);
    CHECK(read_text2(build / kOutputManifestFilename).find("Gone.vcxproj") != std::string::npos);

    std::ofstream(file, std::ios::trunc) << "<Project>hand written</Project>\n";
    auto report = reconcile_generated_outputs(scope, {}, {}, out, err);
    CHECK(fs::exists(file));
    CHECK_FALSE(has(report.removed, "Gone.vcxproj"));

    // An unmodified file is still removed.
    std::ofstream(build / "Other.vcxproj") << "<Project>x</Project>\n";
    reconcile_generated_outputs(scope, {build / "Other.vcxproj"}, {}, out, err);
    auto removed = reconcile_generated_outputs(scope, {}, {}, out, err);
    CHECK(has(removed.removed, "Other.vcxproj"));
    CHECK_FALSE(fs::exists(build / "Other.vcxproj"));
}

TEST_CASE("Stale outputs: an old manifest without hashes never deletes a user's header-less file", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_legacy");
    const fs::path build = dir.path;
    std::ofstream(build / kOutputManifestFilename)
        << "# sighmake generation manifest\n[makefile linux]\nMine.Release\nOurs.Release\nCMakeLists.txt\n";
    std::ofstream(build / "Mine.Release") << "all:\n\techo mine\n";
    std::ofstream(build / "Ours.Release") << "# Generated by sighmake\n\nall:\n";
    std::ofstream(build / "CMakeLists.txt") << "notes\n";

    OutputManifestScope scope;
    scope.directory = build;
    scope.key = "makefile linux";
    std::ostringstream out, err;
    auto report = reconcile_generated_outputs(scope, {}, {}, out, err);
    CHECK(fs::exists(build / "Mine.Release"));
    CHECK(fs::exists(build / "CMakeLists.txt"));
    CHECK_FALSE(fs::exists(build / "Ours.Release"));
    CHECK(has(report.removed, "Ours.Release"));
}

TEST_CASE("--prune-unlisted keeps a hand-written solution in the -B . directory", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_prune_sln");
    Bugfix2TargetOs os(TargetOS::Windows);
    VcxprojGenerator generator;
    // With "-B ." the manifest directory is the project root itself.
    generator.set_build_dir(".");
    const std::string handwritten =
        "\xEF\xBB\xBF\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n"
        "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"Mine\", \"Mine.vcxproj\", \"{1}\"\r\nEndProject\r\n"
        "Global\r\n\tGlobalSection(ExtensibilityGlobals) = postSolution\r\n\t\tSolutionGuid = {2}\r\n"
        "\tEndGlobalSection\r\nEndGlobal\r\n";
    const std::string handxml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<Project DefaultTargets=\"Build\" "
                                "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"/>\r\n";
    for (const char* suffix : {"_.sln", ".sln", "_.slnx", "_.vcxproj", ".vcxproj"}) {
        std::ofstream(dir.path / (std::string("Mine") + suffix), std::ios::binary)
            << (std::string(suffix).find("sln") != std::string::npos && std::string(suffix) != "_.slnx" ? handwritten : handxml);
    }
    auto scope = generator.output_manifest_scope(dir.path.string());
    REQUIRE(scope.has_value());
    StaleOutputOptions options;
    options.prune_unlisted = true;
    std::ostringstream out, err;
    reconcile_generated_outputs(*scope, {}, options, out, err);
    for (const char* suffix : {"_.sln", ".sln", "_.slnx", "_.vcxproj", ".vcxproj"}) {
        INFO(suffix);
        CHECK(fs::exists(dir.path / (std::string("Mine") + suffix)));
    }
}

TEST_CASE("--prune-unlisted still removes leftover sighmake-looking project files", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_prune_old");
    Bugfix2TargetOs os(TargetOS::Windows);
    VcxprojGenerator generator;
    std::ofstream(dir.path / "Old_.vcxproj", std::ios::binary)
        << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Project DefaultTargets=\"Build\" ToolsVersion=\"4.0\" "
           "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n</Project>\n";
    std::ofstream(dir.path / "Old_.sln", std::ios::binary)
        << "\xEF\xBB\xBF\nMicrosoft Visual Studio Solution File, Format Version 12.00\n"
           "Global\n\tGlobalSection(SolutionProperties) = preSolution\n\t\tHideSolutionNode = FALSE\n"
           "\tEndGlobalSection\nEndGlobal\n";
    auto scope = generator.output_manifest_scope(dir.path.string());
    REQUIRE(scope.has_value());
    scope->directory = dir.path;
    StaleOutputOptions options;
    options.prune_unlisted = true;
    std::ostringstream out, err;
    auto report = reconcile_generated_outputs(*scope, {}, options, out, err);
    CHECK_FALSE(fs::exists(dir.path / "Old_.vcxproj"));
    CHECK_FALSE(fs::exists(dir.path / "Old_.sln"));
    CHECK(report.unlisted_removed.size() == 2);
}

TEST_CASE("--export-deps omits Android-only edges for a mixed desktop and Android solution", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_deps_android");
    // The makefile generator does not prune platform-scoped declarations, so the exporter sees them.
    BuildscriptParser parser;
    Solution sol = parser.parse_string(R"(
[solution]
name = Demo
configurations = Release
platforms = Linux, Android

[project:Common]
type = lib
sources = main.cpp

[project:Headers]
type = lib
sources = main.cpp

[project:Launcher]
type = exe
sources = main.cpp
target_link_libraries(
    Common PUBLIC
)
if(Android) {
    dependencies = Headers
}
)", dir.path.string());
    REQUIRE(export_dependencies_html(sol, dir.path.string()));
    const std::string html = read_text2(dir.path / "Demo_dependencies.html");
    CHECK(html.find("<li>Common ") != std::string::npos);
    CHECK(html.find("<li>Headers ") == std::string::npos);
    CHECK(html.find("3 projects, 1 dependencies") != std::string::npos);
}

TEST_CASE("--export-deps keeps Android edges of an Android-only solution", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_deps_android_only");
    BuildscriptParser parser;
    Solution sol = parser.parse_string(R"(
[solution]
name = Demo
configurations = Release
platforms = Android

[project:Headers]
type = lib
sources = main.cpp

[project:Launcher]
type = exe
sources = main.cpp
if(Android) {
    dependencies = Headers
}
)", dir.path.string());
    REQUIRE(export_dependencies_html(sol, dir.path.string()));
    const std::string html = read_text2(dir.path / "Demo_dependencies.html");
    CHECK(html.find("<li>Headers ") != std::string::npos);
}

#ifndef _WIN32
namespace {
struct MakeFixture {
    Bugfix2TempDir dir;
    fs::path log;
    fs::path root;
    std::unique_ptr<Env2Guard> make_env;
    explicit MakeFixture(const std::string& name) : dir(name) {
        log = dir.path / "tool.log";
        write_logging_tool2(dir.path / "faketool", log);
        root = dir.path / "root";
        fs::create_directories(root / "build");
        std::ofstream(root / "build" / "Makefile") << "# Generated by sighmake\n";
        make_env = std::make_unique<Env2Guard>("SIGHMAKE_MAKE", (dir.path / "faketool").string());
    }
    BuildCache make_cache(std::vector<std::string> platforms) const {
        BuildCache cache;
        cache.generator = "makefile";
        cache.solution_name = "Demo";
        cache.configurations = {"Debug", "Release"};
        cache.platforms = std::move(platforms);
        cache.build_dir = "build";
        REQUIRE(cache.write(root.string()));
        return cache;
    }
};

struct CerrCapture {
    std::ostringstream text;
    std::streambuf* previous;
    CerrCapture() : previous(std::cerr.rdbuf(text.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(previous); }
};
} // namespace

TEST_CASE("Make --platform accepts any desktop platform name and builds the host targets", "[bugfix2_common]") {
    MakeFixture fx("sighmake_bugfix2_common_make_platform");
    fx.make_cache({"Linux"});
    BuildOptions options;
    options.directory = fx.root.string();
    options.platform = "x64";
    options.config = "Release";
    CerrCapture cerr_capture;
    CHECK(BuildRunner::run(options) == 0);
    const std::string calls = read_text2(fx.log);
    CHECK(calls.find("Release") != std::string::npos);
    CHECK(calls.find(".Android") == std::string::npos);
}

TEST_CASE("Make --project together with --target is rejected instead of dropping the target", "[bugfix2_common]") {
    MakeFixture fx("sighmake_bugfix2_common_make_project_target");
    fx.make_cache({"Linux"});
    BuildOptions options;
    options.directory = fx.root.string();
    options.project = "App";
    options.target = "tests";
    CerrCapture cerr_capture;
    CHECK(BuildRunner::run(options) != 0);
    CHECK(cerr_capture.text.str().find("--target") != std::string::npos);
    CHECK_FALSE(fs::exists(fx.log));
}

TEST_CASE("CMake --project together with --target is rejected instead of dropping the target", "[bugfix2_common]") {
    Bugfix2TempDir dir("sighmake_bugfix2_common_cmake_project_target");
    const fs::path bin = dir.path / "bin";
    const fs::path log = dir.path / "cmake.log";
    fs::create_directories(bin);
    write_logging_tool2(bin / "cmake", log);
    const fs::path root = dir.path / "root";
    fs::create_directories(root / "build");
    std::ofstream(root / "build" / "CMakeCache.txt") << "\n";
    BuildCache cache;
    cache.generator = "cmake";
    cache.solution_name = "Demo";
    cache.configurations = {"Debug", "Release"};
    cache.build_dir = "build";
    REQUIRE(cache.write(root.string()));
    Env2Guard path("PATH", bin.string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : ""));
    BuildOptions options;
    options.directory = root.string();
    options.project = "App";
    options.target = "tests";
    CerrCapture cerr_capture;
    CHECK(BuildRunner::run(options) != 0);
    CHECK(cerr_capture.text.str().find("--target") != std::string::npos);
    CHECK_FALSE(fs::exists(log));
}

TEST_CASE("Make --config with an explicit --target warns that the config has no effect", "[bugfix2_common]") {
    MakeFixture fx("sighmake_bugfix2_common_make_config_target");
    fx.make_cache({"Linux"});
    BuildOptions options;
    options.directory = fx.root.string();
    options.config = "Release";
    options.target = "Debug";
    CerrCapture cerr_capture;
    CHECK(BuildRunner::run(options) == 0);
    CHECK(cerr_capture.text.str().find("--config") != std::string::npos);
    CHECK(read_text2(fx.log).find("Debug") != std::string::npos);
}
#endif

TEST_CASE("MSBuild --clean-first is kept when --target is given", "[bugfix2_common]") {
    BuildOptions options;
    options.target = "Foo";
    CHECK(msbuild_target_switch(options) == "/t:Foo");
    options.clean_first = true;
    CHECK(msbuild_target_switch(options) == "/t:Clean;Foo");
    options.target.clear();
    CHECK(msbuild_target_switch(options) == "/t:Rebuild");
    options.clean_only = true;
    CHECK(msbuild_target_switch(options) == "/t:Clean");
}
