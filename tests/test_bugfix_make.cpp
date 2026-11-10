#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string bugfix_make_read(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Generates makefiles for a buildscript into a fresh temp directory holding the given files.
struct BugfixMakeProject {
    fs::path dir;
    Solution solution;
    bool generated = false;

    BugfixMakeProject(const std::string& name, const std::string& buildscript,
                      const std::map<std::string, std::string>& files) {
        dir = fs::temp_directory_path() / ("sighmake_bugfix_make_" + name);
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
    ~BugfixMakeProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string makefile(const std::string& name) const { return bugfix_make_read(dir / "build" / name); }
    // Runs make in the build directory; returns the exit status (0 = success).
    int make(const std::string& goal) const {
        const std::string cmd = "make -C \"" + (dir / "build").string() + "\" " + goal + " >/dev/null 2>&1";
        return std::system(cmd.c_str());
    }
};

} // namespace

TEST_CASE("Makefile honors per-file defines", "[bugfix_make]") {
    BugfixMakeProject p("per_file_defines", R"(
[solution]
name = PF
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp, other.cpp
main.cpp:defines = FILE_DEF=2
)", {{"main.cpp", "#if FILE_DEF != 2\n#error FILE_DEF missing\n#endif\nint other();\nint main() { return other(); }\n"},
     {"other.cpp", "#ifdef FILE_DEF\n#error leaked\n#endif\nint other() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("-DFILE_DEF=2") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
#endif
}

TEST_CASE("Makefile honors per-file compile_as", "[bugfix_make]") {
    BugfixMakeProject p("compile_as", R"(
[solution]
name = PF
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.c, legacy.c
legacy.c:compile_as = CompileAsCpp
)", {{"main.c", "int legacy(void);\nint main(void) { return legacy(); }\n"},
     {"legacy.c", "extern \"C\" int legacy() { bool b = true; return b ? 0 : 1; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("$(CXX) $(SIGHMAKE_CXXFLAGS) -x c++") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
#endif
}

TEST_CASE("Makefile C sources in a C++ PCH project do not force-include the C++ PCH", "[bugfix_make]") {
    BugfixMakeProject p("pch_c", R"(
[solution]
name = PCH
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp, c.c, pch.cpp
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
)", {{"pch.h", "#pragma once\n#ifdef __cplusplus\n#include <vector>\n#endif\n"},
     {"pch.cpp", "#include \"pch.h\"\n"},
     {"main.cpp", "extern \"C\" int cfn(void);\nint main() { std::vector<int> v; return cfn() + (int)v.size(); }\n"},
     {"c.c", "int cfn(void) { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("$(CC) $(SIGHMAKE_CFLAGS) -include") == std::string::npos);
    CHECK(mk.find("$(CXX) $(SIGHMAKE_CXXFLAGS) -include") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
#endif
}

TEST_CASE("Makefile resolves pch_header relative to the buildscript and include directories", "[bugfix_make]") {
    // The test runs with a working directory other than the buildscript's directory, like
    // `sighmake sub/app.buildscript` does.
    BugfixMakeProject p("pch_resolve", R"(
[solution]
name = PCH
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp, pch.cpp
includes = include
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
)", {{"include/pch.h", "#pragma once\n#include <vector>\n"},
     {"pch.cpp", "#include \"pch.h\"\n"},
     {"main.cpp", "int main() { std::vector<int> v; return (int)v.size(); }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("PCH_HEADER = ../include/pch.h\n") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
#endif
}

TEST_CASE("Makefile writes every line of a multi-line build event as a recipe line", "[bugfix_make]") {
    BugfixMakeProject p("multiline_event", R"(
[solution]
name = Ev
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp
prebuild = """
touch pre1.txt
touch pre2.txt"""
postbuild = """
touch post1.txt
touch post2.txt
"""
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("\ttouch pre1.txt\n\ttouch pre2.txt\n") != std::string::npos);
    CHECK(mk.find("\ttouch post1.txt\n\ttouch post2.txt\n") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
    CHECK(fs::exists(p.dir / "build" / "pre2.txt"));
    CHECK(fs::exists(p.dir / "build" / "post2.txt"));
#endif
}

TEST_CASE("Makefile does not run build events with use_in_build = false", "[bugfix_make]") {
    BugfixMakeProject p("event_use_in_build", R"(
[solution]
name = Ev
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp
prebuild = echo PREBUILD_RUNS
prebuild_use_in_build = false
prelink = echo PRELINK_RUNS
prelink_use_in_build = false
postbuild = echo POSTBUILD_RUNS
postbuild_use_in_build = false
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("PREBUILD_RUNS") == std::string::npos);
    CHECK(mk.find("PRELINK_RUNS") == std::string::npos);
    CHECK(mk.find("POSTBUILD_RUNS") == std::string::npos);
    CHECK(mk.find("prebuild") == std::string::npos);
}

TEST_CASE("Master Makefile install expands $(Configuration) in outdir like the project makefile", "[bugfix_make]") {
    BugfixMakeProject p("install_vars", R"(
[solution]
name = Inst
configurations = Debug, Release
platforms = Linux

[project:App]
type = exe
outdir = bin/$(Configuration)
intdir = obj/$(Configuration)
sources = main.cpp
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string master = p.makefile("Makefile");
    CHECK(master.find("$(INSTALL) -m 755 ../bin/Release/App ") != std::string::npos);
    CHECK(master.find("$(Configuration)") == std::string::npos);
#ifndef _WIN32
    CHECK(p.make("install DESTDIR=\"" + (p.dir / "dest").string() + "\"") == 0);
    CHECK(fs::exists(p.dir / "dest" / "usr" / "local" / "bin" / "App"));
#endif
}

TEST_CASE("Makefile builds sources whose path contains spaces", "[bugfix_make]") {
    BugfixMakeProject p("source_spaces", R"(
[solution]
name = Sp
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = my src/main.cpp
)", {{"my src/main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find(": ../my\\ src/main.cpp") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
    CHECK(fs::exists(p.dir / "bin" / "App"));
#endif
}

TEST_CASE("Makefile rebuilds the PCH when a header it includes changes", "[bugfix_make]") {
    BugfixMakeProject p("pch_deps", R"(
[solution]
name = PCH
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp, pch.cpp
pch = Use
pch_header = pch.h
pch.cpp:pch = Create
)", {{"pch.h", "#pragma once\n#include \"common.h\"\n"},
     {"common.h", "#define VALUE 1\n"},
     {"pch.cpp", "#include \"pch.h\"\n"},
     {"main.cpp", "int main() { return VALUE; }\n"}});
    REQUIRE(p.generated);
    CHECK(p.makefile("App.Debug").find("-include $(PCH_OUTPUT).d") != std::string::npos);
#ifndef _WIN32
    REQUIRE(p.make("App.Debug") == 0);
    const std::string app = "\"" + (p.dir / "bin" / "App").string() + "\"";
    const int first_status = std::system(app.c_str());
    CHECK(WEXITSTATUS(first_status) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::ofstream(p.dir / "common.h") << "#define VALUE 7\n";
    REQUIRE(p.make("App.Debug") == 0);
    const int second_status = std::system(app.c_str());
    CHECK(WEXITSTATUS(second_status) == 7);
#endif
}

TEST_CASE("Makefile translates bare shared-library file names to -l flags", "[bugfix_make]") {
    BugfixMakeProject p("bare_so", R"(
[solution]
name = L
configurations = Debug
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp
libs = libm.so, dl.so, libfoo.dylib
)", {{"main.cpp", "#include <cmath>\nint main(int argc, char**) { return (int)std::sqrt((double)argc) - 1; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Debug");
    CHECK(mk.find("-lm ") != std::string::npos);
    CHECK(mk.find("-ldl ") != std::string::npos);
    CHECK(mk.find("-lfoo ") != std::string::npos);
    CHECK(mk.find(".so ") == std::string::npos);
    CHECK(mk.find(".dylib") == std::string::npos);
}

TEST_CASE("Makefile post-build event runs on the stripped binary", "[bugfix_make]") {
    BugfixMakeProject p("postbuild_after_strip", R"(
[solution]
name = Ev
configurations = Release
platforms = Linux

[project:App]
type = exe
outdir = bin
intdir = obj/App
sources = main.cpp
postbuild = cp $@ App.copy
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string mk = p.makefile("App.Release");
    const size_t strip_pos = mk.find("\t$(STRIP) $@");
    const size_t post_pos = mk.find("\tcp $@ App.copy");
    REQUIRE(strip_pos != std::string::npos);
    REQUIRE(post_pos != std::string::npos);
    CHECK(strip_pos < post_pos);
#ifndef _WIN32
    REQUIRE(p.make("App.Release") == 0);
    CHECK(fs::file_size(p.dir / "build" / "App.copy") == fs::file_size(p.dir / "bin" / "App"));
#endif
}

TEST_CASE("Makefile expands $(IntDir) in per-file object_file", "[bugfix_make]") {
    BugfixMakeProject p("object_file_intdir", R"(
[solution]
name = O
configurations = Debug, Release
platforms = Linux

[project:App]
type = exe
outdir = bin/$(Configuration)
intdir = obj/$(Configuration)
sources = main.cpp
main.cpp:object_file = $(IntDir)custom_main.obj
)", {{"main.cpp", "int main() { return 0; }\n"}});
    REQUIRE(p.generated);
    const std::string debug = p.makefile("App.Debug");
    const std::string release = p.makefile("App.Release");
    CHECK(debug.find("$(IntDir)") == std::string::npos);
    CHECK(debug.find("../obj/Debug/custom_main.o") != std::string::npos);
    CHECK(release.find("../obj/Release/custom_main.o") != std::string::npos);
#ifndef _WIN32
    CHECK(p.make("App.Debug") == 0);
    CHECK(fs::exists(p.dir / "obj" / "Debug" / "custom_main.o"));
#endif
}
