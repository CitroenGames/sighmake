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

