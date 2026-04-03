#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/cmake_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper to read file content
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Helper to generate CMake files and return root + project content
struct CMakeResult {
    fs::path temp_dir;
    std::string root_content;
    std::string project_content;
    Solution solution;

    ~CMakeResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static CMakeResult generate_cmake(const std::string& buildscript, const std::string& project_name = "App") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_cmake";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    // Create a dummy source file so source paths resolve
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(result.temp_dir / "util.cpp") << "void util() {}";
    std::ofstream(result.temp_dir / "main.c") << "int main() { return 0; }";
    std::ofstream(result.temp_dir / "pch.h") << "#pragma once\n";
    std::ofstream(result.temp_dir / "pch.cpp") << "#include \"pch.h\"\n";

    BuildscriptParser parser;
    result.solution = parser.parse_string(buildscript, result.temp_dir.string());

    CMakeGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    // Read root CMakeLists.txt
    auto root_path = result.temp_dir / "CMakeLists.txt";
    if (fs::exists(root_path)) {
        result.root_content = read_file(root_path);
    }

    // Read project CMakeLists.txt
    auto proj_path = result.temp_dir / project_name / "CMakeLists.txt";
    if (fs::exists(proj_path)) {
        result.project_content = read_file(proj_path);
    }

    return result;
}

// ============================================================================
// Basic generation
// ============================================================================

TEST_CASE("CMakeGenerator name and description", "[cmake_generator]") {
    CMakeGenerator gen;
    CHECK(gen.name() == "cmake");
    CHECK(!gen.description().empty());
}

TEST_CASE("CMakeGenerator creates root CMakeLists.txt", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32, x64

[project:App]
type = exe
sources = main.cpp
)");
    CHECK(!result.root_content.empty());
    CHECK(result.root_content.find("cmake_minimum_required") != std::string::npos);
    CHECK(result.root_content.find("project(Test") != std::string::npos);
}

TEST_CASE("CMakeGenerator creates project CMakeLists.txt", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32, x64

[project:App]
type = exe
sources = main.cpp
)");
    CHECK(!result.project_content.empty());
}

TEST_CASE("CMakeGenerator root contains add_subdirectory", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
)");
    CHECK(result.root_content.find("add_subdirectory(App)") != std::string::npos);
}

// ============================================================================
// Target types
// ============================================================================

TEST_CASE("CMakeGenerator exe produces add_executable", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
)");
    CHECK(result.project_content.find("add_executable(App)") != std::string::npos);
}

TEST_CASE("CMakeGenerator lib produces add_library STATIC", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:Lib]
type = lib
sources = main.cpp
)", "Lib");
    CHECK(result.project_content.find("add_library(Lib STATIC)") != std::string::npos);
}

TEST_CASE("CMakeGenerator dll produces add_library SHARED", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:Lib]
type = dll
sources = main.cpp
)", "Lib");
    CHECK(result.project_content.find("add_library(Lib SHARED)") != std::string::npos);
}

// ============================================================================
// C++ standard
// ============================================================================

TEST_CASE("CMakeGenerator sets CXX_STANDARD", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
std = 20
)");
    CHECK(result.project_content.find("CXX_STANDARD 20") != std::string::npos);
    CHECK(result.project_content.find("CXX_STANDARD_REQUIRED ON") != std::string::npos);
}

TEST_CASE("CMakeGenerator sets C_STANDARD for C projects", "[cmake_generator]") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_cmake_c";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.c") << "int main() { return 0; }";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
language = C
c_standard = 11
sources = main.c
)", result.temp_dir.string());

    CMakeGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto proj_path = result.temp_dir / "App" / "CMakeLists.txt";
    if (fs::exists(proj_path)) {
        result.project_content = read_file(proj_path);
    }

    CHECK(result.project_content.find("C_STANDARD 11") != std::string::npos);
}

// ============================================================================
// Compile definitions
// ============================================================================

TEST_CASE("CMakeGenerator includes compile definitions", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
defines = MY_FLAG, RELEASE_MODE
)");
    CHECK(result.project_content.find("target_compile_definitions") != std::string::npos);
    CHECK(result.project_content.find("MY_FLAG") != std::string::npos);
    CHECK(result.project_content.find("RELEASE_MODE") != std::string::npos);
}

TEST_CASE("CMakeGenerator per-config defines use generator expressions", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
defines[Debug|x64] = _DEBUG
defines[Release|x64] = NDEBUG
)");
    CHECK(result.project_content.find("$<$<CONFIG:Debug>:_DEBUG>") != std::string::npos);
    CHECK(result.project_content.find("$<$<CONFIG:Release>:NDEBUG>") != std::string::npos);
}

// ============================================================================
// Include directories
// ============================================================================

TEST_CASE("CMakeGenerator includes include directories", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
includes = src, include
)");
    CHECK(result.project_content.find("target_include_directories") != std::string::npos);
}

// ============================================================================
// Compile options
// ============================================================================

TEST_CASE("CMakeGenerator maps warning level", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
warning_level = Level4
)");
    CHECK(result.project_content.find("target_compile_options") != std::string::npos);
    CHECK(result.project_content.find("/W4") != std::string::npos);
    CHECK(result.project_content.find("-Wall") != std::string::npos);
    CHECK(result.project_content.find("-Wextra") != std::string::npos);
}

TEST_CASE("CMakeGenerator maps per-config optimization", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
)");
    // Debug has Disabled optimization by default, Release has MaxSpeed
    if (!result.project_content.empty()) {
        CHECK(result.project_content.find("target_compile_options") != std::string::npos);
    }
}

TEST_CASE("CMakeGenerator MSVC and GCC options", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
warning_level = Level3
multiprocessor = true
)");
    CHECK(result.project_content.find("CXX_COMPILER_ID:MSVC") != std::string::npos);
    CHECK(result.project_content.find("/MP") != std::string::npos);
}

// ============================================================================
// Dependencies
// ============================================================================

TEST_CASE("CMakeGenerator project dependencies with visibility", "[cmake_generator]") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_cmake_deps";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(result.temp_dir / "lib.cpp") << "void lib() {}";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:CoreLib]
type = lib
sources = lib.cpp

[project:App]
type = exe
sources = main.cpp
target_link_libraries(
    CoreLib PUBLIC
)
)", result.temp_dir.string());

    CMakeGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto proj_path = result.temp_dir / "App" / "CMakeLists.txt";
    if (fs::exists(proj_path)) {
        result.project_content = read_file(proj_path);
    }

    CHECK(result.project_content.find("target_link_libraries") != std::string::npos);
    CHECK(result.project_content.find("CoreLib") != std::string::npos);
}

TEST_CASE("CMakeGenerator dependency order in root CMakeLists", "[cmake_generator]") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_cmake_order";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(result.temp_dir / "lib.cpp") << "void lib() {}";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:CoreLib]
type = lib
sources = lib.cpp

[project:App]
type = exe
sources = main.cpp
depends = CoreLib
)", result.temp_dir.string());

    CMakeGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto root_path = result.temp_dir / "CMakeLists.txt";
    if (fs::exists(root_path)) {
        result.root_content = read_file(root_path);
    }

    // CoreLib should appear before App in the root file
    auto core_pos = result.root_content.find("add_subdirectory(CoreLib)");
    auto app_pos = result.root_content.find("add_subdirectory(App)");
    CHECK(core_pos != std::string::npos);
    CHECK(app_pos != std::string::npos);
    CHECK(core_pos < app_pos);
}

// ============================================================================
// Target properties
// ============================================================================

TEST_CASE("CMakeGenerator shared lib has POSITION_INDEPENDENT_CODE", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:Lib]
type = dll
sources = main.cpp
)", "Lib");
    CHECK(result.project_content.find("POSITION_INDEPENDENT_CODE ON") != std::string::npos);
}

TEST_CASE("CMakeGenerator custom target name", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
target_name = MyApp
)");
    CHECK(result.project_content.find("OUTPUT_NAME") != std::string::npos);
    CHECK(result.project_content.find("MyApp") != std::string::npos);
}

// ============================================================================
// PCH support
// ============================================================================

TEST_CASE("CMakeGenerator PCH generates target_precompile_headers", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp, pch.cpp
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
pch.cpp:pch_header = pch.h
)");
    CHECK(result.project_content.find("target_precompile_headers") != std::string::npos);
    CHECK(result.project_content.find("pch.h") != std::string::npos);
}

// ============================================================================
// Build events
// ============================================================================

TEST_CASE("CMakeGenerator post-build event", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
postbuild = echo Done
)");
    CHECK(result.project_content.find("add_custom_command") != std::string::npos);
    CHECK(result.project_content.find("POST_BUILD") != std::string::npos);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("CMakeGenerator handles project without sources", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
)");
    // Should not crash even with no sources
    CHECK(result.project_content.find("add_executable(App)") != std::string::npos);
}

TEST_CASE("CMakeGenerator multiple projects", "[cmake_generator]") {
    CMakeResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_cmake_multi";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(result.temp_dir / "lib.cpp") << "void lib() {}";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = MultiProject
configurations = Debug, Release
platforms = x64

[project:Engine]
type = lib
sources = lib.cpp

[project:Game]
type = exe
sources = main.cpp
depends = Engine
)", result.temp_dir.string());

    CMakeGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto root_path = result.temp_dir / "CMakeLists.txt";
    auto engine_path = result.temp_dir / "Engine" / "CMakeLists.txt";
    auto game_path = result.temp_dir / "Game" / "CMakeLists.txt";

    CHECK(fs::exists(root_path));
    CHECK(fs::exists(engine_path));
    CHECK(fs::exists(game_path));

    std::string root = read_file(root_path);
    CHECK(root.find("add_subdirectory(Engine)") != std::string::npos);
    CHECK(root.find("add_subdirectory(Game)") != std::string::npos);

    std::string engine = read_file(engine_path);
    CHECK(engine.find("add_library(Engine STATIC)") != std::string::npos);

    std::string game = read_file(game_path);
    CHECK(game.find("add_executable(Game)") != std::string::npos);
}

// ============================================================================
// Subsystem and link options
// ============================================================================

TEST_CASE("CMakeGenerator subsystem Console", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
subsystem = Console
)");
    CHECK(result.project_content.find("/SUBSYSTEM:CONSOLE") != std::string::npos);
}

TEST_CASE("CMakeGenerator exception handling", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
exception_handling = Sync
)");
    CHECK(result.project_content.find("/EHsc") != std::string::npos);
}

// ============================================================================
// UTF-8 flag
// ============================================================================

TEST_CASE("CMakeGenerator UTF-8 flags", "[cmake_generator]") {
    auto result = generate_cmake(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:App]
type = exe
sources = main.cpp
utf8 = true
)");
    CHECK(result.project_content.find("/utf-8") != std::string::npos);
    CHECK(result.project_content.find("-finput-charset=UTF-8") != std::string::npos);
}
