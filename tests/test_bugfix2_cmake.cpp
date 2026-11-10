#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "parsers/cmake_parser.hpp"
#include "generators/cmake_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct CMakeResult {
    fs::path temp_dir;
    std::string project_content;

    ~CMakeResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

CMakeResult generate_cmake(const std::string& buildscript,
                           const std::vector<std::pair<std::string, std::string>>& files,
                           const std::string& project_name = "App") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_bugfix2_cmake_gen";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    for (const auto& [rel, content] : files) {
        fs::path p = result.temp_dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    BuildscriptParser parser;
    Solution solution = parser.parse_string(buildscript, result.temp_dir.string());
    CMakeGenerator generator;
    generator.generate(solution, result.temp_dir.string());

    result.project_content = read_file(result.temp_dir / project_name / "CMakeLists.txt");
    return result;
}

const Project* find_project(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

bool contains(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

// ============================================================================
// Custom build rules
// ============================================================================

static const char* kCustomHeader = R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
)";

TEST_CASE("bugfix2_cmake: custom build outputs are attached to the target so the rule runs",
          "[bugfix2_cmake]") {
    auto result = generate_cmake(std::string(kCustomHeader) + R"(
custom_build(gen.txt.in, command = cp gen.txt.in gen.cpp, outputs = gen.cpp, description = Generating)
)", {{"main.cpp", "int main() { return 0; }\n"}, {"gen.txt.in", "x\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("OUTPUT") != std::string::npos);
    CHECK(c.find("target_sources(App PRIVATE\n    ${CMAKE_CURRENT_SOURCE_DIR}/../gen.cpp\n)") != std::string::npos);
}

TEST_CASE("bugfix2_cmake: custom build without outputs becomes a custom target the project depends on",
          "[bugfix2_cmake]") {
    auto result = generate_cmake(std::string(kCustomHeader) + R"(
custom_build(gen.txt.in, command = echo hello, description = Say hello)
)", {{"main.cpp", "int main() { return 0; }\n"}, {"gen.txt.in", "x\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("add_custom_target(") != std::string::npos);
    CHECK(c.find("add_dependencies(App ") != std::string::npos);
}

TEST_CASE("bugfix2_cmake: custom build commands that differ per configuration are guarded",
          "[bugfix2_cmake]") {
    auto result = generate_cmake(std::string(kCustomHeader) + R"(
gen.in:command[Debug|x64] = echo dbg
gen.in:command[Release|x64] = echo rel
gen.in:outputs = gen.out
)", {{"main.cpp", "int main() { return 0; }\n"}, {"gen.in", "x\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("$<$<CONFIG:Debug>:dbg>") != std::string::npos);
    CHECK(c.find("$<$<CONFIG:Release>:rel>") != std::string::npos);
}

TEST_CASE("bugfix2_cmake: debug/optimized/general keywords scope the following library",
          "[bugfix2_cmake]") {
    CMakeParser parser;
    auto sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE debug dbgonly optimized optonly general both plain)
)");
    auto* p = find_project(sol, "App");
    REQUIRE(p != nullptr);
    const auto& dbg = p->configurations.at("Debug|x64").link.additional_dependencies;
    const auto& rel = p->configurations.at("Release|x64").link.additional_dependencies;
    CHECK_FALSE(contains(dbg, "debug.lib"));
    CHECK_FALSE(contains(rel, "optimized.lib"));
    CHECK_FALSE(contains(rel, "general.lib"));
    CHECK(contains(dbg, "dbgonly.lib"));
    CHECK_FALSE(contains(rel, "dbgonly.lib"));
    CHECK(contains(rel, "optonly.lib"));
    CHECK_FALSE(contains(dbg, "optonly.lib"));
    CHECK(contains(dbg, "both.lib"));
    CHECK(contains(rel, "both.lib"));
    CHECK(contains(dbg, "plain.lib"));
    CHECK(contains(rel, "plain.lib"));
}

// ============================================================================
// Release-only link optimisation flags
// ============================================================================

TEST_CASE("bugfix2_cmake: gc-sections is written even without other linker options", "[bugfix2_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Final
platforms = x64

[project:App]
type = exe
sources = main.cpp

[config:Final|x64]
optimize_references = true
)", {{"main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("-Wl,--gc-sections") != std::string::npos);
    CHECK(c.find("/OPT:REF") != std::string::npos);
}

// ============================================================================
// C-only projects
// ============================================================================

TEST_CASE("bugfix2_cmake: C-only projects guard flags with the C compiler id", "[bugfix2_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.c
warning_level = Level3
subsystem = Console
)", {{"main.c", "int main(void) { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("$<C_COMPILER_ID:MSVC>") != std::string::npos);
    CHECK(c.find("CXX_COMPILER_ID") == std::string::npos);
}

// ============================================================================
// Backslashes in build-event commands
// ============================================================================

TEST_CASE("bugfix2_cmake: escape sequences in build events survive, Windows paths still convert",
          "[bugfix2_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
postbuild = printf "a\n" && copy "C:\out\app.exe" bin\app.exe
)", {{"main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("a/n") == std::string::npos);
    CHECK(c.find("\"a\\\\\\\\n\"") != std::string::npos);
    CHECK(c.find("C:/out/app.exe") != std::string::npos);
    CHECK(c.find("bin/app.exe") != std::string::npos);
}

TEST_CASE("bugfix2_cmake: escape sequences survive in configuration-specific build events",
          "[bugfix2_cmake]") {
    auto result = generate_cmake(R"(
[solution]
name = T
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp

[config:Release|x64]
postbuild = printf "x\ty\n" "C:\out\app.exe"
)", {{"main.cpp", "int main() { return 0; }\n"}});
    const auto& c = result.project_content;
    INFO(c);
    CHECK(c.find("$<$<CONFIG:Release>:x\\\\\\\\ty\\\\\\\\n>") != std::string::npos);
    CHECK(c.find("$<$<CONFIG:Release>:C:/out/app.exe>") != std::string::npos);
}
