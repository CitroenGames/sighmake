#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"

#include <cstdio>
#include <cstdlib>

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

std::string mdt_read(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Generates makefiles for a one-executable solution into a fresh temp directory.
struct MdtProject {
    fs::path dir;
    Solution solution;
    bool generated = false;

    MdtProject(const std::string& name, const std::string& solutionExtra) {
        dir = fs::temp_directory_path() / ("sighmake-macos-deployment-target-" + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "src");
        std::ofstream(dir / "src" / "main.cpp", std::ios::binary) << "int main() { return 0; }\n";
#ifdef __APPLE__
        const char* platform = "macOS";
#else
        const char* platform = "Linux";
#endif
        const std::string buildscript = std::string("[solution]\nname = Mdt\nconfigurations = Release\nplatforms = ") + platform +
            "\n" + solutionExtra + "\n[project:App]\ntype = exe\nsources = src/main.cpp\noutdir = out\nintdir = int\n";
        BuildscriptParser parser;
        solution = parser.parse_string(buildscript, dir.string());
        MakefileGenerator generator;
        generated = generator.generate(solution, dir.string());
    }
    ~MdtProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string makefile() const { return mdt_read(dir / "build" / "App.Release"); }
    std::string stamps() const {
        std::string all;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().filename().string().rfind("sighmake-", 0) == 0 && it->path().extension() == ".flags")
                all += mdt_read(it->path()) + "\n";
        return all;
    }
};

} // namespace

TEST_CASE("macos_deployment_target is parsed as a solution key", "[macos_deployment_target]") {
    BuildscriptParser parser;
    const Solution with = parser.parse_string("[solution]\nname = S\nmacos_deployment_target = 14.0\n[project:A]\ntype = exe\n", ".");
    CHECK(with.macos_deployment_target == "14.0");
    BuildscriptParser parser2;
    const Solution without = parser2.parse_string("[solution]\nname = S\n[project:A]\ntype = exe\n", ".");
    CHECK(without.macos_deployment_target.empty());
}

TEST_CASE("Makefiles without macos_deployment_target export nothing", "[macos_deployment_target]") {
    MdtProject p("unset", "");
    REQUIRE(p.generated);
    CHECK(p.makefile().find("MACOSX_DEPLOYMENT_TARGET") == std::string::npos);
    CHECK(p.stamps().find("macos_deployment_target") == std::string::npos);
}

TEST_CASE("macos_deployment_target exports MACOSX_DEPLOYMENT_TARGET on Apple hosts only", "[macos_deployment_target]") {
    MdtProject p("set", "macos_deployment_target = 14.0\n");
    REQUIRE(p.generated);
    const std::string makefile = p.makefile();
#ifdef __APPLE__
    CHECK(makefile.find("export MACOSX_DEPLOYMENT_TARGET := 14.0\n") != std::string::npos);
    // Recorded in the command-line stamps, so changing the value rebuilds and relinks.
    CHECK(p.stamps().find("[macos_deployment_target=14.0]") != std::string::npos);
#else
    CHECK(makefile.find("MACOSX_DEPLOYMENT_TARGET") == std::string::npos);
#endif
}

#ifdef __APPLE__
TEST_CASE("A Mach-O built with macos_deployment_target carries that minos", "[macos_deployment_target]") {
    if (std::system("command -v make >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 && command -v otool >/dev/null 2>&1 && "
                    "command -v python3 >/dev/null 2>&1") != 0) {
        WARN("make/clang++/otool/python3 not available: skipping the real build");
        return;
    }
    MdtProject p("build", "macos_deployment_target = 14.0\n");
    REQUIRE(p.generated);
    REQUIRE(std::system(("make -C \"" + (p.dir / "build").string() + "\" >/dev/null 2>&1").c_str()) == 0);
    const fs::path app = p.dir / "out" / "App";
    REQUIRE(fs::exists(app));
    const std::string cmd = "otool -l \"" + app.string() + "\" | awk '/LC_BUILD_VERSION/{x=1} x&&/minos/{print $2; exit}'";
    std::string minos;
    if (FILE* pipe = popen(cmd.c_str(), "r")) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), pipe)) minos = buf;
        pclose(pipe);
    }
    while (!minos.empty() && (minos.back() == '\n' || minos.back() == '\r')) minos.pop_back();
    CHECK(minos == "14.0");
}
#endif
