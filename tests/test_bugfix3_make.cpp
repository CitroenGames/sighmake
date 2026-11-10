#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/cmake_generator.hpp"
#include "parsers/cmake_parser.hpp"

#include <cstdlib>

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string bugfix3_make_read(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Generates makefiles for a buildscript into a fresh temp directory holding the given files. The
// directory name may contain spaces.
struct Bugfix3MakeProject {
    fs::path dir;
    Solution solution;
    bool generated = false;

    Bugfix3MakeProject(const std::string& name, const std::string& buildscript,
                       const std::map<std::string, std::string>& files) {
        dir = fs::temp_directory_path() / ("sighmake bugfix3 make " + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        for (const auto& [path, content] : files) {
            fs::create_directories((dir / path).parent_path());
            std::ofstream(dir / path, std::ios::binary) << content;
        }
        BuildscriptParser parser;
        solution = parser.parse_string(buildscript, dir.string());
        MakefileGenerator generator;
        generated = generator.generate(solution, dir.string());
    }
    ~Bugfix3MakeProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string makefile(const std::string& name) const { return bugfix3_make_read(dir / "build" / name); }
    int make(const std::string& goal) const {
        const std::string cmd = "make -C \"" + (dir / "build").string() + "\" " + goal + " >/dev/null 2>&1";
        const int status = std::system(cmd.c_str());
        return status;
    }
};

#ifndef _WIN32
bool bugfix3_tool_available(const char* tool) {
    const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    const int status = std::system(cmd.c_str());
    return status == 0;
}

bool bugfix3_toolchain_available() {
#ifdef __APPLE__
    const char* cxx = "clang++";
#else
    const char* cxx = "g++";
#endif
    return bugfix3_tool_available("make") && bugfix3_tool_available(cxx) && bugfix3_tool_available("ar") &&
           bugfix3_tool_available("strip") && bugfix3_tool_available("python3");
}

int bugfix3_run(const std::string& command) {
    const int status = std::system((command + " >/dev/null 2>&1").c_str());
    return status;
}
#endif

} // namespace

TEST_CASE("Makefile builds when output, intermediate and build directories contain spaces", "[bugfix3_make]") {
    Bugfix3MakeProject p("spaces", R"(
[solution]
name = Sp
configurations = Release
platforms = Linux

[project:Core]
type = static
sources = src dir/core.cpp
outdir = out dir/lib s
intdir = int dir/core

[project:Sh]
type = shared
sources = src dir/sh.cpp
outdir = out dir/so s
intdir = int dir/sh

[project:App]
type = exe
sources = src dir/main.cpp, src dir/pch.cpp
pch = Use
pch_header = src dir/pch.h
pch.cpp:pch = Create
project_references = Core, Sh
outdir = out dir/bin s
intdir = int dir/app
postbuild = echo post > "post file.txt"
)", {{"src dir/pch.h", "#pragma once\n"},
     {"src dir/pch.cpp", "#include \"pch.h\"\n"},
     {"src dir/main.cpp", "#include \"pch.h\"\nint core();\nint sh();\nint main() { return core() + sh() == 3 ? 0 : 1; }\n"},
     {"src dir/core.cpp", "int core() { return 1; }\n"},
     {"src dir/sh.cpp", "int sh() { return 2; }\n"}});
    REQUIRE(p.generated);
    const std::string app = p.makefile("App.Release");
    CHECK(app.find("TARGET = ../out\\ dir/bin\\ s/App") != std::string::npos);
    CHECK(app.find("OBJ_DIR = ../int\\ dir/app/") != std::string::npos);
#ifndef _WIN32
    if (!bugfix3_toolchain_available()) {
        WARN("make/compiler/ar/strip/python3 not available: skipping the real build");
        return;
    }
    CHECK(p.make("Release") == 0);
    CHECK(fs::exists(p.dir / "out dir" / "lib s" / "Core.a"));
    CHECK(fs::exists(p.dir / "out dir" / "so s" / ("Sh" + std::string(
#ifdef __APPLE__
        ".dylib"
#else
        ".so"
#endif
        ))));
    CHECK(fs::exists(p.dir / "out dir" / "bin s" / "App"));
    CHECK(fs::exists(p.dir / "out dir" / "bin s" / "App.targetreceipt.json"));
    CHECK(fs::exists(p.dir / "build" / "post file.txt"));
    // The shared library is linked by its build-relative path: run from the build directory.
    CHECK(bugfix3_run("cd \"" + (p.dir / "build").string() + "\" && \"../out dir/bin s/App\"") == 0);

    const fs::path prefix = p.dir / "inst dir";
    CHECK(p.make("install PREFIX=\"" + prefix.string() + "\"") == 0);
    CHECK(fs::exists(prefix / "bin" / "App"));

    CHECK(p.make("clean") == 0);
    CHECK(!fs::exists(p.dir / "out dir" / "bin s" / "App"));
    CHECK(!fs::exists(p.dir / "out dir" / "bin s" / "App.targetreceipt.json"));
    CHECK(!fs::exists(p.dir / "out dir" / "lib s" / "Core.a"));
#endif
}

TEST_CASE("Makefile quotes whole-archive dependency paths containing spaces", "[bugfix3_make]") {
    Bugfix3MakeProject p("whole archive", R"(
[solution]
name = Wa
configurations = Debug
platforms = Linux

[project:Plugins]
type = static
sources = reg.cpp
outdir = lib dir
intdir = obj dir/plugins

[project:App]
type = exe
sources = main.cpp
outdir = bin dir
intdir = obj dir/app
target_link_libraries(
    PRIVATE WHOLE_ARCHIVE Plugins
)
)", {{"reg.cpp", "extern int plugin_flag;\nstatic int registered = (plugin_flag = 1, 0);\n"},
     {"main.cpp", "int plugin_flag = 0;\nint main() { return plugin_flag ? 0 : 1; }\n"}});
    REQUIRE(p.generated);
    const std::string app = p.makefile("App.Debug");
    CHECK(app.find("lib dir/Plugins.a\"") != std::string::npos);
    CHECK(app.find("lib dir/Plugins.a ") == std::string::npos);
#ifndef _WIN32
    if (!bugfix3_toolchain_available()) {
        WARN("make/compiler/ar/strip/python3 not available: skipping the real build");
        return;
    }
    CHECK(p.make("Debug") == 0);
    CHECK(bugfix3_run("\"" + (p.dir / "bin dir" / "App").string() + "\"") == 0);
#endif
}

TEST_CASE("Makefile builds sources whose names contain '#' and '$'", "[bugfix3_make]") {
    Bugfix3MakeProject p("special chars", R"(
[solution]
name = Sc
configurations = Debug
platforms = Linux

[project:App]
type = exe
sources = main.cpp, a#b.cpp, c$d.cpp
outdir = bin
intdir = obj
)", {{"main.cpp", "int ab();\nint cd();\nint main() { return ab() + cd() == 3 ? 0 : 1; }\n"},
     {"a#b.cpp", "int ab() { return 1; }\n"},
     {"c$d.cpp", "int cd() { return 2; }\n"}});
    REQUIRE(p.generated);
#ifndef _WIN32
    if (!bugfix3_toolchain_available()) {
        WARN("make/compiler/ar/strip/python3 not available: skipping the real build");
        return;
    }
    CHECK(p.make("Debug") == 0);
    CHECK(bugfix3_run("\"" + (p.dir / "bin" / "App").string() + "\"") == 0);
#endif
}

TEST_CASE("CMake root enables C when a C++ project also holds C sources", "[bugfix3_make]") {
    const fs::path dir = fs::temp_directory_path() / "sighmake bugfix3 cmake mixed";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src");
    std::ofstream(dir / "src" / "a.c") << "int c_fn(void) { return 1; }\n";
    std::ofstream(dir / "src" / "main.cpp") << "extern \"C\" int c_fn(void);\nint main() { return c_fn() == 1 ? 0 : 1; }\n";
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Mixed
configurations = Debug
platforms = Linux

[project:App]
type = exe
sources = src/a.c, src/main.cpp
)", dir.string());
    CMakeGenerator generator;
    REQUIRE(generator.generate(solution, dir.string()));
    const std::string root = bugfix3_make_read(dir / "CMakeLists.txt");
    CHECK(root.find("project(Mixed LANGUAGES C CXX)") != std::string::npos);
    fs::remove_all(dir, ec);
}

namespace {
const Project* bugfix3_find(const Solution& sol, const std::string& name) {
    for (const auto& proj : sol.projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}
bool bugfix3_has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
} // namespace

TEST_CASE("CMake reader applies plain target_compile_definitions to every configuration", "[bugfix3_make]") {
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE PLAIN=1 PUBLIC OTHER)
)");
    const Project* p = bugfix3_find(sol, "App");
    REQUIRE(p != nullptr);
    for (const auto& key : sol.get_config_keys()) {
        const auto& defs = p->configurations.at(key).cl_compile.preprocessor_definitions;
        CHECK(bugfix3_has(defs, "PLAIN=1"));
        CHECK(bugfix3_has(defs, "OTHER"));
    }
}

TEST_CASE("CMake reader directory-scope commands reach targets created afterwards", "[bugfix3_make]") {
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
include_directories(inc)
link_directories(libs)
add_definitions(-DFOO=1 -Wall)
add_compile_definitions(BAR)
add_compile_options(-Wextra)
add_executable(App main.cpp)
add_library(Lib STATIC lib.cpp)
)");
    for (const char* name : {"App", "Lib"}) {
        const Project* p = bugfix3_find(sol, name);
        REQUIRE(p != nullptr);
        for (const auto& key : sol.get_config_keys()) {
            const auto& cfg = p->configurations.at(key);
            CHECK(bugfix3_has(cfg.cl_compile.preprocessor_definitions, "FOO=1"));
            CHECK(bugfix3_has(cfg.cl_compile.preprocessor_definitions, "BAR"));
            CHECK(cfg.cl_compile.additional_options.find("-Wall") != std::string::npos);
            CHECK(cfg.cl_compile.additional_options.find("-Wextra") != std::string::npos);
            REQUIRE(cfg.cl_compile.additional_include_directories.size() == 1);
            CHECK(cfg.cl_compile.additional_include_directories[0].find("inc") != std::string::npos);
            REQUIRE(cfg.link.additional_library_directories.size() == 1);
        }
    }
}

TEST_CASE("CMake reader resolves ALIAS targets in target_link_libraries", "[bugfix3_make]") {
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
add_library(core STATIC core.cpp)
add_library(T::core ALIAS core)
add_executable(App main.cpp)
target_link_libraries(App PRIVATE T::core)
)");
    const Project* app = bugfix3_find(sol, "App");
    REQUIRE(app != nullptr);
    REQUIRE(app->project_references.size() == 1);
    CHECK(app->project_references[0].name == "core");
    CHECK(bugfix3_find(sol, "T_core") == nullptr);
    for (const auto& [key, cfg] : app->configurations) {
        (void)key;
        CHECK(cfg.link.additional_dependencies.empty());
    }
}

TEST_CASE("CMake reader runs include() files in the current scope", "[bugfix3_make]") {
    const fs::path dir = fs::temp_directory_path() / "sighmake bugfix3 cmake include";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "cmake");
    std::ofstream(dir / "cmake" / "extra.cmake") << "set(EXTRA_DEFS ONE TWO)\nadd_library(fromfile STATIC f.cpp)\n";
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
include(cmake/extra.cmake)
include(DoesNotExist OPTIONAL)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE ${EXTRA_DEFS})
)", dir.string());
    const Project* app = bugfix3_find(sol, "App");
    REQUIRE(app != nullptr);
    CHECK(bugfix3_has(app->project_level_preprocessor_definitions, "ONE"));
    CHECK(bugfix3_has(app->project_level_preprocessor_definitions, "TWO"));
    CHECK(bugfix3_find(sol, "fromfile") != nullptr);
    fs::remove_all(dir, ec);
}

TEST_CASE("CMake reader implements the list() subcommands", "[bugfix3_make]") {
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
set(LST a;b;c)
list(LENGTH LST N)
list(REMOVE_ITEM LST b)
list(INSERT LST 0 z)
list(SORT LST)
list(TRANSFORM LST PREPEND "D_")
list(GET LST 0 -1 PAIR)
list(FIND LST D_c IDX)
set(FILES src/a.cpp;src/main.cpp;src/b.cpp)
list(FILTER FILES EXCLUDE REGEX "main")
list(JOIN LST "+" JOINED)
list(REMOVE_DUPLICATES DUPS)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE ${LST} N_${N} PAIR_${PAIR} IDX_${IDX} J_${JOINED})
target_sources(App PRIVATE ${FILES})
)");
    const Project* p = bugfix3_find(sol, "App");
    REQUIRE(p != nullptr);
    const auto& defs = p->project_level_preprocessor_definitions;
    CHECK(bugfix3_has(defs, "D_a"));
    CHECK(bugfix3_has(defs, "D_c"));
    CHECK(bugfix3_has(defs, "D_z"));
    CHECK_FALSE(bugfix3_has(defs, "D_b"));
    CHECK(bugfix3_has(defs, "N_3"));
    CHECK(bugfix3_has(defs, "PAIR_D_a"));
    CHECK(bugfix3_has(defs, "IDX_1"));
    CHECK(bugfix3_has(defs, "J_D_a+D_c+D_z"));
    bool main_twice = false;
    int mains = 0;
    for (const auto& src : p->sources) {
        if (src.path.find("main.cpp") != std::string::npos) ++mains;
        CHECK(src.path.find("src/main.cpp") == std::string::npos);
    }
    main_twice = mains > 1;
    CHECK_FALSE(main_twice);
}

TEST_CASE("CMake reader propagates set(... PARENT_SCOPE) out of a function", "[bugfix3_make]") {
    CMakeParser parser;
    Solution sol = parser.parse_string(R"(
project(T)
function(setres value)
  set(RESULT ${value} PARENT_SCOPE)
endfunction()
setres(fromfunc)
add_executable(App main.cpp)
target_compile_definitions(App PRIVATE R_${RESULT})
)");
    const Project* p = bugfix3_find(sol, "App");
    REQUIRE(p != nullptr);
    CHECK(bugfix3_has(p->project_level_preprocessor_definitions, "R_fromfunc"));
}
