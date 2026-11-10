#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

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
