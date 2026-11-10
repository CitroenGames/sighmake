#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "common/output_manifest.hpp"
#include "common/target_os.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/cmake_generator.hpp"
#include "generators/vcxproj_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
        std::ofstream(path / "main.cpp") << "int main() { return 0; }\n";
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Pins the target OS for the test and restores the previous override afterwards.
struct TargetOsGuard {
    std::optional<TargetOS> previous = target_os_override();
    explicit TargetOsGuard(TargetOS os) { set_target_os(os); }
    ~TargetOsGuard() { set_target_os(previous); }
};

struct Run {
    StaleOutputReport report;
    std::string out;
    std::string err;
};

// One generation the way main() runs it: reset, generate, reconcile with the previous manifest.
Run generate_into(Generator& generator, const fs::path& dir, const std::string& buildscript,
                  StaleOutputOptions options = {}) {
    BuildscriptParser parser;
    Solution solution = parser.parse_string(buildscript, dir.string());
    reset_generated_outputs();
    REQUIRE(generator.generate(solution, dir.string()));
    auto scope = generator.output_manifest_scope(dir.string());
    REQUIRE(scope.has_value());
    std::ostringstream out, err;
    Run run;
    run.report = reconcile_generated_outputs(*scope, generated_outputs(), options, out, err);
    run.out = out.str();
    run.err = err.str();
    return run;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::string two_projects(const char* first, const char* second) {
    std::string text = R"(
[solution]
name = Stale
configurations = Debug, Release
platforms = Linux

[project:)";
    text += first;
    text += R"(]
type = lib
sources = main.cpp
)";
    if (second) {
        text += "\n[project:";
        text += second;
        text += "]\ntype = lib\nsources = main.cpp\n";
    }
    return text;
}

} // namespace

TEST_CASE("Stale outputs: a removed project's makefiles are deleted", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_removed");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";

    auto first = generate_into(generator, dir.path, two_projects("Core", "Gone"));
    CHECK_FALSE(first.report.had_previous_manifest);
    CHECK(first.report.removed.empty());
    REQUIRE(fs::exists(build / "Gone.Release"));
    REQUIRE(fs::exists(build / "Gone.Debug"));
    REQUIRE(fs::exists(build / kOutputManifestFilename));
    const std::string manifest = read_file(build / kOutputManifestFilename);
    CHECK(manifest.find("[makefile linux]") != std::string::npos);
    CHECK(manifest.find("\nGone.Release\n") != std::string::npos);
    CHECK(manifest.find("\nMakefile\n") != std::string::npos);

    auto second = generate_into(generator, dir.path, two_projects("Core", nullptr));
    CHECK(second.report.had_previous_manifest);
    CHECK(contains(second.report.removed, "Gone.Release"));
    CHECK(contains(second.report.removed, "Gone.Debug"));
    CHECK(second.report.removed.size() == 2);
    CHECK_FALSE(fs::exists(build / "Gone.Release"));
    CHECK_FALSE(fs::exists(build / "Gone.Debug"));
    CHECK(fs::exists(build / "Core.Release"));
    CHECK(fs::exists(build / "Makefile"));
    CHECK(second.out.find("Stale outputs: 2 removed, 0 listed") != std::string::npos);
    CHECK(read_file(build / kOutputManifestFilename).find("Gone.") == std::string::npos);
}

TEST_CASE("Stale outputs: a renamed project's old makefiles are deleted", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_renamed");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";

    generate_into(generator, dir.path, two_projects("OldName", nullptr));
    REQUIRE(fs::exists(build / "OldName.Release"));

    auto second = generate_into(generator, dir.path, two_projects("NewName", nullptr));
    CHECK(contains(second.report.removed, "OldName.Release"));
    CHECK(contains(second.report.removed, "OldName.Debug"));
    CHECK_FALSE(fs::exists(build / "OldName.Release"));
    CHECK(fs::exists(build / "NewName.Release"));
    CHECK(fs::exists(build / "NewName.Debug"));
}

TEST_CASE("Stale outputs: a project that no longer applies to the target is deleted", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_platform");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";
    const std::string before = R"(
[solution]
name = Stale
configurations = Release
platforms = x64, Linux, Android

[project:Core]
type = lib
sources = main.cpp

[project:DesktopTool]
type = lib
sources = main.cpp
)";
    // DesktopTool becomes Windows-only, Core drops Android.
    const std::string after = R"(
[solution]
name = Stale
configurations = Release
platforms = x64, Linux, Android

[project:Core]
type = lib
sources = main.cpp
platforms = !Android

[project:DesktopTool]
type = lib
sources = main.cpp
platforms = Windows
)";
    generate_into(generator, dir.path, before);
    REQUIRE(fs::exists(build / "DesktopTool.Release"));
    REQUIRE(fs::exists(build / "DesktopTool.Release.Android"));
    REQUIRE(fs::exists(build / "Core.Release.Android"));

    auto second = generate_into(generator, dir.path, after);
    CHECK(contains(second.report.removed, "DesktopTool.Release"));
    CHECK(contains(second.report.removed, "DesktopTool.Release.Android"));
    CHECK(contains(second.report.removed, "Core.Release.Android"));
    CHECK_FALSE(fs::exists(build / "DesktopTool.Release"));
    CHECK_FALSE(fs::exists(build / "Core.Release.Android"));
    CHECK(fs::exists(build / "Core.Release"));
}

TEST_CASE("Stale outputs: hand-written files in the build directory are left alone", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_handwritten");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";
    fs::create_directories(build);
    std::ofstream(build / "notes.txt") << "kept\n";
    std::ofstream(build / "Custom.Release") << "# hand-written makefile\nall:\n";
    std::ofstream(build / "tool.sh") << "#!/bin/sh\n";

    auto first = generate_into(generator, dir.path, two_projects("Core", "Gone"));
    CHECK(first.report.unlisted.empty());
    auto second = generate_into(generator, dir.path, two_projects("Core", nullptr), {false, true});
    CHECK(second.report.unlisted_removed.empty());
    CHECK(fs::exists(build / "notes.txt"));
    CHECK(fs::exists(build / "Custom.Release"));
    CHECK(fs::exists(build / "tool.sh"));
    CHECK_FALSE(fs::exists(build / "Gone.Release"));
}

TEST_CASE("Stale outputs: the first generation only lists probable stale outputs", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_first_run");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";
    // Outputs of an older sighmake (no manifest): a makefile of a project that no longer exists.
    fs::create_directories(build);
    std::ofstream(build / "Ghost.Release") << "# Auto-generated Makefile for Ghost (Release)\n# Generated by sighmake\n\n";
    std::ofstream(build / "Ghost.runtime-dependencies.txt") << "";
    std::ofstream(build / "hand.txt") << "mine\n";

    auto first = generate_into(generator, dir.path, two_projects("Core", nullptr));
    CHECK_FALSE(first.report.had_previous_manifest);
    CHECK(first.report.removed.empty());
    CHECK(contains(first.report.unlisted, "Ghost.Release"));
    CHECK(contains(first.report.unlisted, "Ghost.runtime-dependencies.txt"));
    CHECK_FALSE(contains(first.report.unlisted, "hand.txt"));
    CHECK_FALSE(contains(first.report.unlisted, "Core.Release"));
    CHECK(fs::exists(build / "Ghost.Release"));
    CHECK(first.err.find("Ghost.Release") != std::string::npos);
    CHECK(first.err.find("--prune-unlisted") != std::string::npos);
    CHECK(first.out.find("Stale outputs: 0 removed, 2 listed") != std::string::npos);
    // The unlisted files are not adopted into the manifest.
    CHECK(read_file(build / kOutputManifestFilename).find("Ghost") == std::string::npos);

    // Later runs only count them ...
    auto second = generate_into(generator, dir.path, two_projects("Core", nullptr));
    CHECK(second.report.unlisted.size() == 2);
    CHECK(second.err.find("Ghost.Release") == std::string::npos);
    CHECK(fs::exists(build / "Ghost.Release"));

    // ... and --prune-unlisted removes them, never the hand-written file.
    auto third = generate_into(generator, dir.path, two_projects("Core", nullptr), {false, true});
    CHECK(contains(third.report.unlisted_removed, "Ghost.Release"));
    CHECK(contains(third.report.unlisted_removed, "Ghost.runtime-dependencies.txt"));
    CHECK_FALSE(fs::exists(build / "Ghost.Release"));
    CHECK(fs::exists(build / "hand.txt"));
    CHECK(fs::exists(build / "Core.Release"));
}

TEST_CASE("Stale outputs: --keep-stale keeps and still tracks stale outputs", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_keep");
    TargetOsGuard os(TargetOS::Linux);
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";

    generate_into(generator, dir.path, two_projects("Core", "Gone"));
    auto kept = generate_into(generator, dir.path, two_projects("Core", nullptr), {true, false});
    CHECK(kept.report.removed.empty());
    CHECK(contains(kept.report.kept, "Gone.Release"));
    CHECK(fs::exists(build / "Gone.Release"));
    CHECK(kept.out.find("Stale outputs: 0 removed, 2 listed") != std::string::npos);
    CHECK(read_file(build / kOutputManifestFilename).find("\nGone.Release\n") != std::string::npos);

    auto removed = generate_into(generator, dir.path, two_projects("Core", nullptr));
    CHECK(contains(removed.report.removed, "Gone.Release"));
    CHECK_FALSE(fs::exists(build / "Gone.Release"));
}

TEST_CASE("Stale outputs: another target OS's section is left alone", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_sections");
    MakefileGenerator generator;
    const fs::path build = dir.path / "build";
    {
        TargetOsGuard os(TargetOS::Android);
        generate_into(generator, dir.path, two_projects("Core", "AndroidSide"));
    }
    REQUIRE(fs::exists(build / "AndroidSide.Release"));
    {
        // Same directory, other target OS: it owns its own section and removes nothing of Android's.
        TargetOsGuard os(TargetOS::Linux);
        auto linux_run = generate_into(generator, dir.path, two_projects("Core", nullptr));
        CHECK(linux_run.report.removed.empty());
        CHECK(linux_run.report.unlisted.empty());
    }
    CHECK(fs::exists(build / "AndroidSide.Release"));
    const std::string manifest = read_file(build / kOutputManifestFilename);
    CHECK(manifest.find("[makefile android]") != std::string::npos);
    CHECK(manifest.find("[makefile linux]") != std::string::npos);
    {
        // The Android section removes its own stale file on its next generation.
        TargetOsGuard os(TargetOS::Android);
        auto android_run = generate_into(generator, dir.path, two_projects("Core", nullptr));
        CHECK(contains(android_run.report.removed, "AndroidSide.Release"));
    }
    CHECK_FALSE(fs::exists(build / "AndroidSide.Release"));
}

TEST_CASE("Stale outputs: manifest entries outside the directory are ignored", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_escape");
    const fs::path outside = dir.path / "outside.txt";
    std::ofstream(outside) << "must survive\n";
    const fs::path build = dir.path / "build";
    fs::create_directories(build);
    std::ofstream(build / kOutputManifestFilename)
        << "[test linux]\n../outside.txt\n" << outside.generic_string() << "\nlisted.txt\n";
    std::ofstream(build / "listed.txt") << "stale\n";

    OutputManifestScope scope;
    scope.directory = build;
    scope.key = "test linux";
    std::ostringstream out, err;
    auto report = reconcile_generated_outputs(scope, {}, StaleOutputOptions{}, out, err);
    CHECK(report.removed == std::vector<std::string>{"listed.txt"});
    CHECK(fs::exists(outside));
    CHECK_FALSE(fs::exists(build / "listed.txt"));
}

TEST_CASE("Stale outputs: CMake project directories of removed projects", "[output_manifest]") {
    TempDir dir("sighmake_test_stale_cmake");
    TargetOsGuard os(TargetOS::Linux);
    CMakeGenerator generator;
    generate_into(generator, dir.path, two_projects("Core", "Gone"));
    REQUIRE(fs::exists(dir.path / "Gone" / "CMakeLists.txt"));
    REQUIRE(fs::exists(dir.path / kOutputManifestFilename));

    auto second = generate_into(generator, dir.path, two_projects("Core", nullptr));
    CHECK(contains(second.report.removed, "Gone/CMakeLists.txt"));
    CHECK_FALSE(fs::exists(dir.path / "Gone" / "CMakeLists.txt"));
    CHECK(fs::exists(dir.path / "Core" / "CMakeLists.txt"));
    CHECK(fs::exists(dir.path / "CMakeLists.txt"));
}

TEST_CASE("Stale outputs: Visual Studio project files of removed projects", "[output_manifest][vcxproj_generator]") {
    TempDir dir("sighmake_test_stale_vcxproj");
    TargetOsGuard os(TargetOS::Windows);
    VcxprojGenerator generator;
    const std::string before = R"(
[solution]
name = Stale
configurations = Release
platforms = x64

[project:Core]
type = lib
sources = main.cpp

[project:Gone]
type = lib
sources = main.cpp
)";
    const std::string after = R"(
[solution]
name = Stale
configurations = Release
platforms = x64

[project:Core]
type = lib
sources = main.cpp
)";
    const fs::path build = dir.path / "build";
    fs::create_directories(build);
    std::ofstream(build / "Gone_.vcxproj.user") << "<user settings/>\n";
    generate_into(generator, dir.path, before);
    REQUIRE(fs::exists(build / "Gone_.vcxproj"));
    REQUIRE(fs::exists(build / "Gone_.vcxproj.filters"));

    auto second = generate_into(generator, dir.path, after);
    CHECK(contains(second.report.removed, "Gone_.vcxproj"));
    CHECK(contains(second.report.removed, "Gone_.vcxproj.filters"));
    CHECK_FALSE(fs::exists(build / "Gone_.vcxproj"));
    CHECK(fs::exists(build / "Core_.vcxproj"));
    CHECK(fs::exists(build / "Gone_.vcxproj.user"));  // not sighmake's
    const std::string manifest = read_file(build / kOutputManifestFilename);
    CHECK(manifest.find("[vcxproj windows]") != std::string::npos);
}
