#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

#include <cstdlib>

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
    CHECK(mk.find("$(CXX) $(CXXFLAGS) -x c++") != std::string::npos);
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
    CHECK(mk.find("$(CC) $(CFLAGS) -include") == std::string::npos);
    CHECK(mk.find("$(CXX) $(CXXFLAGS) -include") != std::string::npos);
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
