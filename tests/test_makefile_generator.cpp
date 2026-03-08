#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/vcxproj_generator.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper to read file content
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Helper to generate Makefile and return its content
struct MakefileResult {
    fs::path temp_dir;
    std::string content;
    Solution solution;

    ~MakefileResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static MakefileResult generate_makefile(const std::string& buildscript) {
    MakefileResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_makefile";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    // Create a dummy source file so source paths resolve
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";

    BuildscriptParser parser;
    result.solution = parser.parse_string(buildscript, result.temp_dir.string());

    MakefileGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    // Read the project-specific Makefile (e.g., App.Release), not the master Makefile
    auto build_dir = result.temp_dir / "build";
    if (fs::exists(build_dir)) {
        for (auto& entry : fs::directory_iterator(build_dir)) {
            if (entry.is_regular_file()) {
                std::string fname = entry.path().filename().string();
                // Skip the master "Makefile" - we want the project-specific ones
                if (fname == "Makefile") continue;
                result.content = read_file(entry.path());
                if (!result.content.empty()) break;
            }
        }
    }

    return result;
}

// ============================================================================
// Basic Makefile generation
// ============================================================================

TEST_CASE("MakefileGenerator creates output files", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
std = 17
)");
    CHECK(!result.content.empty());
}

TEST_CASE("MakefileGenerator contains CXX variable", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    if (!result.content.empty()) {
        // Makefile should reference CXX or CC compiler
        bool has_compiler = result.content.find("CXX") != std::string::npos ||
                           result.content.find("CC") != std::string::npos ||
                           result.content.find("g++") != std::string::npos ||
                           result.content.find("gcc") != std::string::npos;
        CHECK(has_compiler);
    }
}

TEST_CASE("MakefileGenerator includes C++ standard flag", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
std = 17
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-std=c++17") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator includes define flags", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
defines = MY_FLAG
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-DMY_FLAG") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator name and description", "[makefile_generator]") {
    MakefileGenerator gen;
    CHECK(gen.name() == "makefile");
    CHECK(!gen.description().empty());
}

// ============================================================================
// VcxprojGenerator name and description
// ============================================================================

TEST_CASE("VcxprojGenerator name and description", "[vcxproj_generator]") {
    VcxprojGenerator gen;
    CHECK(gen.name() == "vcxproj");
    CHECK(!gen.description().empty());
}

// ============================================================================
// Optimization and warning mapping
// ============================================================================

TEST_CASE("MakefileGenerator maps optimization Disabled to -O0", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
optimization[Release|Linux] = Disabled
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-O0") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator maps optimization MaxSpeed to -O3", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    if (!result.content.empty()) {
        // Default Release optimization is MaxSpeed -> -O3
        CHECK(result.content.find("-O3") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator maps warning Level4 to -Wall -Wextra", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
warning_level = Level4
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-Wall") != std::string::npos);
        CHECK(result.content.find("-Wextra") != std::string::npos);
    }
}

// ============================================================================
// Library output types
// ============================================================================

TEST_CASE("MakefileGenerator static lib produces ar command", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Lib]
type = lib
sources = main.cpp
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("ar") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator shared lib produces -shared flag", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Lib]
type = dll
sources = main.cpp
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-shared") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator shared lib uses -fPIC", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Lib]
type = dll
sources = main.cpp
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-fPIC") != std::string::npos);
    }
}

// ============================================================================
// Include and link flags
// ============================================================================

TEST_CASE("MakefileGenerator includes -I flags for include dirs", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
includes = src, include
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-I") != std::string::npos);
    }
}

// ============================================================================
// Master Makefile
// ============================================================================

TEST_CASE("MakefileGenerator creates master Makefile", "[makefile_generator]") {
    MakefileResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_makefile_master";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)", result.temp_dir.string());

    MakefileGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto master_path = result.temp_dir / "build" / "Makefile";
    CHECK(fs::exists(master_path));
}

TEST_CASE("MakefileGenerator master Makefile references project targets", "[makefile_generator]") {
    MakefileResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_makefile_targets";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);
    std::ofstream(result.temp_dir / "main.cpp") << "int main() { return 0; }";

    BuildscriptParser parser;
    result.solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)", result.temp_dir.string());

    MakefileGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    auto master_path = result.temp_dir / "build" / "Makefile";
    if (fs::exists(master_path)) {
        std::string content = read_file(master_path);
        CHECK(content.find("App") != std::string::npos);
    }
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("MakefileGenerator handles project without sources", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
)");
    // Should not crash even with no sources
    // Content may be empty since MakefileGenerator only emits CXXFLAGS when project has sources
}

TEST_CASE("MakefileGenerator C++ standard 20", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
std = 20
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-std=c++20") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator multiple defines", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
defines = DEF_A, DEF_B, DEF_C
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-DDEF_A") != std::string::npos);
        CHECK(result.content.find("-DDEF_B") != std::string::npos);
        CHECK(result.content.find("-DDEF_C") != std::string::npos);
    }
}

// ============================================================================
// Additional optimization and warning mappings
// ============================================================================

TEST_CASE("MakefileGenerator maps optimization MinSpace to -Os", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
optimization[Release|Linux] = MinSpace
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-Os") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator maps optimization Full to -O3", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
optimization[Release|Linux] = Full
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-O3") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator maps warning Level0 to -w", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
warning_level = Level0
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-w") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator maps warning Level3 to -Wall", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
warning_level = Level3
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-Wall") != std::string::npos);
    }
}

// ============================================================================
// Link flags
// ============================================================================

TEST_CASE("MakefileGenerator includes -L flags for library dirs", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
libdirs = /usr/local/lib
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-L") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator includes -l flags for libraries", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
additional_dependencies = pthread, m
)");
    if (!result.content.empty()) {
        bool has_lib_flags = result.content.find("-lpthread") != std::string::npos ||
                             result.content.find("-lm") != std::string::npos;
        CHECK(has_lib_flags);
    }
}

// ============================================================================
// C standard support
// ============================================================================

TEST_CASE("MakefileGenerator C standard 11", "[makefile_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_makefile_cstd";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.c") << "int main() { return 0; }";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
language = C
c_standard = 11
sources = main.c
)", temp_dir.string());

    MakefileGenerator generator;
    generator.generate(sol, temp_dir.string());

    auto build_dir = temp_dir / "build";
    std::string content;
    if (fs::exists(build_dir)) {
        for (auto& entry : fs::directory_iterator(build_dir)) {
            if (entry.is_regular_file() && entry.path().filename().string() != "Makefile") {
                content = read_file(entry.path());
                if (!content.empty()) break;
            }
        }
    }
    if (!content.empty()) {
        CHECK(content.find("-std=c11") != std::string::npos);
    }

    fs::remove_all(temp_dir, ec);
}

// ============================================================================
// UTF-8 flag mapping
// ============================================================================

TEST_CASE("MakefileGenerator includes UTF-8 charset flags", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
utf8 = true
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-finput-charset=UTF-8") != std::string::npos);
    }
}

// ============================================================================
// Function-level linking and data sections
// ============================================================================

TEST_CASE("MakefileGenerator includes -ffunction-sections for function_level_linking", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    if (!result.content.empty()) {
        // Release defaults have function_level_linking = true
        CHECK(result.content.find("-ffunction-sections") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator includes --gc-sections for linker optimization", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    if (!result.content.empty()) {
        // Release defaults have optimize_references and enable_comdat_folding
        CHECK(result.content.find("--gc-sections") != std::string::npos);
    }
}
