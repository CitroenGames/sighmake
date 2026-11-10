#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "common/target_os.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/vcxproj_generator.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

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
    std::string master_content;
    std::map<std::string, std::string> files;  // All generated makefiles by filename
    Solution solution;

    ~MakefileResult() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

static MakefileResult generate_makefile(const std::string& buildscript,
                                        std::vector<std::string> source_files = {"main.cpp"}) {
    MakefileResult result;
    result.temp_dir = fs::temp_directory_path() / "sighmake_test_makefile";
    std::error_code ec;
    fs::remove_all(result.temp_dir, ec);
    fs::create_directories(result.temp_dir);

    // Create dummy source files so source paths resolve.
    for (const auto& source : source_files) {
        fs::path source_path = result.temp_dir / source;
        fs::create_directories(source_path.parent_path());
        std::ofstream(source_path) << "int main() { return 0; }";
    }

    BuildscriptParser parser;
    result.solution = parser.parse_string(buildscript, result.temp_dir.string());

    MakefileGenerator generator;
    generator.generate(result.solution, result.temp_dir.string());

    // Read the project-specific Makefile (e.g., App.Release), not the master Makefile
    auto build_dir = result.temp_dir / "build";
    if (fs::exists(build_dir)) {
        auto master_path = build_dir / "Makefile";
        if (fs::exists(master_path)) {
            result.master_content = read_file(master_path);
        }

        for (auto& entry : fs::directory_iterator(build_dir)) {
            if (entry.is_regular_file()) {
                std::string fname = entry.path().filename().string();
                result.files[fname] = read_file(entry.path());
                // Skip the master Makefile and generator support artifacts. Directory iteration
                // order is unspecified, so only a project Makefile may become the primary result.
                if (fname == "Makefile") continue;
                if (fname == "write-sighmake-target-receipt.py" ||
                    fname == "Write-SighmakeTargetReceipt.ps1" ||
                    (fname.size() >= 25 &&
                     fname.compare(fname.size() - 25, 25, ".runtime-dependencies.txt") == 0)) {
                    continue;
                }
                if (result.content.empty()) {
                    result.content = result.files[fname];
                }
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

static std::string makefile_for_config(const std::string& defines_lines, const std::string& config) {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)" + defines_lines);
    for (const auto& [name, content] : result.files) {
        if (name.find("." + config) != std::string::npos && name.find("App") != std::string::npos)
            return content;
    }
    return {};
}

TEST_CASE("MakefileGenerator defaults NDEBUG for Release and _DEBUG for Debug", "[makefile_generator][ndebug]") {
    const std::string rel = makefile_for_config("defines = MY_FLAG\n", "Release");
    const std::string dbg = makefile_for_config("defines = MY_FLAG\n", "Debug");
    REQUIRE(!rel.empty());
    REQUIRE(!dbg.empty());
    CHECK(rel.find("-DNDEBUG") != std::string::npos);
    CHECK(rel.find("-D_DEBUG") == std::string::npos);
    CHECK(dbg.find("-D_DEBUG") != std::string::npos);
    CHECK(dbg.find("-DNDEBUG") == std::string::npos);
    CHECK(rel.find("-DMY_FLAG") != std::string::npos);
}

TEST_CASE("MakefileGenerator explicit NDEBUG or _DEBUG suppresses the default", "[makefile_generator][ndebug]") {
    // Project keeps asserts live in Release: no NDEBUG is added.
    const std::string rel = makefile_for_config("defines[Release|Linux] = _DEBUG\n", "Release");
    REQUIRE(!rel.empty());
    CHECK(rel.find("-D_DEBUG") != std::string::npos);
    CHECK(rel.find("-DNDEBUG") == std::string::npos);
    // Explicit NDEBUG in Debug: no _DEBUG is added.
    const std::string dbg = makefile_for_config("defines[Debug|Linux] = NDEBUG\n", "Debug");
    REQUIRE(!dbg.empty());
    CHECK(dbg.find("-DNDEBUG") != std::string::npos);
    CHECK(dbg.find("-D_DEBUG") == std::string::npos);
}

TEST_CASE("MakefileGenerator NDEBUG default is per config and recorded in the flag stamp", "[makefile_generator][ndebug]") {
    // An explicit define in Release must not leak into Debug's default.
    const std::string dbg = makefile_for_config("defines[Release|Linux] = NDEBUG\n", "Debug");
    REQUIRE(!dbg.empty());
    CHECK(dbg.find("-D_DEBUG") != std::string::npos);
    // The define is part of CXXFLAGS, which feeds the per-target flag stamp, so a
    // generator upgrade that adds NDEBUG changes the stamp and rebuilds objects.
    const std::string rel = makefile_for_config("", "Release");
    REQUIRE(!rel.empty());
    CHECK(rel.find("CXXFLAGS") != std::string::npos);
    CHECK(rel.find("SIGHMAKE_CXX_STAMP") != std::string::npos);
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

// Static libraries are compiled -fPIC only when a shared library of the solution links them.
static const char* kMakefilePicGraph = R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:ExeOnly]
type = lib
sources = exeonly.cpp

[project:OrderOnly]
type = lib
sources = orderonly.cpp

[project:Inner]
type = lib
sources = inner.cpp

[project:IntoShared]
type = lib
sources = intoshared.cpp
target_link_libraries(PRIVATE Inner)

[project:Plugin]
type = dll
sources = plugin.cpp
dependencies = OrderOnly
target_link_libraries(PRIVATE IntoShared)

[project:App]
type = exe
sources = app.cpp
target_link_libraries(PRIVATE ExeOnly, OrderOnly, Plugin)
)";

static const std::vector<std::string> kMakefilePicSources = {
    "exeonly.cpp", "orderonly.cpp", "inner.cpp", "intoshared.cpp", "plugin.cpp", "app.cpp"};

static std::string cxxflags_line(const std::string& makefile) {
    const size_t start = makefile.find("\nCXXFLAGS = ");
    if (start == std::string::npos) return {};
    return makefile.substr(start + 1, makefile.find('\n', start + 1) - start - 1);
}

TEST_CASE("MakefileGenerator static lib linked only into executables is not -fPIC", "[makefile_generator]") {
    auto result = generate_makefile(kMakefilePicGraph, kMakefilePicSources);
    REQUIRE(result.files.count("ExeOnly.Release"));
    REQUIRE(result.files.count("OrderOnly.Release"));
    REQUIRE(result.files.count("App.Release"));
    for (const char* name : {"ExeOnly.Release", "OrderOnly.Release", "App.Release"}) {
        INFO(name);
        const std::string& mk = result.files[name];
        CHECK(!cxxflags_line(mk).empty());
        CHECK(mk.find("-fPIC") == std::string::npos);
        CHECK(mk.find("semantic-interposition") == std::string::npos);
        CHECK(mk.find("SIGHMAKE_NO_SEMANTIC_INTERPOSITION") == std::string::npos);
    }
}

TEST_CASE("MakefileGenerator static lib linked into a shared lib is -fPIC, transitively", "[makefile_generator]") {
    auto result = generate_makefile(kMakefilePicGraph, kMakefilePicSources);
    for (const char* name : {"IntoShared.Release", "Inner.Release", "Plugin.Release"}) {
        INFO(name);
        REQUIRE(result.files.count(name));
        const std::string flags = cxxflags_line(result.files[name]);
        CHECK(flags.find("-fPIC") != std::string::npos);
        CHECK(flags.find("$(SIGHMAKE_NO_SEMANTIC_INTERPOSITION)") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator adds -fno-semantic-interposition to PIC targets", "[makefile_generator]") {
    auto result = generate_makefile(kMakefilePicGraph, kMakefilePicSources);
    REQUIRE(result.files.count("Plugin.Release"));
    const std::string& mk = result.files["Plugin.Release"];
#ifdef __APPLE__
    // Mach-O has no ELF interposition model; the flag is not used there.
    CHECK(mk.find("semantic-interposition") == std::string::npos);
#else
    // Default GCC toolchain: the flag is set directly and can be overridden on the make command line.
    CHECK(mk.find("SIGHMAKE_NO_SEMANTIC_INTERPOSITION ?= -fno-semantic-interposition") != std::string::npos);
    const size_t def = mk.find("SIGHMAKE_NO_SEMANTIC_INTERPOSITION ?=");
    const size_t use = mk.find("\nCXXFLAGS = ");
    CHECK(def < use);
#endif
}

TEST_CASE("MakefileGenerator probes clang for -fno-semantic-interposition", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux, Android

[project:Engine]
type = dll
sources = main.cpp
)");
    REQUIRE(result.files.count("Engine.Release.Android"));
    const std::string& mk = result.files["Engine.Release.Android"];
    // NDK r19-r21 ship clang 8/9, which reject the flag: the compiler is asked first.
    CHECK(mk.find("ifeq ($(origin SIGHMAKE_NO_SEMANTIC_INTERPOSITION),undefined)") != std::string::npos);
    CHECK(mk.find("-fno-semantic-interposition -fsyntax-only") != std::string::npos);
    CHECK(cxxflags_line(mk).find("$(SIGHMAKE_NO_SEMANTIC_INTERPOSITION)") != std::string::npos);
}

TEST_CASE("MakefileGenerator position_independent override", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:ForcedOn]
type = lib
sources = on.cpp
position_independent = true

[project:ForcedOff]
type = lib
sources = off.cpp
position_independent = false

[project:PieApp]
type = exe
sources = app.cpp
position_independent = true
target_link_libraries(PRIVATE ForcedOn)

[project:Plugin]
type = dll
sources = plugin.cpp
position_independent = false
target_link_libraries(PRIVATE ForcedOff)
)", {"on.cpp", "off.cpp", "app.cpp", "plugin.cpp"});
    REQUIRE(result.files.count("ForcedOn.Release"));
    REQUIRE(result.files.count("ForcedOff.Release"));
    REQUIRE(result.files.count("PieApp.Release"));
    REQUIRE(result.files.count("Plugin.Release"));
    CHECK(cxxflags_line(result.files["ForcedOn.Release"]).find("-fPIC") != std::string::npos);
    CHECK(cxxflags_line(result.files["PieApp.Release"]).find("-fPIC") != std::string::npos);
    // false wins over the graph for a static library ...
    CHECK(cxxflags_line(result.files["ForcedOff.Release"]).find("-fPIC") == std::string::npos);
    // ... but a shared library always needs position-independent code.
    CHECK(cxxflags_line(result.files["Plugin.Release"]).find("-fPIC") != std::string::npos);
}

TEST_CASE("MakefileGenerator semantic_interposition = true opts out", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Preloadable]
type = dll
sources = main.cpp
semantic_interposition = true
)");
    REQUIRE(result.files.count("Preloadable.Release"));
    const std::string& mk = result.files["Preloadable.Release"];
    CHECK(cxxflags_line(mk).find("-fPIC") != std::string::npos);
    CHECK(mk.find("semantic-interposition") == std::string::npos);
    CHECK(mk.find("SIGHMAKE_NO_SEMANTIC_INTERPOSITION") == std::string::npos);
}

TEST_CASE("BuildscriptParser warns on invalid position_independent and falls back to auto", "[makefile_generator]") {
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Lib]
type = lib
position_independent = sometimes
)", fs::temp_directory_path().string());
    REQUIRE(!solution.projects.empty());
    CHECK(solution.projects[0].position_independent == "auto");
}

// ============================================================================
// Driver (sys) type
// ============================================================================

TEST_CASE("MakefileGenerator driver produces linked binary", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:MyDriver]
type = sys
sources = main.cpp
)");
    if (!result.content.empty()) {
        // Driver should produce a linked binary like Application, not ar rcs like StaticLibrary
        CHECK(result.content.find("ar rcs") == std::string::npos);
        CHECK(result.content.find("-o $@") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator driver target has .sys extension", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:MyDriver]
type = sys
sources = main.cpp
)");
    if (!result.content.empty()) {
        CHECK(result.content.find(".sys") != std::string::npos);
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

TEST_CASE("MakefileGenerator master Makefile exposes project dependency targets", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Core]
type = lib
sources = core.cpp

[project:Util]
type = lib
sources = util.cpp

[project:App]
type = exe
sources = app.cpp
depends = Core

[project:Tool]
type = exe
sources = tool.cpp
depends = Util
)", {"core.cpp", "util.cpp", "app.cpp", "tool.cpp"});

    REQUIRE(!result.master_content.empty());
    CHECK(result.master_content.find("Release: Core.Release Util.Release App.Release Tool.Release") != std::string::npos);
    CHECK(result.master_content.find("App.Release: Core.Release") != std::string::npos);
    CHECK(result.master_content.find("Tool.Release: Util.Release") != std::string::npos);
    CHECK(result.master_content.find("\nRelease:\n\t$(MAKE)") == std::string::npos);
}

TEST_CASE("MakefileGenerator relinks targets when project libraries change", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Core]
type = lib
sources = core.cpp
outdir = bin
intdir = obj/Core

[project:App]
type = exe
sources = app.cpp
outdir = bin
intdir = obj/App

target_link_libraries(
    Core PRIVATE
)
)", {"core.cpp", "app.cpp"});

    REQUIRE(result.files.count("App.Release"));
    const std::string& app_makefile = result.files["App.Release"];
    CHECK(app_makefile.find("PROJECT_DEPS =") != std::string::npos);
    CHECK(app_makefile.find("$(TARGET): $(OBJS) $(PROJECT_DEPS)") != std::string::npos);

#ifndef _WIN32
    std::ofstream(result.temp_dir / "core.cpp") << "int core() { return 1; }\n";
    std::ofstream(result.temp_dir / "app.cpp")
        << "extern int core(); int main() { return core() == 1 ? 0 : 1; }\n";

    const std::string make_command = "make -C \"" +
        (result.temp_dir / "build").string() + "\" Release >/dev/null";
    REQUIRE(std::system(make_command.c_str()) == 0);

    const fs::path app_binary = result.temp_dir / "bin" / "App";
    REQUIRE(fs::exists(app_binary));
    const auto initial_write_time = fs::last_write_time(app_binary);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(std::system(make_command.c_str()) == 0);
    CHECK((fs::last_write_time(app_binary) == initial_write_time));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::ofstream(result.temp_dir / "core.cpp") << "int core() { return 2; }\n";
    REQUIRE(std::system(make_command.c_str()) == 0);
    CHECK((fs::last_write_time(app_binary) > initial_write_time));
#endif
}

TEST_CASE("MakefileGenerator propagates private static-library link requirements", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:ManagedRuntime]
type = lib
sources = managed.cpp

[project:Runtime]
type = lib
sources = runtime.cpp
target_link_libraries(PRIVATE ManagedRuntime)

[project:Host]
type = dll
sources = host.cpp
target_link_libraries(PRIVATE Runtime)
)", {"managed.cpp", "runtime.cpp", "host.cpp"});

    REQUIRE(result.files.count("Host.Release"));
    const std::string& host_makefile = result.files["Host.Release"];
    const size_t runtime = host_makefile.find("Runtime.a");
    const size_t managed = host_makefile.find("ManagedRuntime.a");
    REQUIRE(runtime != std::string::npos);
    REQUIRE(managed != std::string::npos);
    CHECK(runtime < managed);
}

#ifndef _WIN32
TEST_CASE("MakefileGenerator reconciles symlink aliases in generated paths", "[makefile_generator]") {
    const fs::path test_root = fs::temp_directory_path() / "sighmake_test_makefile_symlink_alias";
    struct TempDirCleanup {
        fs::path path;
        ~TempDirCleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{test_root};

    std::error_code ec;
    fs::remove_all(test_root, ec);
    const fs::path real_dir = test_root / "real";
    const fs::path alias_dir = test_root / "alias";
    fs::create_directories(real_dir);
    fs::create_directory_symlink(real_dir, alias_dir, ec);
    REQUIRE_FALSE(ec);

    std::ofstream(real_dir / "main.cpp") << "int main() { return 0; }\n";

    BuildscriptParser parser;
    auto solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
outdir = bin
intdir = obj/App
)", alias_dir.string());

    MakefileGenerator generator;
    REQUIRE(generator.generate(solution, alias_dir.string()));

    const std::string makefile = read_file(real_dir / "build" / "App.Release");
    CHECK(makefile.find("../main.cpp") != std::string::npos);
}
#endif

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

TEST_CASE("MakefileGenerator creates unique object paths for same-stem sources", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = src/main.cpp, tools/main.cpp
)", {"src/main.cpp", "tools/main.cpp"});

    REQUIRE(!result.content.empty());
    CHECK(result.content.find("/src/main_") != std::string::npos);
    CHECK(result.content.find("/tools/main_") != std::string::npos);
    CHECK(result.content.find("main.o") == std::string::npos);
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
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
language = C
c_standard = 11
sources = main.c
)", {"main.c"});

    CHECK(result.content.find("-std=c11") != std::string::npos);
}

TEST_CASE("MakefileGenerator mixed C and C++ projects use separate standards", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
std = 20
c_standard = 11
sources = main.cpp, helper.c
)", {"main.cpp", "helper.c"});

    CHECK(result.content.find("CXXFLAGS = -std=c++20") != std::string::npos);
    CHECK(result.content.find("CFLAGS = -std=c11") != std::string::npos);
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

TEST_CASE("MakefileGenerator includes --gc-sections for linker optimization", "[makefile_generator][gnu_ld]") {
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

TEST_CASE("MakefileGenerator emits base_address in linker flags", "[makefile_generator][gnu_ld]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = dll
sources = main.cpp
base_address = 0x10000000
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("--image-base=0x10000000") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator emits -nodefaultlibs for ignore_all_default_libraries", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
ignore_all_default_libraries = true
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("-nodefaultlibs") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator emits version-script for module_def", "[makefile_generator][gnu_ld]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = dll
sources = main.cpp
module_def = exports.def
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("--version-script=") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator emits prebuild and postbuild events", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
prebuild = echo prebuild_step
postbuild = echo postbuild_step
)");
    if (!result.content.empty()) {
        CHECK(result.content.find("prebuild_step") != std::string::npos);
        CHECK(result.content.find("postbuild_step") != std::string::npos);
        CHECK(result.content.find("prebuild") != std::string::npos);
        CHECK(result.content.find(".DEFAULT_GOAL := all") != std::string::npos);
        CHECK(result.content.find("$(OBJS): | prebuild") != std::string::npos);
        CHECK(result.content.find("$(TARGET): $(OBJS) | prebuild") != std::string::npos);
        CHECK(result.content.find("$(TARGET): prebuild $(OBJS)") == std::string::npos);

        const auto default_goal = result.content.find(".DEFAULT_GOAL := all");
        const auto prebuild_target = result.content.find("prebuild:");
        CHECK(default_goal < prebuild_target);
    }
}

TEST_CASE("MakefileGenerator keeps phony prebuild order-only", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
prebuild = echo prebuild_step
)");
    REQUIRE(!result.content.empty());
    CHECK(result.content.find("$(TARGET): $(OBJS) | prebuild") != std::string::npos);
    CHECK(result.content.find("$(TARGET): prebuild $(OBJS)") == std::string::npos);
#ifdef __APPLE__
    CHECK(result.content.find("-o $@ $(OBJS) $(LDLIBS)") != std::string::npos);
#else
    CHECK(result.content.find("-o $@ $(OBJS) -Wl,--start-group $(LDLIBS) -Wl,--end-group") != std::string::npos);
#endif
    CHECK(result.content.find("$^") == std::string::npos);
}

TEST_CASE("MakefileGenerator uses CXX to link Objective-C++ targets", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.mm
)", {"main.mm"});

    REQUIRE(!result.content.empty());
    CHECK(result.content.find("CXX = ") != std::string::npos);
    CHECK(result.content.find("\t$(CXX) $(LDFLAGS) -o $@") != std::string::npos);
}

TEST_CASE("MakefileGenerator quotes include paths with spaces", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
includes = path with spaces/include
)");
    if (!result.content.empty()) {
        // Include path should be quoted: -I"path..."
        CHECK(result.content.find("-I\"") != std::string::npos);
    }
}

TEST_CASE("MakefileGenerator quotes library paths with spaces", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
libdirs = path with spaces/lib
)");
    if (!result.content.empty()) {
        // Library path should be quoted: -L"path..."
        CHECK(result.content.find("-L\"") != std::string::npos);
    }
}

TEST_CASE("Master Makefile installs the generated shared-library name", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Plugin]
type = dll
sources = main.cpp
target_name = renderer
target_ext = .plugin
outdir = bin
)");

    REQUIRE(!result.master_content.empty());
    CHECK(result.master_content.find("renderer.plugin") != std::string::npos);
    CHECK(result.master_content.find("librenderer") == std::string::npos);
}

// ============================================================================
// Android (NDK) support
// ============================================================================

TEST_CASE("MakefileGenerator emits NDK toolchain for Android platform", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:App]
type = exe
sources = main.cpp
std = 17
)");
    REQUIRE(result.files.count("App.Debug.Android"));
    const std::string& mk = result.files["App.Debug.Android"];
    CHECK(mk.find("ANDROID_ABI ?= arm64-v8a") != std::string::npos);
    CHECK(mk.find("ANDROID_API ?= 24") != std::string::npos);
    CHECK(mk.find("toolchains/llvm/prebuilt/") != std::string::npos);
    CHECK(mk.find("--target=$(ANDROID_TRIPLE)$(ANDROID_API)") != std::string::npos);
    CHECK(mk.find("aarch64-linux-android") != std::string::npos);
    CHECK(mk.find("armv7a-linux-androideabi") != std::string::npos);
    // Host compilers must not leak into Android makefiles
    CHECK(mk.find("CXX = g++") == std::string::npos);
    CHECK(mk.find("CXX = clang++\n") == std::string::npos);
}

TEST_CASE("MakefileGenerator Android objects are ABI-scoped", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:App]
type = exe
sources = main.cpp
)");
    REQUIRE(result.files.count("App.Debug.Android"));
    const std::string& mk = result.files["App.Debug.Android"];
    // Default out/int dirs gain an android/$(ANDROID_ABI) subdirectory
    CHECK(mk.find("android/$(ANDROID_ABI)") != std::string::npos);
}

TEST_CASE("MakefileGenerator Android shared library gets lib prefix and .so", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:Engine]
type = dll
sources = main.cpp
)");
    REQUIRE(result.files.count("Engine.Debug.Android"));
    const std::string& mk = result.files["Engine.Debug.Android"];
    CHECK(mk.find("libEngine.so") != std::string::npos);
    CHECK(mk.find("-shared") != std::string::npos);
    CHECK(mk.find(".dylib") == std::string::npos);
}

TEST_CASE("MakefileGenerator Android static library uses llvm-ar", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:Core]
type = lib
sources = main.cpp
)");
    REQUIRE(result.files.count("Core.Debug.Android"));
    const std::string& mk = result.files["Core.Debug.Android"];
    // Objects go through a response file so long lists fit the Windows command-line limit.
    CHECK(mk.find("$(file >$@.rsp,$(OBJS))") != std::string::npos);
    CHECK(mk.find("$(AR) rcs $@ @$@.rsp") != std::string::npos);
    CHECK(mk.find("$(AR) rcs $@ $(OBJS)") == std::string::npos);
    CHECK(mk.find("llvm-ar") != std::string::npos);
}

TEST_CASE("MakefileGenerator Android Release strips with llvm-strip", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:App]
type = exe
sources = main.cpp
)");
    REQUIRE(result.files.count("App.Release.Android"));
    const std::string& mk = result.files["App.Release.Android"];
    CHECK(mk.find("$(STRIP) --strip-unneeded $@") != std::string::npos);
    CHECK(mk.find("llvm-strip") != std::string::npos);
}

TEST_CASE("Master Makefile exposes Android targets", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:App]
type = exe
sources = main.cpp
)");
    REQUIRE(!result.master_content.empty());
    CHECK(result.master_content.find("android: Debug.Android") != std::string::npos);
    CHECK(result.master_content.find("Debug.Android: App.Debug.Android") != std::string::npos);
    CHECK(result.master_content.find("$(MAKE) -f App.Debug.Android") != std::string::npos);
    // Android-only solution: `make` defaults to the Android build
    CHECK(result.master_content.find("all: Debug.Android") != std::string::npos);
}

TEST_CASE("Linux and Android configs coexist without colliding", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Linux, Android

[project:App]
type = exe
sources = main.cpp
)");
    // Separate makefiles per platform flavor
    REQUIRE(result.files.count("App.Debug"));
    REQUIRE(result.files.count("App.Debug.Android"));
    CHECK(result.files["App.Debug"].find("ANDROID_TOOLCHAIN") == std::string::npos);
    CHECK(result.files["App.Debug.Android"].find("ANDROID_TOOLCHAIN") != std::string::npos);
    // Desktop config stays the default; Android is opt-in
    CHECK(result.master_content.find("all: Debug\n") != std::string::npos);
    CHECK(result.master_content.find("android: Debug.Android") != std::string::npos);
}

TEST_CASE("Master Makefile install skips Android configs", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:App]
type = exe
sources = main.cpp
)");
    REQUIRE(!result.master_content.empty());
    // No install rule for device binaries
    CHECK(result.master_content.find("install -m 755") == std::string::npos);
}

TEST_CASE("Android dependency archives resolve to ABI-scoped paths", "[makefile_generator][android]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:Core]
type = lib
sources = core.cpp

[project:App]
type = exe
sources = main.cpp

target_link_libraries(
    Core PUBLIC
)
)", {"main.cpp", "core.cpp"});
    REQUIRE(result.files.count("App.Debug.Android"));
    const std::string& mk = result.files["App.Debug.Android"];
    // The dependency archive lives in the ABI-scoped output directory
    CHECK(mk.find("android/$(ANDROID_ABI)/Core.a") != std::string::npos);
}

TEST_CASE("MakefileGenerator emits target receipt metadata and build hook", "[makefile_generator][receipt]") {
    fs::path temp_dir = fs::temp_directory_path() / "sighmake_test_makefile_receipt";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);

    fs::path main_cpp = temp_dir / "main.cpp";
    std::ofstream(main_cpp) << "int main() { return 0; }";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:RuntimeBase]
type = interface
runtime_dependencies = Codec|thirdparty/codec.so|Codec.so|true

[project:App]
type = exe
sources = main.cpp
target_link_libraries(PRIVATE RuntimeBase)
)", temp_dir.string());

    MakefileGenerator gen;
    REQUIRE(gen.generate(sol, temp_dir.string()));

    const fs::path build_dir = temp_dir / "build";
    REQUIRE(fs::exists(build_dir / "write-sighmake-target-receipt.py"));
    REQUIRE(fs::exists(build_dir / "Write-SighmakeTargetReceipt.ps1"));
    const std::string receipt_script = read_file(build_dir / "Write-SighmakeTargetReceipt.ps1");
    CHECK(receipt_script.find("Get-FileHash") == std::string::npos);
    CHECK(receipt_script.find("[IO.File]::OpenRead($Path)") != std::string::npos);
    CHECK(receipt_script.find("[uint64]$stream.Length") != std::string::npos);
    CHECK(receipt_script.find("$hash.ComputeHash($stream)") != std::string::npos);
    CHECK(receipt_script.find(".ToLowerInvariant()") != std::string::npos);
    CHECK(receipt_script.find("$stream.Dispose()") != std::string::npos);
    CHECK(receipt_script.find("$hash.Dispose()") != std::string::npos);
    REQUIRE(fs::exists(build_dir / "App.runtime-dependencies.txt"));

    const std::string metadata = read_file(build_dir / "App.runtime-dependencies.txt");
    CHECK(metadata.find("Codec|") != std::string::npos);
    CHECK(metadata.find("|Codec.so|1") != std::string::npos);

    const std::string mk = read_file(build_dir / "App.Release");
    CHECK(mk.find("SIGHMAKE_TARGET_RECEIPT = ") != std::string::npos);
    CHECK(mk.find("SIGHMAKE_RECEIPT_ARCHITECTURE ?= $(shell uname -m)") != std::string::npos);
    CHECK(mk.find(".PHONY: all clean sighmake-target-receipt") != std::string::npos);
    CHECK(mk.find("all: sighmake-target-receipt") != std::string::npos);
    CHECK(mk.find("sighmake-target-receipt: $(TARGET)") != std::string::npos);
    CHECK(mk.find("write-sighmake-target-receipt.py") != std::string::npos);
    CHECK(mk.find("App.runtime-dependencies.txt") != std::string::npos);
    CHECK(mk.find(".targetreceipt.json") != std::string::npos);
    CHECK(mk.find("--primary-artifact \"$(TARGET)\"") != std::string::npos);
    CHECK(mk.find("--output \"$(SIGHMAKE_TARGET_RECEIPT)\"") != std::string::npos);
    CHECK(mk.find("rm -rf $(OBJ_DIR) $(TARGET) $(SIGHMAKE_TARGET_RECEIPT)") != std::string::npos);

    fs::remove_all(temp_dir, ec);
}

TEST_CASE("MakefileGenerator names shared-library receipts after the logical target",
          "[makefile_generator][receipt]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = macOS

[project:EngineRuntime]
type = dll
sources = runtime.cpp
target_name = libEngineRuntime
)", {"runtime.cpp"});

    REQUIRE(result.files.count("EngineRuntime.Release"));
    const std::string& makefile = result.files["EngineRuntime.Release"];
    CHECK(makefile.find("EngineRuntime.targetreceipt.json") != std::string::npos);
    CHECK(makefile.find("libEngineRuntime.targetreceipt.json") == std::string::npos);
}

TEST_CASE("MakefileGenerator limits target receipts to executable and shared-library targets",
          "[makefile_generator][receipt]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Core]
type = lib
sources = main.cpp
)");

    REQUIRE(result.files.count("Core.Release"));
    CHECK_FALSE(result.files.count("write-sighmake-target-receipt.py"));
    CHECK_FALSE(result.files.count("Write-SighmakeTargetReceipt.ps1"));
    CHECK_FALSE(result.files.count("Core.runtime-dependencies.txt"));
    CHECK(result.files["Core.Release"].find("sighmake-target-receipt") == std::string::npos);
}

TEST_CASE("MakefileGenerator uses the Android ABI in target receipts", "[makefile_generator][receipt]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:Plugin]
type = dll
sources = main.cpp
)");

    REQUIRE(result.files.count("Plugin.Debug.Android"));
    const std::string& mk = result.files["Plugin.Debug.Android"];
    CHECK(mk.find("SIGHMAKE_RECEIPT_ARCHITECTURE ?= $(ANDROID_ABI)") != std::string::npos);
    CHECK(mk.find("--platform \"Android\"") != std::string::npos);
    CHECK(mk.find("--architecture \"$(SIGHMAKE_RECEIPT_ARCHITECTURE)\"") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("Makefile target receipts revalidate incremental builds and clean up", "[makefile_generator][receipt]") {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temp_dir = fs::temp_directory_path() /
        ("sighmake receipt lifecycle " + std::to_string(unique));
    struct TempDirCleanup {
        fs::path path;
        ~TempDirCleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{temp_dir};

    fs::create_directories(temp_dir / "runtime assets");
    std::ofstream(temp_dir / "main.cpp") << "int main() { return 0; }\n";
    const fs::path required_dependency = temp_dir / "runtime assets" / "codec.so";
    const fs::path optional_dependency = temp_dir / "runtime assets" / "optional.so";
    std::ofstream(required_dependency) << "required-v1\n";

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug
platforms = Linux

[project:RuntimeBase]
type = interface
runtime_dependencies = Codec|runtime assets/codec.so|Codec.so|true
runtime_dependencies = Optional|runtime assets/optional.so|plugins/optional.so|false

[project:App]
type = exe
sources = main.cpp
outdir = artifacts
intdir = objects
target_link_libraries(PRIVATE RuntimeBase)
)", temp_dir.string());

    MakefileGenerator generator;
    REQUIRE(generator.generate(sol, temp_dir.string()));

    const fs::path build_dir = temp_dir / "build";
    const fs::path binary = temp_dir / "artifacts" / "App";
    const fs::path receipt = temp_dir / "artifacts" / "App.targetreceipt.json";
    const std::string make_command = "make -C \"" + build_dir.string() +
        "\" -f App.Debug SIGHMAKE_RECEIPT_ARCHITECTURE=aarch64 >/dev/null 2>&1";
    const std::string clean_command = "make -C \"" + build_dir.string() +
        "\" -f App.Debug clean >/dev/null 2>&1";

    REQUIRE(std::system(make_command.c_str()) == 0);
    REQUIRE(fs::exists(binary));
    REQUIRE(fs::exists(receipt));
    const auto binary_write_time = fs::last_write_time(binary);
    const std::string initial_receipt = read_file(receipt);
    CHECK(initial_receipt.find("\"FormatVersion\": 2") != std::string::npos);
    CHECK(initial_receipt.find("\"PrimaryArtifactUnixMode\":") != std::string::npos);
    CHECK(initial_receipt.find("\"UnixMode\":") != std::string::npos);
    CHECK(initial_receipt.find("\"Architecture\": \"ARM64\"") != std::string::npos);
    CHECK(initial_receipt.find("\"SkippedRuntimeDependencies\": [") != std::string::npos);
    CHECK(initial_receipt.find("\"Name\": \"Optional\"") != std::string::npos);

    // A newly-present optional dependency must refresh the receipt without relinking the target.
    std::ofstream(optional_dependency) << "optional-v1\n";
    REQUIRE(std::system(make_command.c_str()) == 0);
    CHECK((fs::last_write_time(binary) == binary_write_time));
    const std::string optional_present_receipt = read_file(receipt);
    CHECK(optional_present_receipt != initial_receipt);
    CHECK(optional_present_receipt.find("\"StagePath\": \"plugins/optional.so\"") != std::string::npos);
    CHECK(optional_present_receipt.find("\"SkippedRuntimeDependencies\": []") != std::string::npos);

    // Runtime content changes are rehashed even though they are not link prerequisites.
    std::ofstream(required_dependency, std::ios::trunc) << "required-v2\n";
    REQUIRE(std::system(make_command.c_str()) == 0);
    CHECK((fs::last_write_time(binary) == binary_write_time));
    const std::string dependency_changed_receipt = read_file(receipt);
    CHECK(dependency_changed_receipt != optional_present_receipt);

    // A failed receipt remains retryable on an otherwise up-to-date binary.
    std::error_code ec;
    REQUIRE(fs::remove(required_dependency, ec));
    REQUIRE(std::system(make_command.c_str()) != 0);
    REQUIRE(std::system(make_command.c_str()) != 0);
    CHECK((fs::last_write_time(binary) == binary_write_time));
    std::ofstream(required_dependency) << "required-v3\n";
    REQUIRE(std::system(make_command.c_str()) == 0);

    REQUIRE(std::system(clean_command.c_str()) == 0);
    CHECK_FALSE(fs::exists(binary));
    CHECK_FALSE(fs::exists(receipt));
}
#endif

// ============================================================================
// Target OS (--target-os / SIGHMAKE_TARGET_OS)
// ============================================================================

namespace {
struct MakefileTargetOsScope {
    explicit MakefileTargetOsScope(TargetOS os) { set_target_os(os); }
    ~MakefileTargetOsScope() { set_target_os(std::nullopt); }
};
}

TEST_CASE("MakefileGenerator link rule has no self-referencing prerequisite", "[makefile_generator]") {
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    REQUIRE(result.files.count("App.Release"));
    const std::string& mk = result.files["App.Release"];
    CHECK(mk.find("$(TARGET): $(OBJS)\n") != std::string::npos);
    CHECK(mk.find("$(OBJS) App.Release") == std::string::npos);
}

TEST_CASE("MakefileGenerator generates Android from conditions evaluated for the target OS", "[makefile_generator][android][target_os]") {
    MakefileTargetOsScope scope(TargetOS::Android);
    auto result = generate_makefile(R"(
[solution]
name = Test
configurations = Debug
platforms = Android

[project:Core]
type = lib
sources = main.cpp
if(windows) {
    defines = HOST_WINDOWS_ONLY
}
if(posix) {
    defines = TARGET_POSIX
}
)");
    REQUIRE(result.files.count("Core.Debug.Android"));
    const std::string& mk = result.files["Core.Debug.Android"];
    CHECK(mk.find("TARGET_POSIX") != std::string::npos);
    CHECK(mk.find("HOST_WINDOWS_ONLY") == std::string::npos);
    CHECK(mk.find("$(AR) rcs $@ @$@.rsp") != std::string::npos);
    CHECK(mk.find("$(OBJS) Core.Debug") == std::string::npos);
}


// ============================================================================
// Command-line stamps: objects/links must rebuild when flags change
// ============================================================================

namespace {

// sighmake resolves default output directories against the process working directory, so tests
// that look at generated outputs run with the temp directory as the working directory.
struct ScopedCwd {
    fs::path previous;
    explicit ScopedCwd(const fs::path& dir) : previous(fs::current_path()) { fs::current_path(dir); }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

fs::path find_file_matching(const fs::path& root, const std::string& prefix, const std::string& suffix) {
    fs::path found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string name = it->path().filename().string();
        if (name.size() >= prefix.size() + suffix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = it->path();
        }
    }
    return found;
}

std::string stamp_test_script(const std::string& define, const std::string& extra) {
    return std::string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
defines = )") + define + "\n" + extra + "\n";
}

bool generate_stamp_test(const fs::path& dir, const std::string& text) {
    BuildscriptParser parser;
    Solution sol = parser.parse_string(text, dir.string());
    MakefileGenerator generator;
    return generator.generate(sol, dir.string());
}

} // namespace

TEST_CASE("MakefileGenerator objects depend on generation-time command-line stamps", "[makefile_generator][stamp]") {
    const fs::path dir = fs::temp_directory_path() / "sighmake_test_stamps_struct";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "main.cpp") << "int main() { return 0; }\n";
    ScopedCwd cwd(dir);

    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=1", "")));
    const std::string mk = read_file(dir / "build" / "App.Release");
    INFO(mk);
    // Stamp variable, ordinary prerequisites, no work at make time beyond creating a missing stamp.
    CHECK(mk.find("SIGHMAKE_CXX_STAMP = $(OBJ_DIR)sighmake-cxx.Release-Linux.flags") != std::string::npos);
    CHECK(mk.find("main.cpp $(SIGHMAKE_CXX_STAMP)") != std::string::npos);
    CHECK(mk.find("$(TARGET): $(SIGHMAKE_LINK_STAMP)") != std::string::npos);
    CHECK(mk.find("SIGHMAKE_FORCE") == std::string::npos);
    CHECK(mk.find("cmp -s") == std::string::npos);
    CHECK(mk.find("SIGHMAKE_C_STAMP") == std::string::npos);
    // Desktop Makefiles never mention Android link flags.
    CHECK(mk.find("ANDROID_LINK_FLAGS") == std::string::npos);

    // The stamp files hold the command line and only change when it changes.
    const fs::path cxx_stamp = find_file_matching(dir, "sighmake-cxx.", ".flags");
    const fs::path link_stamp = find_file_matching(dir, "sighmake-link.", ".flags");
    REQUIRE(!cxx_stamp.empty());
    REQUIRE(!link_stamp.empty());
    const std::string cxx_text = read_file(cxx_stamp);
    CHECK(cxx_text.find("-DFOO=1") != std::string::npos);
    const auto cxx_time = fs::last_write_time(cxx_stamp);
    const auto link_time = fs::last_write_time(link_stamp);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=1", "")));
    CHECK((fs::last_write_time(cxx_stamp) == cxx_time));
    CHECK((fs::last_write_time(link_stamp) == link_time));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=2", "")));
    CHECK(read_file(cxx_stamp).find("-DFOO=2") != std::string::npos);
    CHECK((fs::last_write_time(cxx_stamp) != cxx_time));
    CHECK((fs::last_write_time(link_stamp) == link_time));  // compile-only change: link stamp untouched

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto cxx_time2 = fs::last_write_time(cxx_stamp);
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=2", "ldflags = -g")));
    CHECK((fs::last_write_time(cxx_stamp) == cxx_time2));
    CHECK((fs::last_write_time(link_stamp) != link_time));
    CHECK(read_file(link_stamp).find("-g") != std::string::npos);
}

#ifndef _WIN32
static bool tool_available(const char* cmd) {
    return std::system((std::string("command -v ") + cmd + " >/dev/null 2>&1").c_str()) == 0;
}

TEST_CASE("MakefileGenerator rebuilds objects and relinks when flags change", "[makefile_generator][stamp]") {
    if (!tool_available("make") || !tool_available("c++")) {
        SKIP("make or c++ not available");
    }
    const fs::path dir = fs::temp_directory_path() / "sighmake_test_stamps_run";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "main.cpp") << "#include <cstdio>\nint main() { std::printf(\"%d\", FOO); return 0; }\n";
    ScopedCwd cwd(dir);

    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=1", "")));
    const std::string make_cmd = "make -C \"" + (dir / "build").string() + "\" Release >/dev/null 2>&1";
    const std::string make_j = "make -j4 -C \"" + (dir / "build").string() + "\" Release >/dev/null 2>&1";
    REQUIRE(std::system(make_cmd.c_str()) == 0);

    const fs::path obj = find_file_matching(dir, "main", ".o");
    fs::path app;  // the linked binary, not the App.Release Makefile
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().filename() == "App") app = it->path();
    }
    REQUIRE(!obj.empty());
    REQUIRE(!app.empty());
    const auto obj_time = fs::last_write_time(obj);
    const auto app_time = fs::last_write_time(app);

    // Regenerating with the same flags rebuilds nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=1", "")));
    REQUIRE(std::system(make_cmd.c_str()) == 0);
    CHECK((fs::last_write_time(obj) == obj_time));
    CHECK((fs::last_write_time(app) == app_time));

    // Changed define, source untouched: the object and the binary rebuild.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=2", "")));
    REQUIRE(std::system(make_j.c_str()) == 0);
    CHECK((fs::last_write_time(obj) > obj_time));
    CHECK((fs::last_write_time(app) > app_time));

    // Link flags only: the object stays, the binary relinks.
    const auto obj_time2 = fs::last_write_time(obj);
    const auto app_time2 = fs::last_write_time(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(generate_stamp_test(dir, stamp_test_script("FOO=2", "ldflags = -g")));
    REQUIRE(std::system(make_j.c_str()) == 0);
    CHECK((fs::last_write_time(obj) == obj_time2));
    CHECK((fs::last_write_time(app) > app_time2));

    // A missing stamp (make clean removes the intermediate directory) must not break the build.
    REQUIRE(std::system(("make -C \"" + (dir / "build").string() + "\" clean >/dev/null 2>&1").c_str()) == 0);
    REQUIRE(std::system(make_j.c_str()) == 0);
    CHECK(fs::exists(app));
}
#endif
