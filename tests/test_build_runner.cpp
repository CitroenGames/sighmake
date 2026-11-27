#include "catch_amalgamated.hpp"
#include "common/build_runner.hpp"

using namespace vcxproj;

TEST_CASE("Build platform defaults to x64 when available", "[build_runner]") {
    auto platform = resolve_build_platform({"Win32", "x64"}, "");

    REQUIRE(platform);
    CHECK(*platform == "x64");
}

TEST_CASE("Build platform defaults to the first cached platform without x64", "[build_runner]") {
    auto platform = resolve_build_platform({"Win32", "ARM64"}, "");

    REQUIRE(platform);
    CHECK(*platform == "Win32");
}

TEST_CASE("Build platform honors explicit selection and x86 alias", "[build_runner]") {
    auto win32 = resolve_build_platform({"Win32", "x64"}, "x86");
    auto x64 = resolve_build_platform({"Win32", "x64"}, "X64");

    REQUIRE(win32);
    CHECK(*win32 == "Win32");
    REQUIRE(x64);
    CHECK(*x64 == "x64");
}

TEST_CASE("Build platform rejects an unavailable explicit selection", "[build_runner]") {
    auto platform = resolve_build_platform({"Win32", "x64"}, "ARM64");

    CHECK_FALSE(platform);
}

TEST_CASE("Build platform supports explicit selection with a legacy empty cache", "[build_runner]") {
    auto platform = resolve_build_platform({}, "x86");

    REQUIRE(platform);
    CHECK(*platform == "Win32");
}

TEST_CASE("Make project builds select the requested configuration", "[build_runner]") {
    BuildCache cache;
    cache.platforms = {"Linux"};
    BuildOptions options;
    options.project = "OpenStrike";

    CHECK(resolve_make_build_target(cache, options, "Release") == "OpenStrike.Release");
}

TEST_CASE("Make project builds retain Android target suffix", "[build_runner]") {
    BuildCache cache;
    cache.platforms = {"Android"};
    BuildOptions options;
    options.project = "Game";

    CHECK(resolve_make_build_target(cache, options, "Release") == "Game.Release.Android");
}

TEST_CASE("Make explicit targets remain unchanged", "[build_runner]") {
    BuildCache cache;
    cache.platforms = {"Linux"};
    BuildOptions options;
    options.target = "install";

    CHECK(resolve_make_build_target(cache, options, "Release") == "install");
}

// Issue #7: Android builds need GNU Make 4+ ($(file) response files); macOS's /usr/bin/make is 3.81.
TEST_CASE("gnu_make_major_version parses make --version output", "[build_runner]") {
    CHECK(gnu_make_major_version("GNU Make 3.81\nCopyright (C) 2006  Free Software Foundation, Inc.\n") == 3);
    CHECK(gnu_make_major_version("GNU Make 4.3\nBuilt for x86_64-pc-linux-gnu\n") == 4);
    CHECK(gnu_make_major_version("GNU Make 10.0\n") == 10);
    CHECK_FALSE(gnu_make_major_version("bmake: unknown option -- -\n"));
    CHECK_FALSE(gnu_make_major_version(""));
}

#ifndef _WIN32
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include "common/android_sdk.hpp"

namespace {
struct MakeEnvGuard {
    std::string name;
    std::optional<std::string> previous;
    MakeEnvGuard(const std::string& n, const std::optional<std::string>& value) : name(n) {
        if (const char* old = std::getenv(n.c_str())) previous = old;
        if (value) setenv(n.c_str(), value->c_str(), 1);
        else unsetenv(n.c_str());
    }
    ~MakeEnvGuard() {
        if (previous) setenv(name.c_str(), previous->c_str(), 1);
        else unsetenv(name.c_str());
    }
};

void write_fake_make(const std::filesystem::path& path, const std::string& version_line) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\necho '" << version_line << "'\n";
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
}
}  // namespace

TEST_CASE("Android builds use the NDK's make over a PATH make older than GNU Make 4 (issue #7)", "[build_runner]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "sighmake_test_old_make";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path ndk = root / "ndk";
    const fs::path ndk_make = ndk / "prebuilt" / android_sdk::ndk_host_tag() / "bin" / "make";
    write_fake_make(ndk_make, "GNU Make 4.3");

    MakeEnvGuard no_override("SIGHMAKE_MAKE", std::nullopt);
    MakeEnvGuard ndk_env("ANDROID_NDK_HOME", ndk.string());

    SECTION("GNU Make 3.81 on PATH") {
        write_fake_make(root / "path" / "make", "GNU Make 3.81");
        MakeEnvGuard path_env("PATH", (root / "path").string());
        std::string note;
        CHECK(resolve_make_program(true, &note) == ndk_make.string());
        CHECK(note.find("GNU Make 3") != std::string::npos);
        // Non-Android builds keep the PATH make.
        CHECK(resolve_make_program(false, nullptr) == "make");
    }
    SECTION("GNU Make 4 on PATH") {
        write_fake_make(root / "path" / "make", "GNU Make 4.4.1");
        MakeEnvGuard path_env("PATH", (root / "path").string());
        std::string note;
        CHECK(resolve_make_program(true, &note) == "make");
        CHECK(note.empty());
    }
    SECTION("no make on PATH") {
        fs::create_directories(root / "empty");
        MakeEnvGuard path_env("PATH", (root / "empty").string());
        std::string note;
        CHECK(resolve_make_program(true, &note) == ndk_make.string());
        CHECK(note.find("not found on PATH") != std::string::npos);
    }
    fs::remove_all(root, ec);
}
#endif
