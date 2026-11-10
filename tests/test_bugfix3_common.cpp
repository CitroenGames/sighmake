#include "pch.h"
#include "catch_amalgamated.hpp"
#include "common/build_runner.hpp"
#include "common/build_cache.hpp"

#ifndef _WIN32
#include <cstdlib>
#include <unistd.h>
#endif

using namespace vcxproj;
namespace fs = std::filesystem;

namespace {

struct Bugfix3TempDir {
    fs::path path;
    explicit Bugfix3TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~Bugfix3TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_text3(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#ifndef _WIN32
struct Env3Guard {
    std::string name;
    std::optional<std::string> previous;
    Env3Guard(const std::string& n, const std::string& value) : name(n) {
        if (const char* old = std::getenv(n.c_str())) previous = old;
        setenv(n.c_str(), value.c_str(), 1);
    }
    ~Env3Guard() {
        if (previous) setenv(name.c_str(), previous->c_str(), 1);
        else unsetenv(name.c_str());
    }
};

// A makefile cache in `root` whose "make" is the script `make_body` (sh). Returns the script path.
fs::path make_fake_make_project(const fs::path& root, const std::string& make_body) {
    fs::create_directories(root / "build");
    std::ofstream(root / "build" / "Makefile") << "all:\n";
    BuildCache cache;
    cache.generator = "makefile";
    cache.solution_name = "S";
    cache.configurations = {"Debug"};
    cache.platforms = {"Linux"};
    cache.build_dir = "build";
    REQUIRE(cache.write(root.string()));
    fs::path script = root / "fakemake.sh";
    std::ofstream(script) << "#!/bin/sh\n" << make_body;
    fs::permissions(script, fs::perms::owner_all);
    return script;
}
#endif

} // namespace

TEST_CASE("bugfix3 common: system() wait status maps to the exit code", "[bugfix3_common]") {
#ifdef _WIN32
    CHECK(exit_code_from_system_status(3) == 3);
#else
    int status = std::system("exit 2");
    CHECK(exit_code_from_system_status(status) == 2);
    status = std::system("exit 0");
    CHECK(exit_code_from_system_status(status) == 0);
    status = std::system("kill -9 $$");
    CHECK(exit_code_from_system_status(status) == 128 + 9);
#endif
}

TEST_CASE("bugfix3 common: --build returns the failing make's exit code", "[bugfix3_common]") {
#ifdef _WIN32
    SUCCEED("POSIX shell script test");
#else
    Bugfix3TempDir dir("sighmake_bf3_exit");
    fs::path make = make_fake_make_project(dir.path, "exit 2\n");
    Env3Guard guard("SIGHMAKE_MAKE", make.string());
    BuildOptions options;
    options.directory = dir.path.string();
    // system() reports 512 for exit code 2; main() would truncate that to 0 (success).
    CHECK(BuildRunner::run(options) == 2);
#endif
}

TEST_CASE("bugfix3 common: --build passes paths with shell metacharacters literally", "[bugfix3_common]") {
#ifdef _WIN32
    SUCCEED("POSIX shell test");
#else
    Bugfix3TempDir base("sighmake_bf3_quote");
    fs::path root = base.path / "we$HOME ird `x` it's";
    fs::create_directories(root);
    fs::path record = base.path / "args.txt";
    fs::path make = make_fake_make_project(root, "printf '%s\\n' \"$@\" > '" + record.string() + "'\n");
    Env3Guard guard("SIGHMAKE_MAKE", make.string());
    BuildOptions options;
    options.directory = root.string();
    REQUIRE(BuildRunner::run(options) == 0);
    std::string recorded = read_text3(record);
    std::string expected_dir = (fs::canonical(root) / "build").string();
    CHECK(recorded.find("-C\n" + expected_dir + "\n") == 0);
#endif
}
