#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"
#include "common/android_sdk.hpp"
#include "common/build_runner.hpp"
#include "common/target_os.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Tests for the Android / cross-target features: per-project platform filters, host conditions,
// wildcard configuration sections, NDK discovery, find_package under Android and `--build`.

namespace {

struct AndroidOsScope {
    explicit AndroidOsScope(TargetOS os = TargetOS::Android) { set_target_os(os); }
    ~AndroidOsScope() { set_target_os(std::nullopt); }
};

struct ScopedEnv {
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        const char* old = std::getenv(name);
        had_old_ = old != nullptr;
        if (old) old_ = old;
        set(value);
    }
    ~ScopedEnv() { set(had_old_ ? old_ : std::string()); }
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        if (value.empty()) unsetenv(name_); else setenv(name_, value.c_str(), 1);
#endif
    }
    const char* name_;
    bool had_old_ = false;
    std::string old_;
};

struct TempDir {
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

bool has_define(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string slurp(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Generated {
    TempDir dir{"sighmake_test_android_features"};
    Solution solution;
    std::map<std::string, std::string> files;
    std::string master;
};

Generated generate(const std::string& script) {
    Generated g;
    std::ofstream(g.dir.path / "main.cpp") << "int main() { return 0; }";
    BuildscriptParser parser;
    g.solution = parser.parse_string(script, g.dir.path.string());
    MakefileGenerator generator;
    generator.generate(g.solution, g.dir.path.string());
    for (const auto& entry : fs::directory_iterator(g.dir.path / "build")) {
        if (!entry.is_regular_file()) continue;
        g.files[entry.path().filename().string()] = slurp(entry.path());
    }
    g.master = g.files["Makefile"];
    return g;
}

} // namespace

// ============================================================================
// Per-project platform filter
// ============================================================================

TEST_CASE("Project platform filter drops configurations of other platforms", "[android][platforms]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:Everywhere]
type = lib
sources = main.cpp

[project:DesktopOnly]
type = lib
platforms = Windows, macOS
sources = main.cpp

[project:MobileOnly]
type = lib
platforms = Android
sources = main.cpp
)");
    REQUIRE(sol.projects.size() == 3);
    const auto& everywhere = sol.projects[0];
    const auto& desktop = sol.projects[1];
    const auto& mobile = sol.projects[2];

    CHECK(everywhere.configurations.count("Release|x64"));
    CHECK(everywhere.configurations.count("Release|Android"));
    CHECK(everywhere.platform_filter.empty());

    CHECK(desktop.configurations.count("Release|x64"));
    CHECK_FALSE(desktop.configurations.count("Release|Android"));
    CHECK_FALSE(desktop.configurations.count("Debug|Android"));

    CHECK_FALSE(mobile.configurations.count("Release|x64"));
    CHECK(mobile.configurations.count("Release|Android"));
}

TEST_CASE("Platform filter entries: groups, negation and case", "[android][platforms]") {
    Project p;
    p.platform_filter = {"Windows"};
    CHECK(project_supports_platform(p, "x64"));
    CHECK(project_supports_platform(p, "Win32"));
    CHECK_FALSE(project_supports_platform(p, "Android"));
    CHECK_FALSE(project_supports_platform(p, "Linux"));

    p.platform_filter = {"!android"};
    CHECK(project_supports_platform(p, "x64"));
    CHECK(project_supports_platform(p, "Linux"));
    CHECK_FALSE(project_supports_platform(p, "Android"));

    p.platform_filter = {"posix", "!Android"};
    CHECK(project_supports_platform(p, "Linux"));
    CHECK(project_supports_platform(p, "macOS"));
    CHECK_FALSE(project_supports_platform(p, "Android"));
    CHECK_FALSE(project_supports_platform(p, "x64"));

    p.platform_filter = {"MACOS"};
    CHECK(project_supports_platform(p, "macOS"));
    CHECK(project_supports_platform(p, "darwin"));
    CHECK_FALSE(project_supports_platform(p, "Linux"));

    p.platform_filter = {"x86"};
    CHECK(project_supports_platform(p, "Win32"));

    p.platform_filter.clear();
    CHECK(project_supports_platform(p, "anything"));
}

TEST_CASE("Makefile output skips projects filtered out of the Android platform", "[android][platforms][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Gfx]
type = lib
sources = main.cpp

[project:D3D12RHI]
type = lib
platforms = Windows
sources = main.cpp

[project:Game]
type = dll
sources = main.cpp
target_link_libraries(
    Gfx PUBLIC
    D3D12RHI PUBLIC
)
)");
    CHECK(g.files.count("Gfx.Release.Android"));
    CHECK(g.files.count("Game.Release.Android"));
    CHECK_FALSE(g.files.count("D3D12RHI.Release.Android"));
    CHECK(g.master.find("D3D12RHI.Release.Android") == std::string::npos);
    CHECK(g.master.find("Gfx.Release.Android") != std::string::npos);
    // The filtered dependency contributes nothing to the link line.
    CHECK(g.files["Game.Release.Android"].find("D3D12RHI") == std::string::npos);
}

TEST_CASE("Project excluded from Visual Studio when no Windows platform remains", "[android][platforms]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64, Android

[project:A]
type = lib
platforms = Android
sources = main.cpp

[project:B]
type = lib
platforms = Windows
sources = main.cpp

[project:C]
type = lib
sources = main.cpp
)");
    REQUIRE(sol.projects.size() == 3);
    CHECK(project_excluded_from_visual_studio(sol.projects[0]));
    CHECK_FALSE(project_excluded_from_visual_studio(sol.projects[1]));
    CHECK_FALSE(project_excluded_from_visual_studio(sol.projects[2]));
}

// ============================================================================
// Host conditions
// ============================================================================

TEST_CASE("Host conditions follow the machine, not --target-os", "[android][host_conditions]") {
    const char* script = R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:App]
type = lib
if(host_windows) {
    defines = HOST_IS_WINDOWS
}
if(host_posix) {
    defines = HOST_IS_POSIX
}
if(!host_windows) {
    defines = HOST_NOT_WINDOWS
}
if(host_linux) {
    defines = HOST_IS_LINUX
}
if(host_macos) {
    defines = HOST_IS_MACOS
}
if(posix) {
    defines = TARGET_IS_POSIX
}
)";
    AndroidOsScope scope;
    BuildscriptParser parser;
    auto sol = parser.parse_string(script);
    const auto& defs = sol.projects[0].project_level_preprocessor_definitions;

    // The target is Android (POSIX) regardless of the host.
    CHECK(has_define(defs, "TARGET_IS_POSIX"));
#ifdef _WIN32
    CHECK(has_define(defs, "HOST_IS_WINDOWS"));
    CHECK_FALSE(has_define(defs, "HOST_IS_POSIX"));
    CHECK_FALSE(has_define(defs, "HOST_NOT_WINDOWS"));
    CHECK_FALSE(has_define(defs, "HOST_IS_LINUX"));
#elif defined(__APPLE__)
    CHECK_FALSE(has_define(defs, "HOST_IS_WINDOWS"));
    CHECK(has_define(defs, "HOST_IS_POSIX"));
    CHECK(has_define(defs, "HOST_NOT_WINDOWS"));
    CHECK(has_define(defs, "HOST_IS_MACOS"));
#else
    CHECK_FALSE(has_define(defs, "HOST_IS_WINDOWS"));
    CHECK(has_define(defs, "HOST_IS_POSIX"));
    CHECK(has_define(defs, "HOST_NOT_WINDOWS"));
    CHECK(has_define(defs, "HOST_IS_LINUX"));
#endif
}

TEST_CASE("Host conditions select build event commands independently of the target", "[android][host_conditions]") {
    AndroidOsScope scope;
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:App]
type = lib
if(android) {
    if(host_windows) {
        prebuild = python tools/gen.py
    }
    if(host_posix) {
        prebuild = python3 tools/gen.py
    }
}
)");
    const auto it = sol.projects[0].configurations.find("Release|Android");
    REQUIRE(it != sol.projects[0].configurations.end());
#ifdef _WIN32
    CHECK(it->second.pre_build_event.command == "python tools/gen.py");
#else
    CHECK(it->second.pre_build_event.command == "python3 tools/gen.py");
#endif
}

// ============================================================================
// Wildcard and inherited configuration sections
// ============================================================================

TEST_CASE("[config:*|Android] applies to every configuration of the platform", "[android][config_wildcard]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:App]
type = exe
sources = main.cpp

[config:Debug|x64]
outdir = out/x64/Debug

[config:Release|x64]
outdir = out/x64/Release

[config:*|Android]
outdir = out/android/$(ANDROID_ABI)
defines = ON_DEVICE
)");
    const auto& cfgs = sol.projects[0].configurations;
    REQUIRE(cfgs.count("Debug|Android"));
    REQUIRE(cfgs.count("Release|Android"));
    CHECK(cfgs.at("Debug|Android").out_dir == "out/android/$(ANDROID_ABI)");
    CHECK(cfgs.at("Release|Android").out_dir == "out/android/$(ANDROID_ABI)");
    CHECK(has_define(cfgs.at("Debug|Android").cl_compile.preprocessor_definitions, "ON_DEVICE"));
    CHECK(has_define(cfgs.at("Release|Android").cl_compile.preprocessor_definitions, "ON_DEVICE"));
    CHECK_FALSE(has_define(cfgs.at("Release|x64").cl_compile.preprocessor_definitions, "ON_DEVICE"));
    CHECK(cfgs.at("Release|x64").out_dir.find("out") != std::string::npos);
}

TEST_CASE("[config:*|Android] keeps the platforms declared in [solution]", "[android][config_wildcard]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:App]
type = exe
sources = main.cpp

[config:*|Android]
defines = ON_DEVICE
)");
    CHECK(std::find(sol.platforms.begin(), sol.platforms.end(), "x64") != sol.platforms.end());
    CHECK(std::find(sol.platforms.begin(), sol.platforms.end(), "Android") != sol.platforms.end());
    CHECK(std::find(sol.configurations.begin(), sol.configurations.end(), "Debug") != sol.configurations.end());
    CHECK(sol.projects[0].configurations.count("Debug|x64"));
}

TEST_CASE("[config:Release|*] and bracket wildcards expand over the matrix", "[android][config_wildcard]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:App]
type = exe
sources = main.cpp
outdir[*|Android] = bin/android/$(ANDROID_ABI)
intdir[Release|*] = obj/release

[config:Release|*]
defines = SHIPPING
)");
    const auto& cfgs = sol.projects[0].configurations;
    CHECK(cfgs.at("Debug|Android").out_dir == "bin/android/$(ANDROID_ABI)");
    CHECK(cfgs.at("Release|Android").out_dir == "bin/android/$(ANDROID_ABI)");
    CHECK(cfgs.at("Release|x64").out_dir != "bin/android/$(ANDROID_ABI)");
    CHECK(has_define(cfgs.at("Release|x64").cl_compile.preprocessor_definitions, "SHIPPING"));
    CHECK(has_define(cfgs.at("Release|Android").cl_compile.preprocessor_definitions, "SHIPPING"));
    CHECK_FALSE(has_define(cfgs.at("Debug|Android").cl_compile.preprocessor_definitions, "SHIPPING"));
    CHECK_FALSE(cfgs.at("Release|x64").int_dir.empty());
}

TEST_CASE("A configuration can inherit from another platform's cell", "[android][config_wildcard]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64, Android

[project:App]
type = exe
sources = main.cpp

[config:Release|x64]
optimization = MaxSpeed
warning_level = Level4
outdir = out/x64

[config:Release|Android] : Template:Release|x64
outdir = out/android/$(ANDROID_ABI)
)");
    const auto& android = sol.projects[0].configurations.at("Release|Android");
    CHECK(android.out_dir == "out/android/$(ANDROID_ABI)");
    CHECK(android.cl_compile.optimization == "MaxSpeed");
    CHECK(android.cl_compile.warning_level == "Level4");
}

TEST_CASE("Wildcard section with a wildcard template copies the matching configuration", "[android][config_wildcard]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:App]
type = exe
sources = main.cpp

[config:Debug|x64]
optimization = Disabled

[config:Release|x64]
optimization = MaxSpeed

[config:*|Android] : Template:*|x64
outdir = out/android/$(ANDROID_ABI)
)");
    const auto& cfgs = sol.projects[0].configurations;
    CHECK(cfgs.at("Debug|Android").cl_compile.optimization == "Disabled");
    CHECK(cfgs.at("Release|Android").cl_compile.optimization == "MaxSpeed");
    CHECK(cfgs.at("Release|Android").out_dir == "out/android/$(ANDROID_ABI)");
}

// ============================================================================
// Makefile generator: output rules, NDK discovery, output tokens
// ============================================================================

TEST_CASE("Android archive rule creates the output directory before the response file", "[android][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Core]
type = lib
sources = main.cpp
)");
    const std::string& mk = g.files["Core.Release.Android"];
    const auto mkdir_pos = mk.find("$(shell mkdir -p $(dir $@))$(file >$@.rsp,$(OBJS))");
    REQUIRE(mkdir_pos != std::string::npos);
    CHECK(mk.find("$(AR) rcs $@ @$@.rsp") > mkdir_pos);
}

TEST_CASE("Android makefiles discover the NDK from the SDK when ANDROID_NDK_HOME is unset", "[android][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Core]
type = lib
sources = main.cpp
)");
    const std::string& mk = g.files["Core.Release.Android"];
    CHECK(mk.find("ANDROID_NDK_HOME := $(ANDROID_NDK_ROOT)") != std::string::npos);
    CHECK(mk.find("ANDROID_NDK_HOME := $(ANDROID_NDK)") != std::string::npos);
    CHECK(mk.find("$(wildcard $(ANDROID_HOME)/ndk/*)") != std::string::npos);
    CHECK(mk.find("$(wildcard $(ANDROID_SDK_ROOT)/ndk/*)") != std::string::npos);
    CHECK(mk.find("no NDK was found") != std::string::npos);
}

TEST_CASE("Makefile embeds the NDK found at generation time as a last resort", "[android][makefile_generator]") {
    TempDir sdk("sighmake_test_sdk_embed");
    fs::create_directories(sdk.path / "ndk" / "26.1.10909125");
    fs::create_directories(sdk.path / "ndk" / "27.0.12077973");
    ScopedEnv home("ANDROID_HOME", sdk.path.string());
    ScopedEnv ndk_home("ANDROID_NDK_HOME", "");
    ScopedEnv ndk_root("ANDROID_NDK_ROOT", "");
    ScopedEnv ndk("ANDROID_NDK", "");

    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Core]
type = lib
sources = main.cpp
)");
    const std::string& mk = g.files["Core.Release.Android"];
    CHECK(mk.find("27.0.12077973") != std::string::npos);
    CHECK(mk.find("26.1.10909125") == std::string::npos);
}

TEST_CASE("Output directory tokens expand per configuration and anchor to the buildscript", "[android][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:Core]
type = dll
sources = main.cpp
outdir = bin/$(Platform)/$(Configuration)/$(ANDROID_ABI)
)");
    const std::string& release = g.files["Core.Release.Android"];
    const std::string& debug = g.files["Core.Debug.Android"];
    CHECK(release.find("bin/Android/Release/$(ANDROID_ABI)/libCore.so") != std::string::npos);
    CHECK(debug.find("bin/Android/Debug/$(ANDROID_ABI)/libCore.so") != std::string::npos);
    CHECK(release.find("$(Configuration)") == std::string::npos);
}

#ifndef _WIN32
TEST_CASE("Receipt python is probed once by the master Makefile", "[android][makefile_generator]") {
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:App]
type = exe
sources = main.cpp
)");
    CHECK(g.master.find("export PYTHON") != std::string::npos);
    CHECK(g.files["App.Release"].find("ifeq ($(origin PYTHON),undefined)") != std::string::npos);
}
#endif

// ============================================================================
// SDK / NDK discovery
// ============================================================================

TEST_CASE("version_less compares dotted versions numerically", "[android][sdk]") {
    using android_sdk::version_less;
    CHECK(version_less("26.1.10909125", "27.0.12077973"));
    CHECK(version_less("9.0.0", "10.0.0"));
    CHECK(version_less("27.0.12077973", "27.1.12297006"));
    CHECK_FALSE(version_less("28.0.0", "27.9.9"));
    CHECK_FALSE(version_less("27.0", "27.0"));
}

TEST_CASE("find_ndk prefers explicit variables, then the newest SDK ndk", "[android][sdk]") {
    TempDir sdk("sighmake_test_sdk_find");
    fs::create_directories(sdk.path / "ndk" / "9.0.1");
    fs::create_directories(sdk.path / "ndk" / "27.0.12077973");
    fs::create_directories(sdk.path / "ndk" / "26.3.11579264");
    fs::create_directories(sdk.path / "explicit_ndk");

    ScopedEnv home("ANDROID_HOME", sdk.path.string());
    ScopedEnv sdk_root("ANDROID_SDK_ROOT", "");
    ScopedEnv ndk_home("ANDROID_NDK_HOME", "");
    ScopedEnv ndk_root("ANDROID_NDK_ROOT", "");
    ScopedEnv ndk("ANDROID_NDK", "");

    auto found = android_sdk::find_ndk();
    REQUIRE(found);
    CHECK(found->filename() == "27.0.12077973");

    ScopedEnv explicit_home("ANDROID_NDK_HOME", (sdk.path / "explicit_ndk").string());
    auto explicit_found = android_sdk::find_ndk();
    REQUIRE(explicit_found);
    CHECK(explicit_found->filename() == "explicit_ndk");
}

TEST_CASE("find_ndk_make locates the make program shipped with the NDK", "[android][sdk]") {
    TempDir ndk("sighmake_test_ndk_make");
    CHECK_FALSE(android_sdk::find_ndk_make(ndk.path));
    const fs::path bin = ndk.path / "prebuilt" / android_sdk::ndk_host_tag() / "bin";
    fs::create_directories(bin);
#ifdef _WIN32
    std::ofstream(bin / "make.exe") << "x";
#else
    std::ofstream(bin / "make") << "x";
#endif
    CHECK(android_sdk::find_ndk_make(ndk.path));
}

// ============================================================================
// find_package under Android
// ============================================================================

TEST_CASE("find_package(Vulkan) under Android resolves to the NDK sysroot", "[android][find_package]") {
    TempDir ndk("sighmake_test_ndk_vulkan");
    const fs::path include = android_sdk::ndk_sysroot(ndk.path) / "usr" / "include" / "vulkan";
    fs::create_directories(include);
    std::ofstream(include / "vulkan.h") << "// stub";

    ScopedEnv ndk_home("ANDROID_NDK_HOME", ndk.path.string());
    ScopedEnv vulkan_sdk("VULKAN_SDK", "/definitely/not/a/vulkan/sdk");
    AndroidOsScope scope;

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

find_package(Vulkan REQUIRED)

[project:App]
type = dll
sources = main.cpp
target_link_libraries(Vulkan)
)");
    REQUIRE(sol.found_packages.count("Vulkan"));
    const auto& vulkan = sol.found_packages["Vulkan"];
    CHECK(vulkan.found);
    CHECK(vulkan.libraries == "vulkan");
    // The sysroot is searched by the compiler itself; adding it as -I would reorder libc++ headers.
    CHECK(vulkan.include_dirs.empty());
    CHECK(vulkan.library_dirs.empty());
}

TEST_CASE("find_package(Vulkan) under Android fails clearly without an NDK", "[android][find_package]") {
    TempDir empty("sighmake_test_no_ndk");
    ScopedEnv home("ANDROID_HOME", (empty.path / "none").string());
    ScopedEnv sdk_root("ANDROID_SDK_ROOT", "");
    ScopedEnv ndk_home("ANDROID_NDK_HOME", "");
    ScopedEnv ndk_root("ANDROID_NDK_ROOT", "");
    ScopedEnv ndk("ANDROID_NDK", "");
    ScopedEnv local("LOCALAPPDATA", empty.path.string());
    ScopedEnv user_home("HOME", empty.path.string());
    AndroidOsScope scope;

    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

find_package(Vulkan)

[project:App]
type = dll
sources = main.cpp
)");
    CHECK_FALSE(sol.found_packages.count("Vulkan"));
}

// ============================================================================
// sighmake --build with Android
// ============================================================================

TEST_CASE("Make builds pick the Android targets from --platform", "[android][build_runner]") {
    BuildCache cache;
    cache.platforms = {"Linux", "Android"};
    BuildOptions options;

    CHECK(resolve_make_build_target(cache, options, "Release") == "Release");
    options.platform = "Android";
    CHECK(make_build_targets_android(cache, options));
    CHECK(resolve_make_build_target(cache, options, "Release") == "Release.Android");
    options.project = "Game";
    CHECK(resolve_make_build_target(cache, options, "Release") == "Game.Release.Android");

    BuildOptions desktop;
    desktop.platform = "Linux";
    CHECK_FALSE(make_build_targets_android(cache, desktop));
    CHECK(resolve_make_build_target(cache, desktop, "Debug") == "Debug");
}

TEST_CASE("Android-only Make caches build the Android targets by default", "[android][build_runner]") {
    BuildCache cache;
    cache.platforms = {"Android"};
    BuildOptions options;
    CHECK(make_build_targets_android(cache, options));
    CHECK(resolve_make_build_target(cache, options, "Release") == "Release.Android");
}
