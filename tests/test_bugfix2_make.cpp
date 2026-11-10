#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

#include <cstdlib>

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string bugfix2_make_read(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Generates makefiles for a buildscript into a fresh temp directory holding the given files.
struct Bugfix2MakeProject {
    fs::path dir;
    Solution solution;
    bool generated = false;

    Bugfix2MakeProject(const std::string& name, const std::string& buildscript,
                       const std::map<std::string, std::string>& files) {
        dir = fs::temp_directory_path() / ("sighmake_bugfix2_make_" + name);
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
    ~Bugfix2MakeProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string makefile(const std::string& name) const { return bugfix2_make_read(dir / "build" / name); }
    int make(const std::string& goal) const {
        const std::string cmd = "make -C \"" + (dir / "build").string() + "\" " + goal + " >/dev/null 2>&1";
        return std::system(cmd.c_str());
    }
};

// The recipe lines that follow the "<object>: ..." rule whose object name contains `needle`.
std::string bugfix2_rule_for(const std::string& makefile, const std::string& needle) {
    std::istringstream in(makefile);
    std::string line, result;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '\t' && line.find(needle) != std::string::npos &&
            line.find(".o:") != std::string::npos) {
            inside = true;
        } else if (inside && !line.empty() && line[0] != '\t') {
            break;
        }
        if (inside) result += line + "\n";
    }
    return result;
}

} // namespace

TEST_CASE("Makefile resolves relative per-file object_file against the buildscript", "[bugfix2_make]") {
    Bugfix2MakeProject p("object_file_rel", R"(
[solution]
name = O
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj
sources = main.cpp
main.cpp:object_file = objs/x.o
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("../objs/x.o") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
    CHECK(fs::exists(p.dir / "objs" / "x.o"));
#endif
}

TEST_CASE("Makefile does not use the C++ precompiled header for Objective-C sources", "[bugfix2_make]") {
    Bugfix2MakeProject p("pch_objc", R"(
[solution]
name = P
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj
sources = main.cpp, view.m, other.mm, pch.cpp
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
)", {{"pch.h", "#pragma once\n"},
     {"pch.cpp", "#include \"pch.h\"\n"},
     {"main.cpp", "int main() { return 0; }\n"},
     {"view.m", "int v;\n"},
     {"other.mm", "int o;\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    const std::string cpp_rule = bugfix2_rule_for(mk, "main");
    const std::string m_rule = bugfix2_rule_for(mk, "view");
    const std::string mm_rule = bugfix2_rule_for(mk, "other");
    REQUIRE(!cpp_rule.empty());
    REQUIRE(!m_rule.empty());
    REQUIRE(!mm_rule.empty());
    CHECK(cpp_rule.find("-include") != std::string::npos);
    CHECK(m_rule.find("-include") == std::string::npos);
    CHECK(m_rule.find("$(PCH_OUTPUT)") == std::string::npos);
    CHECK(mm_rule.find("-include") == std::string::npos);
    CHECK(mm_rule.find("$(PCH_OUTPUT)") == std::string::npos);
}

TEST_CASE("Master Makefile quotes install paths, libs and .def files containing spaces", "[bugfix2_make]") {
    Bugfix2MakeProject p("spaces", R"(
[solution]
name = S
configurations = Release
platforms = Linux

[project:Lib]
type = dll
outdir = out dir/lib
intdir = obj
sources = lib.cpp
libs = my libs/libextra.a
module_def = my defs/exports.map

[project:App]
type = exe
outdir = out dir/bin
intdir = obj/app
sources = main.cpp
)", {{"lib.cpp", "extern \"C\" int foo() { return 1; }\nextern \"C\" int bar() { return 2; }\n"},
     {"main.cpp", "int main() { return 0; }\n"},
     {"my libs/extra.c", "int extra_fn() { return 3; }\n"},
     {"my defs/exports.map", "{ global: foo; local: *; };\n"}});
    REQUIRE(p.generated);
    const std::string master = bugfix2_make_read(p.dir / "Makefile");
    const std::string lib_mk = p.makefile("Lib.Release");
    CHECK(master.find("install -m 755 \"../out dir/bin/App\" \"$(DESTDIR)$(PREFIX)/bin/App\"") != std::string::npos);
    CHECK(master.find("rm -f \"$(DESTDIR)$(PREFIX)/bin/App\"") != std::string::npos);
    CHECK(lib_mk.find("\"../my libs/libextra.a\"") != std::string::npos);
    CHECK(lib_mk.find("--version-script=\"../my defs/exports.map\"") != std::string::npos);
#ifndef _WIN32
    REQUIRE(std::system(("cc -c \"" + (p.dir / "my libs/extra.c").string() + "\" -o \"" +
                         (p.dir / "my libs/extra.o").string() + "\" && ar rcs \"" +
                         (p.dir / "my libs/libextra.a").string() + "\" \"" +
                         (p.dir / "my libs/extra.o").string() + "\"").c_str()) == 0);
    const fs::path dest = p.dir / "dest dir";
    CHECK(p.make("install DESTDIR=\"" + dest.string() + "\"") == 0);
    CHECK(fs::exists(dest / "usr/local/bin/App"));
    CHECK(fs::exists(dest / "usr/local/lib/libLib.so"));
    CHECK(p.make("uninstall DESTDIR=\"" + dest.string() + "\"") == 0);
    CHECK(!fs::exists(dest / "usr/local/bin/App"));
#endif
}

TEST_CASE("Linux makefiles ignore dependencies declared only for Android", "[bugfix2_make]") {
    Bugfix2MakeProject p("android_dep", R"(
[solution]
name = A
configurations = Debug
platforms = Linux, Android

[project:Engine]
type = lib
outdir = bin
intdir = obj/engine
sources = engine.cpp

[project:Game]
type = exe
outdir = bin
intdir = obj/game
sources = main.cpp
if(Android) {
    target_link_libraries(
        Engine PUBLIC
    )
}
)", {{"engine.cpp", "int engine() { return 1; }\n"},
     {"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string linux_mk = p.makefile("Game.Debug");
    CHECK(linux_mk.find("Engine") == std::string::npos);
    CHECK(linux_mk.find("PROJECT_DEPS") == std::string::npos);
    const std::string master = bugfix2_make_read(p.dir / "Makefile");
    std::istringstream in(master);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("Game.Debug:", 0) == 0) CHECK(line.find("Engine") == std::string::npos);
        if (line.rfind("Game.Debug.Android:", 0) == 0) CHECK(line.find("Engine.Debug.Android") != std::string::npos);
    }
}
