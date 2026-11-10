#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

// Generated Makefiles are written in text mode, so Windows produces CRLF; compare on LF.
std::string bugfix4_read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

// Generates makefiles for a buildscript into a fresh temp directory holding the given files.
struct Bugfix4Project {
    fs::path dir;
    Solution solution;
    bool generated = false;

    Bugfix4Project(const std::string& name, const std::string& buildscript,
                   const std::map<std::string, std::string>& files) {
        dir = fs::temp_directory_path() / ("sighmake bugfix4 makeconv " + name);
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
    ~Bugfix4Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string makefile(const std::string& name) const { return bugfix4_read(dir / "build" / name); }
    fs::path build_dir() const { return dir / "build"; }

#ifndef _WIN32
    // Runs `<env> make -C build <args>`; output goes to <dir>/make.log.
    int make(const std::string& args, const std::string& env = "") const {
#ifdef __APPLE__
        // macOS ships GNU make 3.81, which compares modification times to the second: let the
        // previous step's files age so a rewritten flag stamp is newer than the objects.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
#endif
        const std::string cmd = "cd \"" + build_dir().string() + "\" && " + env + (env.empty() ? "" : " ") +
                                "make " + args + " > \"" + (dir / "make.log").string() + "\" 2>&1";
        const int status = std::system(cmd.c_str());
        return status;
    }
    std::string log() const { return bugfix4_read(dir / "make.log"); }
    int run(const std::string& command) const {
        const std::string cmd = command + " >/dev/null 2>&1";
        const int status = std::system(cmd.c_str());
        return status;
    }
#endif
};

#ifndef _WIN32
bool bugfix4_tool(const char* tool) {
    const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    const int status = std::system(cmd.c_str());
    return status == 0;
}

#ifdef __APPLE__
const char* const kHostCxx = "clang++";
#else
const char* const kHostCxx = "g++";
#endif

bool bugfix4_toolchain() {
    return bugfix4_tool("make") && bugfix4_tool(kHostCxx) && bugfix4_tool("ar") && bugfix4_tool("strip");
}

// make's exit status of a command run through std::system: 0 only when it exited normally with 0.
bool bugfix4_ok(int status) { return status == 0; }
#endif

const char* const kConvScript = R"(
[solution]
name = Conv
configurations = Debug, Release
platforms = Linux

[project:Lib]
type = static
sources = lib.cpp
outdir = out
intdir = obj/lib

[project:Sh]
type = shared
sources = sh.cpp
outdir = out
intdir = obj/sh

[project:App]
type = exe
sources = main.cpp, c.c
outdir = out
intdir = obj/app
project_references = Lib, Sh
)";

const std::map<std::string, std::string> kConvFiles = {
    {"lib.cpp", "int lib() { return 1; }\n"},
    {"sh.cpp", "int sh() { return 2; }\n"},
    {"c.c", "#ifdef USER_C\nint cfn(void) { return 0; }\n#else\nint cfn(void) { return 1; }\n#endif\n"},
    {"main.cpp",
     "int lib();\nint sh();\nextern \"C\" int cfn(void);\n"
     "#ifdef USER_FLAG\nint main() { return lib() + sh() == 3 && cfn() == 0 ? 0 : 1; }\n"
     "#else\nint main() { return 1; }\n#endif\n"}};

} // namespace

TEST_CASE("Makefile toolchain variables yield to the environment and the command line", "[bugfix4_makeconv]") {
    Bugfix4Project p("toolchain", kConvScript, kConvFiles);
    REQUIRE(p.generated);
    const std::string app = p.makefile("App.Release");
    CHECK(app.find("ifeq ($(origin CXX),default)\n") != std::string::npos);
    CHECK(app.find("ifeq ($(origin CC),default)\n") != std::string::npos);
    const std::string lib = p.makefile("Lib.Release");
    CHECK(lib.find("ifeq ($(origin AR),default)\nAR = ar\nendif\n") != std::string::npos);
    CHECK(lib.find("$(AR) rcs") != std::string::npos);
    CHECK(lib.find("\tar rcs") == std::string::npos);
#ifndef _WIN32
    if (!bugfix4_toolchain()) {
        WARN("make/compiler/ar/strip not available: skipping the real build");
        return;
    }
    // An environment CXX is honoured (a failing compiler fails the build) ...
    CHECK_FALSE(bugfix4_ok(p.make("Lib.Release", "AR=false")));
    CHECK_FALSE(bugfix4_ok(p.make("Release", "CXX=false")));
    CHECK_FALSE(bugfix4_ok(p.make("Release CXX=false")));
    // ... and so are the explicit host values; the baked default works when nothing is passed.
    CHECK(bugfix4_ok(p.make("clean")));
    CHECK(bugfix4_ok(p.make("Release", std::string("CXX=") + kHostCxx)));
    CHECK(bugfix4_ok(p.make("clean")));
    CHECK(bugfix4_ok(p.make("Release")));
#endif
}

TEST_CASE("Makefile keeps the project flags apart from the user's CXXFLAGS/CPPFLAGS/CFLAGS/LDFLAGS", "[bugfix4_makeconv]") {
    Bugfix4Project p("userflags", kConvScript, kConvFiles);
    REQUIRE(p.generated);
    const std::string app = p.makefile("App.Release");
    CHECK(app.find("\nSIGHMAKE_CXXFLAGS = ") != std::string::npos);
    CHECK(app.find("\nSIGHMAKE_CFLAGS = ") != std::string::npos);
    CHECK(app.find("\nCXXFLAGS = ") == std::string::npos);
    CHECK(app.find("\nCFLAGS = ") == std::string::npos);
    CHECK(app.find("\nLDFLAGS = ") == std::string::npos);
    CHECK(app.find("\nLDLIBS = ") == std::string::npos);
    CHECK(app.find("$(CXX) $(SIGHMAKE_CXXFLAGS) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ \"$<\"") != std::string::npos);
    CHECK(app.find("$(CC) $(SIGHMAKE_CFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ \"$<\"") != std::string::npos);
    CHECK(app.find("$(SIGHMAKE_LDFLAGS) $(LDFLAGS) -o $@ $(OBJS)") != std::string::npos);
#ifndef _WIN32
    if (!bugfix4_toolchain()) {
        WARN("make/compiler/ar/strip not available: skipping the real build");
        return;
    }
    const fs::path exe = p.dir / "out" / "App";
    REQUIRE(bugfix4_ok(p.make("Release")));
    CHECK_FALSE(p.run("cd \"" + p.build_dir().string() + "\" && \"" + exe.string() + "\"") == 0);
    // A user define on the command line rebuilds the objects that use it, and the program sees it.
    REQUIRE(bugfix4_ok(p.make("Release", "CXXFLAGS=-DUSER_FLAG")));
    CHECK(p.log().find("-DUSER_FLAG") != std::string::npos);
    // (the shared library is found through its build-relative path: run from the build directory)
    CHECK(p.run("cd \"" + p.build_dir().string() + "\" && \"" + exe.string() + "\"") != 0);  // C part still not 0
    REQUIRE(bugfix4_ok(p.make("Release", "CXXFLAGS=-DUSER_FLAG CFLAGS=-DUSER_C")));
    INFO("make log of the last step:\n" << p.log());
    CHECK(p.run("cd \"" + p.build_dir().string() + "\" && \"" + exe.string() + "\"") == 0);
    // The same flags again: nothing to rebuild.
    REQUIRE(bugfix4_ok(p.make("-n Release", "CXXFLAGS=-DUSER_FLAG CFLAGS=-DUSER_C")));
    CHECK(p.log().find("-MMD") == std::string::npos);
    // Dropping them rebuilds back to the original behaviour.
    REQUIRE(bugfix4_ok(p.make("Release")));
    CHECK_FALSE(p.run("cd \"" + p.build_dir().string() + "\" && \"" + exe.string() + "\"") == 0);
    // CPPFLAGS from the environment reach the compiler too.
    REQUIRE(bugfix4_ok(p.make("-n Release", "CPPFLAGS=-DFROM_CPPFLAGS")));
    CHECK(p.log().find("-DFROM_CPPFLAGS") != std::string::npos);
    // User flags come after the project's, so they can override -O/-g.
    const std::string log = p.log();
    const size_t project_flag = log.find("-std=");
    CHECK(project_flag != std::string::npos);
    CHECK(log.find("-DFROM_CPPFLAGS", project_flag) != std::string::npos);
#endif
}

TEST_CASE("Makefile relinks when the user's LDFLAGS or LDLIBS change", "[bugfix4_makeconv]") {
    Bugfix4Project p("userld", kConvScript, kConvFiles);
    REQUIRE(p.generated);
    const std::string app = p.makefile("App.Release");
    CHECK(app.find("$(LDLIBS)") != std::string::npos);
#ifndef _WIN32
    if (!bugfix4_toolchain()) {
        WARN("make/compiler/ar/strip not available: skipping the real build");
        return;
    }
    REQUIRE(bugfix4_ok(p.make("Release")));
    REQUIRE(bugfix4_ok(p.make("-n App.Release")));
    CHECK(p.log().find("-o ") == std::string::npos);  // up to date: no compile or link line
    REQUIRE(bugfix4_ok(p.make("-n App.Release", "LDFLAGS=-Wl,-userldflag")));
    CHECK(p.log().find("-Wl,-userldflag") != std::string::npos);
    REQUIRE(bugfix4_ok(p.make("-n App.Release LDLIBS=-luserlib")));
    CHECK(p.log().find("-luserlib") != std::string::npos);
    // A new compiler from the environment rebuilds the objects.
    REQUIRE(bugfix4_ok(p.make("-n App.Release", "CXX=\"" + std::string(kHostCxx) + " -fno-strict-aliasing\"")));
    CHECK(p.log().find("-fno-strict-aliasing") != std::string::npos);
#endif
}

TEST_CASE("Makefile Release link line is unchanged apart from the variable convention", "[bugfix4_makeconv]") {
    Bugfix4Project p("static", kConvScript, kConvFiles);
    REQUIRE(p.generated);
    const std::string sh = p.makefile("Sh.Release");
    CHECK(sh.find("$(SIGHMAKE_LDFLAGS) $(LDFLAGS) -o $@ $(OBJS)") != std::string::npos);
    CHECK(sh.find("$(LDLIBS)\n") != std::string::npos);
}

TEST_CASE("Makefile install honours DESTDIR, PREFIX, BINDIR, LIBDIR and INSTALL", "[bugfix4_makeconv]") {
    Bugfix4Project p("install", kConvScript, kConvFiles);
    REQUIRE(p.generated);
    const std::string master = p.makefile("Makefile");
    CHECK(master.find("PREFIX ?= $(or $(prefix),/usr/local)\n") != std::string::npos);
    CHECK(master.find("BINDIR ?= $(or $(bindir),$(PREFIX)/bin)\n") != std::string::npos);
    CHECK(master.find("LIBDIR ?= $(or $(libdir),$(PREFIX)/lib)\n") != std::string::npos);
    CHECK(master.find("INSTALL ?= install\n") != std::string::npos);
    CHECK(master.find("\t$(INSTALL) -d \"$(DESTDIR)$(BINDIR)\"\n") != std::string::npos);
    CHECK(master.find("\t$(INSTALL) -m 755 ../out/App \"$(DESTDIR)$(BINDIR)/App\"\n") != std::string::npos);
    CHECK(master.find("\t$(INSTALL) -d \"$(DESTDIR)$(LIBDIR)\"\n") != std::string::npos);
    CHECK(master.find("\trm -f \"$(DESTDIR)$(BINDIR)/App\"\n") != std::string::npos);
    CHECK(master.find(".PHONY: all clean install install-strip uninstall") != std::string::npos);
#ifndef _WIN32
    if (!bugfix4_toolchain() || !bugfix4_tool("install")) {
        WARN("make/compiler/ar/strip/install not available: skipping the real build");
        return;
    }
    const fs::path stage = p.dir / "stage";
    const std::string so = std::string("Sh") +
#ifdef __APPLE__
        ".dylib";
#else
        ".so";
#endif
    REQUIRE(bugfix4_ok(p.make("install DESTDIR=\"" + stage.string() + "\"")));
    CHECK(fs::exists(stage / "usr/local/bin/App"));
    CHECK(fs::exists(stage / "usr/local/lib" / so));
    CHECK(bugfix4_ok(p.make("uninstall DESTDIR=\"" + stage.string() + "\"")));
    CHECK_FALSE(fs::exists(stage / "usr/local/bin/App"));

    const fs::path opt = p.dir / "opt";
    REQUIRE(bugfix4_ok(p.make("install prefix=\"" + opt.string() + "\"")));
    CHECK(fs::exists(opt / "bin" / "App"));
    CHECK(fs::exists(opt / "lib" / so));
    const fs::path split = p.dir / "split";
    REQUIRE(bugfix4_ok(p.make("install PREFIX=\"" + split.string() + "\" BINDIR=\"" + (split / "programs").string() +
                              "\" LIBDIR=\"" + (split / "libs").string() + "\"")));
    CHECK(fs::exists(split / "programs" / "App"));
    CHECK(fs::exists(split / "libs" / so));
    CHECK_FALSE(fs::exists(split / "bin"));
    // INSTALL is overridable like any GNU install variable
    CHECK_FALSE(bugfix4_ok(p.make("install INSTALL=false DESTDIR=\"" + (p.dir / "never").string() + "\"")));
#endif
}

TEST_CASE("Makefile install-strip installs stripped copies of a configuration that is not stripped at link time", "[bugfix4_makeconv]") {
    Bugfix4Project p("installstrip", std::string(kConvScript) + "\n[config:Release|Linux]\nstrip = false\n", kConvFiles);
    REQUIRE(p.generated);
    const std::string master = p.makefile("Makefile");
    CHECK(master.find("install-strip: install\n\t$(STRIP) \"$(DESTDIR)$(BINDIR)/App\"\n") != std::string::npos);
#ifdef __APPLE__
    CHECK(master.find("\t$(STRIP) -x \"$(DESTDIR)$(LIBDIR)/Sh.dylib\"\n") != std::string::npos);
#else
    CHECK(master.find("\t$(STRIP) --strip-unneeded \"$(DESTDIR)$(LIBDIR)/Sh.so\"\n") != std::string::npos);
#endif
#ifndef _WIN32
    if (!bugfix4_toolchain() || !bugfix4_tool("install")) {
        WARN("make/compiler/ar/strip/install not available: skipping the real build");
        return;
    }
    const fs::path plain = p.dir / "plain";
    const fs::path stripped = p.dir / "stripped";
    REQUIRE(bugfix4_ok(p.make("install DESTDIR=\"" + plain.string() + "\"")));
    REQUIRE(bugfix4_ok(p.make("install-strip DESTDIR=\"" + stripped.string() + "\"")));
    const auto size_of = [](const fs::path& f) { return fs::file_size(f); };
    REQUIRE(fs::exists(plain / "usr/local/bin/App"));
    REQUIRE(fs::exists(stripped / "usr/local/bin/App"));
    // The unstripped installation keeps its -g symbols; install-strip drops them (and leaves the build output alone).
    CHECK(size_of(stripped / "usr/local/bin/App") < size_of(plain / "usr/local/bin/App"));
    CHECK(size_of(stripped / "usr/local/bin/App") < size_of(p.dir / "out" / "App"));
    CHECK(size_of(p.dir / "out" / "App") == size_of(plain / "usr/local/bin/App"));
#endif
}

namespace {

std::string symbols_script(const std::string& extra, const std::string& type = "exe") {
    return "\n[solution]\nname = Sym\nconfigurations = Debug, Release\nplatforms = Linux\n\n"
           "[project:App]\ntype = " + type + "\nsources = main.cpp\noutdir = out\nintdir = obj\n" + extra;
}

const std::map<std::string, std::string> kSymFiles = {
    {"main.cpp", "#include <cstdio>\nint helper(int v) { return v * 2; }\nint main() { std::printf(\"%d\\n\", helper(21)); return 0; }\n"}};

bool has_flag_token(const std::string& line, const std::string& flag) {
    return (" " + line + " ").find(" " + flag + " ") != std::string::npos;
}

std::string var_line(const std::string& mk, const std::string& name) {
    const size_t start = mk.find("\n" + name + " = ");
    if (start == std::string::npos) return {};
    return mk.substr(start + 1, mk.find('\n', start + 1) - start - 1);
}

} // namespace

TEST_CASE("debug_symbols picks embedded, separate or no symbols; the strip key stays the fallback", "[bugfix4_makeconv]") {
    // defaults: Debug embedded, Release none + strip (unchanged)
    {
        Bugfix4Project p("sym defaults", symbols_script(""), kSymFiles);
        REQUIRE(p.generated);
        const std::string dbg = p.makefile("App.Debug"), rel = p.makefile("App.Release");
        CHECK(has_flag_token(var_line(dbg, "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(dbg.find("$(STRIP)") == std::string::npos);
        CHECK(dbg.find("STRIP ?=") == std::string::npos);
        CHECK_FALSE(has_flag_token(var_line(rel, "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(rel.find("\t$(STRIP) $@\n") != std::string::npos);
        CHECK(rel.find("$(OBJCOPY)") == std::string::npos);
    }
    // an explicit debug_symbols wins over strip
    {
        Bugfix4Project p("sym explicit", symbols_script("debug_symbols = embedded\nstrip = true\n"), kSymFiles);
        REQUIRE(p.generated);
        const std::string rel = p.makefile("App.Release");
        CHECK(has_flag_token(var_line(rel, "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(rel.find("$(STRIP)") == std::string::npos);
        CHECK(rel.find("\t$(OBJCOPY)") == std::string::npos);
    }
    {
        Bugfix4Project p("sym none", symbols_script("debug_symbols = none\nstrip = false\n"), kSymFiles);
        REQUIRE(p.generated);
        const std::string dbg = p.makefile("App.Debug");
        CHECK_FALSE(has_flag_token(var_line(dbg, "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(dbg.find("\t$(STRIP) $@\n") != std::string::npos);
    }
    // only strip given: exactly the former behaviour
    {
        Bugfix4Project p("sym strip only", symbols_script("strip = false\n"), kSymFiles);
        REQUIRE(p.generated);
        const std::string rel = p.makefile("App.Release");
        CHECK(has_flag_token(var_line(rel, "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(rel.find("$(STRIP)") == std::string::npos);
    }
    // aliases, a config section and an unknown value (ignored: strip decides)
    {
        Bugfix4Project p("sym alias", symbols_script("debug_symbols = split\n\n[config:Debug|Linux]\ndebug_symbols = off\n"), kSymFiles);
        REQUIRE(p.generated);
        const std::string split = p.makefile("App.Release");
        const bool splits = split.find("--add-gnu-debuglink=") != std::string::npos ||
                            split.find("$(DSYMUTIL)") != std::string::npos;
        CHECK(splits);
        CHECK_FALSE(has_flag_token(var_line(p.makefile("App.Debug"), "SIGHMAKE_CXXFLAGS"), "-g"));
        Bugfix4Project q("sym unknown", symbols_script("debug_symbols = banana\n"), kSymFiles);
        REQUIRE(q.generated);
        CHECK_FALSE(has_flag_token(var_line(q.makefile("App.Release"), "SIGHMAKE_CXXFLAGS"), "-g"));
        CHECK(q.makefile("App.Release").find("\t$(STRIP) $@\n") != std::string::npos);
    }
}

TEST_CASE("debug_symbols = separate splits the symbols into a file next to the binary", "[bugfix4_makeconv]") {
    Bugfix4Project p("sym separate", symbols_script("debug_symbols = separate\n"), kSymFiles);
    REQUIRE(p.generated);
    const std::string rel = p.makefile("App.Release");
    CHECK(has_flag_token(var_line(rel, "SIGHMAKE_CXXFLAGS"), "-g"));
    CHECK(rel.find("\nSTRIP ?= strip\n") != std::string::npos);
#ifdef __APPLE__
    CHECK(rel.find("\nDSYMUTIL ?= dsymutil\n") != std::string::npos);
    CHECK(rel.find("\t$(DSYMUTIL) $@ -o $@.dSYM\n\t$(STRIP) $@\n") != std::string::npos);
#else
    CHECK(rel.find("\nOBJCOPY ?= objcopy\n") != std::string::npos);
    CHECK(rel.find("\t$(OBJCOPY) --only-keep-debug $@ $@.debug\n\t$(STRIP) --strip-unneeded $@\n"
                   "\t$(OBJCOPY) --add-gnu-debuglink=$@.debug $@\n") != std::string::npos);
#endif
    CHECK(rel.find("$(TARGET).debug $(TARGET).dSYM") != std::string::npos);  // clean
    // the link stamp records the mode so switching it relinks
    bool found = false;
    for (const auto& entry : fs::recursive_directory_iterator(p.dir)) {
        if (entry.path().filename().string().rfind("sighmake-link.Release", 0) == 0) {
            found = bugfix4_read(entry.path()).find("[debug_symbols=separate]") != std::string::npos;
        }
    }
    CHECK(found);
#ifndef _WIN32
    if (!bugfix4_toolchain()) {
        WARN("make/compiler/ar/strip not available: skipping the real build");
        return;
    }
    const fs::path app = p.dir / "out" / "App";
#ifdef __linux__
    if (bugfix4_tool("objcopy") && bugfix4_tool("readelf")) {
        REQUIRE(bugfix4_ok(p.make("Release")));
        REQUIRE(fs::exists(app));
        REQUIRE(fs::exists(p.dir / "out" / "App.debug"));
        const std::string sections = p.dir.string() + "/sections.txt";
        const auto readelf = [&](const std::string& args, const fs::path& file) {
            const std::string cmd = "readelf " + args + " \"" + file.string() + "\" > \"" + sections + "\" 2>&1";
            const int status = std::system(cmd.c_str());
            REQUIRE(status == 0);
            return bugfix4_read(sections);
        };
        CHECK(readelf("-S", app).find(".debug_info") == std::string::npos);
        CHECK(readelf("-S", p.dir / "out" / "App.debug").find(".debug_info") != std::string::npos);
        // a debugger finds the file through the .gnu_debuglink section
        CHECK(readelf("--string-dump=.gnu_debuglink", app).find("App.debug") != std::string::npos);
        CHECK(p.run("\"" + app.string() + "\"") == 0);
        CHECK(bugfix4_ok(p.make("clean")));
        CHECK_FALSE(fs::exists(p.dir / "out" / "App.debug"));
    } else {
        WARN("objcopy/readelf not available: skipping the separate debug file check");
    }
#endif
#ifdef __APPLE__
    if (bugfix4_tool("dsymutil")) {
        REQUIRE(bugfix4_ok(p.make("Release")));
        CHECK(fs::exists(p.dir / "out" / "App.dSYM"));
        CHECK(p.run("\"" + app.string() + "\"") == 0);
    } else {
        WARN("dsymutil not available: skipping the dSYM check");
    }
#endif
#endif
}

#if !defined(_WIN32) && defined(__linux__)
TEST_CASE("debug_symbols embedded keeps symbols in the binary; none and the Release default strip it", "[bugfix4_makeconv]") {
    if (!bugfix4_toolchain() || !bugfix4_tool("readelf")) {
        WARN("make/compiler/ar/strip/readelf not available: skipping the real build");
        return;
    }
    const auto sections_of = [](const Bugfix4Project& p) {
        const std::string out = (p.dir / "sections.txt").string();
        const std::string cmd = "readelf -S \"" + (p.dir / "out" / "App").string() + "\" > \"" + out + "\" 2>&1";
        const int status = std::system(cmd.c_str());
        REQUIRE(status == 0);
        return bugfix4_read(out);
    };
    {
        Bugfix4Project p("sym embedded", symbols_script("debug_symbols = embedded\n"), kSymFiles);
        REQUIRE(p.generated);
        REQUIRE(bugfix4_ok(p.make("Release")));
        const std::string sections = sections_of(p);
        CHECK(sections.find(".debug_info") != std::string::npos);
        CHECK(sections.find(".symtab") != std::string::npos);
        CHECK_FALSE(fs::exists(p.dir / "out" / "App.debug"));
    }
    {
        Bugfix4Project p("sym release default", symbols_script(""), kSymFiles);
        REQUIRE(p.generated);
        REQUIRE(bugfix4_ok(p.make("Release")));
        const std::string sections = sections_of(p);
        CHECK(sections.find(".debug_info") == std::string::npos);
        CHECK(sections.find(".symtab") == std::string::npos);
    }
    {
        Bugfix4Project p("sym debug none", symbols_script("debug_symbols = none\n"), kSymFiles);
        REQUIRE(p.generated);
        REQUIRE(bugfix4_ok(p.make("Debug")));
        const std::string sections = sections_of(p);
        CHECK(sections.find(".debug_info") == std::string::npos);
        CHECK(sections.find(".symtab") == std::string::npos);
    }
}
#endif

TEST_CASE("debug_symbols on a static library only affects -g, nothing is split", "[bugfix4_makeconv]") {
    Bugfix4Project p("sym static", symbols_script("debug_symbols = separate\n", "static"), kSymFiles);
    REQUIRE(p.generated);
    const std::string rel = p.makefile("App.Release");
    CHECK(has_flag_token(var_line(rel, "SIGHMAKE_CXXFLAGS"), "-g"));
    CHECK(rel.find("--only-keep-debug") == std::string::npos);
    CHECK(rel.find("dsymutil") == std::string::npos);
    CHECK(rel.find("$(STRIP)") == std::string::npos);
    CHECK(rel.find(".debug") == std::string::npos);
}
