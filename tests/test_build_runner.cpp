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
