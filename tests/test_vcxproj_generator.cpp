#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/vcxproj_generator.hpp"
#include "pugixml.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Helper: parse a buildscript, generate vcxproj to temp dir, return the path
struct GeneratedProject {
    fs::path temp_dir;
    fs::path vcxproj_path;
    fs::path sln_path;  // .sln or .slnx
    Solution solution;

    ~GeneratedProject() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static GeneratedProject generate_from_buildscript(const std::string& buildscript) {
    GeneratedProject result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    BuildscriptParser parser;
    result.solution = parser.parse_string(buildscript, result.temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    // Find generated files (check both temp dir and parent for .sln/.slnx)
    auto search_dir = [&](const fs::path& dir) {
        if (!fs::exists(dir)) return;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".vcxproj") {
                result.vcxproj_path = entry.path();
            }
            if (entry.path().extension() == ".sln" || entry.path().extension() == ".slnx") {
                result.sln_path = entry.path();
            }
        }
    };
    search_dir(result.temp_dir);
    // .sln/.slnx may be generated in parent directory
    search_dir(result.temp_dir.parent_path());

    return result;
}

// Helper to read file content
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// ============================================================================
// Basic vcxproj structure
// ============================================================================

TEST_CASE("VcxprojGenerator creates valid XML", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
std = 17
)");

    REQUIRE(!gen.vcxproj_path.empty());
    REQUIRE(fs::exists(gen.vcxproj_path));

    pugi::xml_document doc;
    auto result = doc.load_file(gen.vcxproj_path.string().c_str());
    CHECK(result.status == pugi::status_ok);

    auto project_node = doc.child("Project");
    CHECK(!project_node.empty());
}

TEST_CASE("VcxprojGenerator sets ConfigurationType", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    // Find ConfigurationType node
    std::string config_type;
    for (auto& pg : doc.child("Project").children("PropertyGroup")) {
        auto ct = pg.child("ConfigurationType");
        if (!ct.empty()) {
            config_type = ct.text().as_string();
            break;
        }
    }
    CHECK(config_type == "Application");
}

TEST_CASE("VcxprojGenerator sets LanguageStandard", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
std = 17
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    // Find LanguageStandard node in ClCompile ItemDefinitionGroup
    std::string lang_std;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto ls = cl.child("LanguageStandard");
        if (!ls.empty()) {
            lang_std = ls.text().as_string();
            break;
        }
    }
    CHECK(lang_std == "stdcpp17");
}

TEST_CASE("VcxprojGenerator includes source files in ClCompile", "[vcxproj_generator]") {
    // Create temp source files so the parser can find them
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_sources";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "util.cpp") << "void foo() {}";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp, util.cpp
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    // Find the generated vcxproj
    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    // Count ClCompile items
    int compile_count = 0;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ci : ig.children("ClCompile")) {
            compile_count++;
        }
    }
    CHECK(compile_count >= 2);

    // Cleanup
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("VcxprojGenerator includes header files in ClInclude", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_headers";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "types.h") << "#pragma once";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp
headers = types.h
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    int include_count = 0;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ci : ig.children("ClInclude")) {
            include_count++;
        }
    }
    CHECK(include_count >= 1);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("VcxprojGenerator creates solution file", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = TestSln
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(!gen.sln_path.empty());
    REQUIRE(fs::exists(gen.sln_path));

    std::string content = read_file(gen.sln_path);
    // Solution file (either .sln or .slnx) should contain the project name
    CHECK(content.find("App") != std::string::npos);
}

TEST_CASE("VcxprojGenerator sets PreprocessorDefinitions", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
defines = MY_DEFINE
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_define = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto pd = cl.child("PreprocessorDefinitions");
        if (!pd.empty()) {
            std::string defs = pd.text().as_string();
            if (defs.find("MY_DEFINE") != std::string::npos) {
                found_define = true;
            }
        }
    }
    CHECK(found_define);
}

TEST_CASE("VcxprojGenerator handles static library type", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Lib]
type = lib
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    std::string config_type;
    for (auto& pg : doc.child("Project").children("PropertyGroup")) {
        auto ct = pg.child("ConfigurationType");
        if (!ct.empty()) {
            config_type = ct.text().as_string();
            break;
        }
    }
    CHECK(config_type == "StaticLibrary");
}

// ============================================================================
// PCH in vcxproj output
// ============================================================================

TEST_CASE("VcxprojGenerator sets PrecompiledHeader to Use", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
pch = Use
pch_header = pch.h
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_pch = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto pch_node = cl.child("PrecompiledHeader");
        if (!pch_node.empty()) {
            std::string mode = pch_node.text().as_string();
            if (mode == "Use") found_pch = true;
        }
    }
    CHECK(found_pch);
}

TEST_CASE("VcxprojGenerator sets PrecompiledHeaderFile", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
pch = Use
pch_header = pch.h
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_header = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto pch_file = cl.child("PrecompiledHeaderFile");
        if (!pch_file.empty()) {
            std::string header = pch_file.text().as_string();
            if (header == "pch.h") found_header = true;
        }
    }
    CHECK(found_header);
}

// ============================================================================
// DLL type
// ============================================================================

TEST_CASE("VcxprojGenerator handles DynamicLibrary type", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:Dll]
type = dll
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    std::string config_type;
    for (auto& pg : doc.child("Project").children("PropertyGroup")) {
        auto ct = pg.child("ConfigurationType");
        if (!ct.empty()) {
            config_type = ct.text().as_string();
            break;
        }
    }
    CHECK(config_type == "DynamicLibrary");
}

// ============================================================================
// Multiple configurations and platforms
// ============================================================================

TEST_CASE("VcxprojGenerator handles Debug and Release configs", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    int idg_count = 0;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        (void)idg;
        idg_count++;
    }
    CHECK(idg_count >= 2);
}

TEST_CASE("VcxprojGenerator handles Win32 and x64 platforms", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32, x64

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    int idg_count = 0;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        (void)idg;
        idg_count++;
    }
    CHECK(idg_count >= 2);
}

TEST_CASE("VcxprojGenerator produces correct ProjectConfiguration items", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    int pc_count = 0;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& pc : ig.children("ProjectConfiguration")) {
            (void)pc;
            pc_count++;
        }
    }
    CHECK(pc_count >= 2);
}

// ============================================================================
// Build events and output directories
// ============================================================================

TEST_CASE("VcxprojGenerator sets PostBuildEvent", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(
        "[solution]\n"
        "name = Test\n"
        "configurations = Debug\n"
        "platforms = Win32\n\n"
        "[project:App]\n"
        "type = exe\n"
        "postbuild = echo done\n"
    );

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_postbuild = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto pbe = idg.child("PostBuildEvent");
        if (!pbe.empty()) {
            auto cmd = pbe.child("Command");
            if (!cmd.empty()) {
                std::string cmd_str = cmd.text().as_string();
                if (!cmd_str.empty()) found_postbuild = true;
            }
        }
    }
    CHECK(found_postbuild);
}

TEST_CASE("VcxprojGenerator sets target name and extension", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
target_name = myapp
target_ext = .com
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_name = false;
    bool found_ext = false;
    for (auto& pg : doc.child("Project").children("PropertyGroup")) {
        auto tn = pg.child("TargetName");
        if (!tn.empty() && std::string(tn.text().as_string()) == "myapp") found_name = true;
        auto te = pg.child("TargetExt");
        if (!te.empty() && std::string(te.text().as_string()) == ".com") found_ext = true;
    }
    CHECK(found_name);
    CHECK(found_ext);
}

// ============================================================================
// Solution format
// ============================================================================

TEST_CASE("VcxprojGenerator solution file has configuration mappings", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = TestSln
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(!gen.sln_path.empty());
    REQUIRE(fs::exists(gen.sln_path));

    std::string content = read_file(gen.sln_path);
    // Should contain Debug and Release configurations
    CHECK(content.find("Debug") != std::string::npos);
    CHECK(content.find("Release") != std::string::npos);
}

// ============================================================================
// Additional settings
// ============================================================================

TEST_CASE("VcxprojGenerator sets RuntimeLibrary", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_runtime = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto rl = cl.child("RuntimeLibrary");
        if (!rl.empty()) {
            std::string rtl = rl.text().as_string();
            if (!rtl.empty()) found_runtime = true;
        }
    }
    CHECK(found_runtime);
}

TEST_CASE("VcxprojGenerator sets AdditionalIncludeDirectories", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
includes = src, include
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_includes = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto aid = cl.child("AdditionalIncludeDirectories");
        if (!aid.empty()) {
            std::string dirs = aid.text().as_string();
            if (!dirs.empty()) found_includes = true;
        }
    }
    CHECK(found_includes);
}

TEST_CASE("VcxprojGenerator sets WarningLevel", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
warning_level = Level4
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_wl = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto wl = cl.child("WarningLevel");
        if (!wl.empty()) {
            std::string level = wl.text().as_string();
            if (level == "Level4") found_wl = true;
        }
    }
    CHECK(found_wl);
}

TEST_CASE("VcxprojGenerator sets Optimization per config", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Win32

[project:App]
type = exe
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_disabled = false;
    bool found_maxspeed = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto opt = cl.child("Optimization");
        if (!opt.empty()) {
            std::string level = opt.text().as_string();
            if (level == "Disabled") found_disabled = true;
            if (level == "MaxSpeed") found_maxspeed = true;
        }
    }
    CHECK(found_disabled);
    CHECK(found_maxspeed);
}

TEST_CASE("VcxprojGenerator sets ExceptionHandling", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
exception_handling = Sync
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto cl = idg.child("ClCompile");
        auto eh = cl.child("ExceptionHandling");
        if (!eh.empty() && std::string(eh.text().as_string()) == "Sync") {
            found = true;
        }
    }
    CHECK(found);
}

// ============================================================================
// Per-file settings in vcxproj output
// ============================================================================

TEST_CASE("VcxprojGenerator sets per-file PCH Create", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_perfile_pch";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "pch.cpp") << "#include \"pch.h\"";
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = pch.cpp, main.cpp
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
pch.cpp:pch_header = pch.h
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    bool found_create = false;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ci : ig.children("ClCompile")) {
            std::string include_attr = ci.attribute("Include").as_string();
            if (include_attr.find("pch.cpp") != std::string::npos) {
                auto pch_node = ci.child("PrecompiledHeader");
                if (!pch_node.empty() && std::string(pch_node.text().as_string()) == "Create") {
                    found_create = true;
                }
            }
        }
    }
    CHECK(found_create);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("VcxprojGenerator sets per-file ExcludedFromBuild", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_perfile_excl";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "unused.cpp") << "void unused() {}";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp, unused.cpp
unused.cpp:exclude = true
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    bool found_excluded = false;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ci : ig.children("ClCompile")) {
            std::string include_attr = ci.attribute("Include").as_string();
            if (include_attr.find("unused.cpp") != std::string::npos) {
                auto excl = ci.child("ExcludedFromBuild");
                if (!excl.empty()) {
                    found_excluded = true;
                }
            }
        }
    }
    CHECK(found_excluded);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("VcxprojGenerator sets per-file Optimization", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_perfile_opt";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "slow.cpp") << "void slow() {}";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Win32

[project:App]
type = exe
sources = main.cpp, slow.cpp
slow.cpp:optimization = Disabled
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    bool found_opt = false;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ci : ig.children("ClCompile")) {
            std::string include_attr = ci.attribute("Include").as_string();
            if (include_attr.find("slow.cpp") != std::string::npos) {
                auto opt_node = ci.child("Optimization");
                if (!opt_node.empty() && std::string(opt_node.text().as_string()) == "Disabled") {
                    found_opt = true;
                }
            }
        }
    }
    CHECK(found_opt);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

// ============================================================================
// File type handling in vcxproj output
// ============================================================================

TEST_CASE("VcxprojGenerator includes MASM files", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_masm";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "code.asm") << "; assembly";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp
masm = code.asm
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    bool found_masm = false;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& mi : ig.children("MASM")) {
            found_masm = true;
        }
    }
    CHECK(found_masm);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("VcxprojGenerator includes ResourceCompile files", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_rc";
    fs::create_directories(temp_dir);
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }";
    std::ofstream(temp_dir / "app.rc") << "// resource file";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
sources = main.cpp
resources = app.rc
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    fs::path vcxproj_path;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            vcxproj_path = entry.path();
            break;
        }
    }

    REQUIRE(!vcxproj_path.empty());
    pugi::xml_document doc;
    doc.load_file(vcxproj_path.string().c_str());

    bool found_rc = false;
    for (auto& ig : doc.child("Project").children("ItemGroup")) {
        for (auto& ri : ig.children("ResourceCompile")) {
            found_rc = true;
        }
    }
    CHECK(found_rc);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

// ============================================================================
// Linker settings in vcxproj output
// ============================================================================

TEST_CASE("VcxprojGenerator sets SubSystem in Link", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
subsystem = Windows
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto link = idg.child("Link");
        auto ss = link.child("SubSystem");
        if (!ss.empty() && std::string(ss.text().as_string()) == "Windows") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("VcxprojGenerator sets AdditionalDependencies in Link", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
additional_dependencies = ws2_32.lib, user32.lib
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto link = idg.child("Link");
        auto ad = link.child("AdditionalDependencies");
        if (!ad.empty()) {
            std::string deps = ad.text().as_string();
            if (deps.find("ws2_32.lib") != std::string::npos) {
                found = true;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("VcxprojGenerator sets AdditionalLibraryDirectories in Link", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:App]
type = exe
libdirs = lib/x86
)");

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto link = idg.child("Link");
        auto ald = link.child("AdditionalLibraryDirectories");
        if (!ald.empty()) {
            std::string dirs = ald.text().as_string();
            if (!dirs.empty()) {
                found = true;
            }
        }
    }
    CHECK(found);
}

// ============================================================================
// Build events with messages in vcxproj output
// ============================================================================

TEST_CASE("VcxprojGenerator sets PreBuildEvent with message", "[vcxproj_generator]") {
    auto gen = generate_from_buildscript(
        "[solution]\n"
        "name = Test\n"
        "configurations = Debug\n"
        "platforms = Win32\n\n"
        "[project:App]\n"
        "type = exe\n\n"
        "[config:Debug|Win32]\n"
        "prebuild = echo pre\n"
        "prebuild_message = Running prebuild step\n"
    );

    REQUIRE(fs::exists(gen.vcxproj_path));
    pugi::xml_document doc;
    doc.load_file(gen.vcxproj_path.string().c_str());

    bool found_cmd = false;
    bool found_msg = false;
    for (auto& idg : doc.child("Project").children("ItemDefinitionGroup")) {
        auto pbe = idg.child("PreBuildEvent");
        if (!pbe.empty()) {
            auto cmd = pbe.child("Command");
            if (!cmd.empty() && !std::string(cmd.text().as_string()).empty()) {
                found_cmd = true;
            }
            auto msg = pbe.child("Message");
            if (!msg.empty() && !std::string(msg.text().as_string()).empty()) {
                found_msg = true;
            }
        }
    }
    CHECK(found_cmd);
    CHECK(found_msg);
}

TEST_CASE("VcxprojGenerator creates ProjectReference for dependencies", "[vcxproj_generator]") {
    auto temp_dir = fs::temp_directory_path() / "sighmake_test_vcxproj_projref";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Win32

[project:LibA]
type = lib

[project:App]
type = exe
depends = LibA
)", temp_dir.string());

    VcxprojGenerator generator;
    generator.generate(sol, temp_dir.string());

    // Find App's vcxproj (not LibA's)
    fs::path app_vcxproj;
    for (auto& entry : fs::recursive_directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".vcxproj") {
            std::string content;
            std::ifstream f(entry.path());
            content.assign((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            if (content.find("Application") != std::string::npos) {
                app_vcxproj = entry.path();
                break;
            }
        }
    }

    if (!app_vcxproj.empty()) {
        pugi::xml_document doc;
        doc.load_file(app_vcxproj.string().c_str());

        bool found_ref = false;
        for (auto& ig : doc.child("Project").children("ItemGroup")) {
            for (auto& pr : ig.children("ProjectReference")) {
                found_ref = true;
            }
        }
        CHECK(found_ref);
    }

    fs::remove_all(temp_dir, ec);
}
