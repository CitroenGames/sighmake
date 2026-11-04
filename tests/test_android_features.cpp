#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"
#include "common/android_sdk.hpp"
#include "common/build_runner.hpp"
#include "common/target_os.hpp"
#include "generators/vcxproj_generator.hpp"
#include "pugixml.hpp"

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

// ============================================================================
// Shared libraries and executables for Android
// ============================================================================

TEST_CASE("Android shared library links through a response file with soname and page alignment", "[android][makefile_generator][shared]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Base]
type = lib
sources = main.cpp

[project:Plugin]
type = shared
target_name = plugin
sources = main.cpp
target_link_libraries(
    Base PUBLIC
)
)");
    const std::string& mk = g.files["Plugin.Release.Android"];
    // lib<name>.so naming for loaders and APK packaging
    CHECK(mk.find("libplugin.so") != std::string::npos);
    CHECK(mk.find("$(shell mkdir -p $(dir $@))$(file >$@.rsp,$(OBJS) -Wl,--start-group $(LDLIBS) -Wl,--end-group)") != std::string::npos);
    CHECK(mk.find("-shared -Wl,-soname,$(notdir $@) $(LDFLAGS) $(ANDROID_LINK_FLAGS) -o $@ @$@.rsp") != std::string::npos);
    CHECK(mk.find("@rm -f $@.rsp") != std::string::npos);
    CHECK(mk.find("ANDROID_MAX_PAGE_SIZE ?= 16384") != std::string::npos);
    CHECK(mk.find("-Wl,-z,max-page-size=$(ANDROID_MAX_PAGE_SIZE)") != std::string::npos);
    // The object list is no longer on the command line.
    CHECK(mk.find("-o $@ $(OBJS)") == std::string::npos);
}

TEST_CASE("Android executables use the response file but no soname", "[android][makefile_generator][shared]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Tool]
type = exe
sources = main.cpp
)");
    const std::string& mk = g.files["Tool.Release.Android"];
    CHECK(mk.find("$(CXX) $(LDFLAGS) $(ANDROID_LINK_FLAGS) -o $@ @$@.rsp") != std::string::npos);
    CHECK(mk.find("-soname") == std::string::npos);
    CHECK(mk.find("-shared") == std::string::npos);
}

TEST_CASE("Desktop shared libraries are unchanged by the Android link rule", "[makefile_generator][shared]") {
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Plugin]
type = dll
sources = main.cpp
)");
    const std::string& mk = g.files["Plugin.Release"];
    CHECK(mk.find("-soname") == std::string::npos);
    CHECK(mk.find("ANDROID_LINK_FLAGS") == std::string::npos);
    CHECK(mk.find("@$@.rsp") == std::string::npos);
}

// ============================================================================
// external_cmake
// ============================================================================

TEST_CASE("external_cmake settings are parsed", "[android][external_cmake]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:SDL3]
type = external_cmake
cmake_git = https://example.com/sdl.git
cmake_git_tag = release-3.2.8
cmake_options = -DSDL_SHARED=ON, -DSDL_STATIC=OFF
cmake_targets = SDL3-shared
cmake_outputs = libSDL3.so
cmake_generator = Ninja
cmake_source_subdir = cmake
public_includes = third_party/sdl/include
)", "/work/proj");
    REQUIRE(sol.projects.size() == 1);
    const auto& project = sol.projects[0];
    const auto& ext = project.external_cmake;
    CHECK(project.configurations.at("Release|Android").config_type == "ExternalCMake");
    CHECK(ext.git_url == "https://example.com/sdl.git");
    CHECK(ext.git_ref == "release-3.2.8");
    CHECK(ext.options == std::vector<std::string>{"-DSDL_SHARED=ON", "-DSDL_STATIC=OFF"});
    CHECK(ext.targets == std::vector<std::string>{"SDL3-shared"});
    CHECK(ext.outputs == std::vector<std::string>{"libSDL3.so"});
    CHECK(ext.generator == "Ninja");
    CHECK(ext.source_subdir == "cmake");
    // Default checkout location is below the generated build directory.
    const std::string checkout = fs::path(ext.checkout_dir).generic_string();
    CHECK(checkout.find("build/_external/SDL3/source") != std::string::npos);
    CHECK(project_excluded_from_visual_studio(project));
}

TEST_CASE("external_cmake local source makefile configures, builds and copies outputs", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:Native]
type = external_cmake
cmake_source = vendor/native
cmake_options = -DNATIVE_TESTS=OFF, -DNATIVE_SHARED=ON
cmake_targets = native
cmake_outputs = lib/libnative.so, native.json
)");
    REQUIRE(g.files.count("Native.Release.Android"));
    REQUIRE(g.files.count("Native.Debug.Android"));
    const std::string& mk = g.files["Native.Release.Android"];
    CHECK(mk.find("external CMake project") != std::string::npos);
    CHECK(mk.find("EXT_SOURCE = ../vendor/native") != std::string::npos);
    // Per-configuration, per-ABI build directory
    CHECK(mk.find("Release/android/$(ANDROID_ABI)/obj/Native") != std::string::npos);
    CHECK(g.files["Native.Debug.Android"].find("Debug/android/$(ANDROID_ABI)/obj/Native") != std::string::npos);
    CHECK(mk.find("EXT_BUILD_TYPE = Release") != std::string::npos);
    CHECK(mk.find("android.toolchain.cmake") != std::string::npos);
    CHECK(mk.find("-DANDROID_ABI=$(ANDROID_ABI)") != std::string::npos);
    CHECK(mk.find("-DANDROID_PLATFORM=android-$(ANDROID_API)") != std::string::npos);
    CHECK(mk.find("EXT_OPTIONS = -DNATIVE_TESTS=OFF -DNATIVE_SHARED=ON") != std::string::npos);
    CHECK(mk.find("EXT_TARGETS = --target native") != std::string::npos);
    CHECK(mk.find("$(CMAKE) -S \"$(EXT_SOURCE)\" -B \"$(EXT_BUILD_DIR)\"") != std::string::npos);
    CHECK(mk.find("$(CMAKE) --build \"$(EXT_BUILD_DIR)\" --config $(EXT_CONFIG) $(EXT_TARGETS) $(EXT_PARALLEL)") != std::string::npos);
    CHECK(mk.find("$(EXT_BUILD_DIR)/lib/libnative.so") != std::string::npos);
    CHECK(mk.find("$(EXT_OUT_DIR)/libnative.so") != std::string::npos);
    CHECK(mk.find("$(EXT_OUT_DIR)/native.json") != std::string::npos);
    CHECK(mk.find("clone") == std::string::npos);
    CHECK(g.master.find("Native.Release.Android") != std::string::npos);
}

TEST_CASE("external_cmake git source is pinned and re-fetched when the pin changes", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:SDL3]
type = external_cmake
cmake_git = https://example.com/sdl.git
cmake_git_tag = release-3.2.8
cmake_outputs = libSDL3.so

[project:Pinned]
type = external_cmake
cmake_git = https://example.com/other.git
cmake_git_tag = 0123456789abcdef0123456789abcdef01234567
cmake_outputs = libother.so
)");
    const std::string& tag = g.files["SDL3.Release.Android"];
    CHECK(tag.find("$(GIT) clone -q --depth 1 --branch release-3.2.8 https://example.com/sdl.git \"$(EXT_SOURCE)\"") != std::string::npos);
    CHECK(tag.find("EXT_PIN = https://example.com/sdl.git@release-3.2.8") != std::string::npos);
    CHECK(tag.find("EXT_STAMP = $(EXT_SOURCE).pin") != std::string::npos);
    CHECK(tag.find(".PHONY: $(EXT_STAMP)") != std::string::npos);
    CHECK(tag.find("external-build: $(EXT_STAMP)") != std::string::npos);
    CHECK(tag.find("EXT_SOURCE = _external/SDL3/source") != std::string::npos);

    const std::string& commit = g.files["Pinned.Release.Android"];
    CHECK(commit.find("fetch -q --depth 1 origin 0123456789abcdef0123456789abcdef01234567") != std::string::npos);
    CHECK(commit.find("checkout -q FETCH_HEAD") != std::string::npos);
    CHECK(commit.find("--branch") == std::string::npos);
}

TEST_CASE("Projects depending on an external_cmake project link its libraries", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:SDL3]
type = external_cmake
cmake_source = vendor/sdl
cmake_outputs = libSDL3.so, SDL3.json
public_includes = vendor/sdl/include
outdir = bin/android/$(ANDROID_ABI)

[project:Game]
type = dll
sources = main.cpp
outdir = bin/android/$(ANDROID_ABI)
target_link_libraries(
    SDL3 PUBLIC
)
)");
    const std::string& game = g.files["Game.Release.Android"];
    CHECK(game.find("bin/android/$(ANDROID_ABI)/libSDL3.so") != std::string::npos);
    // Only libraries become link inputs, not other produced files.
    CHECK(game.find("SDL3.json") == std::string::npos);
    CHECK(game.find("-I\"../vendor/sdl/include\"") != std::string::npos);
    // The master Makefile builds SDL3 before Game.
    CHECK(g.master.find("Game.Release.Android: SDL3.Release.Android") != std::string::npos);
}

TEST_CASE("external_cmake without a source is rejected", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    TempDir dir("sighmake_test_external_invalid");
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Broken]
type = external_cmake
cmake_outputs = libbroken.so
)", dir.path.string());
    MakefileGenerator generator;
    CHECK_FALSE(generator.generate(sol, dir.path.string()));
}

// ============================================================================
// android_apk
// ============================================================================

TEST_CASE("android_apk settings are parsed and the project is Android-only", "[android][apk]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:Game]
type = apk
android_package = com.example.game
android_label = My Game
android_native_lib = game
android_min_sdk = 26
android_permissions = INTERNET, com.example.CUSTOM
android_features = glEsVersion=0x00030000, android.hardware.vulkan.level:false
android_assets = content, more
android_java = java
)", "/work/proj");
    REQUIRE(sol.projects.size() == 1);
    const auto& project = sol.projects[0];
    const auto& apk = project.android_apk;
    CHECK(apk.package == "com.example.game");
    CHECK(apk.label == "My Game");
    CHECK(apk.native_lib == "game");
    CHECK(apk.min_sdk == "26");
    CHECK(apk.permissions == std::vector<std::string>{"INTERNET", "com.example.CUSTOM"});
    CHECK(apk.features.size() == 2);
    CHECK(apk.assets.size() == 2);
    CHECK(apk.java.size() == 1);
    CHECK(project.configurations.count("Release|Android") == 1);
    CHECK(project.configurations.count("Release|x64") == 0);
    CHECK(project.configurations.at("Release|Android").config_type == "AndroidApk");
}

TEST_CASE("android_apk makefile packages dependency libraries, assets and signs", "[android][apk][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:Game]
type = dll
sources = main.cpp
target_name = game
outdir = bin/android/$(ANDROID_ABI)

[project:GameApk]
type = apk
android_package = com.example.game
android_native_lib = game
android_permissions = INTERNET
android_assets = content
target_name = GameApp
outdir = bin/android
target_link_libraries(
    Game PUBLIC
)
)");
    REQUIRE(g.files.count("GameApk.Release.Android"));
    const std::string& mk = g.files["GameApk.Release.Android"];
    CHECK(mk.find("APK_NAME = GameApp.apk") != std::string::npos);
    CHECK(mk.find("bin/android/$(ANDROID_ABI)/libgame.so") != std::string::npos);
    CHECK(mk.find("$(AAPT2) link") != std::string::npos);
    CHECK(mk.find("--no-compress") != std::string::npos);
    CHECK(mk.find("$(ZIPALIGN)") != std::string::npos);
    CHECK(mk.find("$(APKSIGNER)") != std::string::npos);
    CHECK(mk.find("SIGHMAKE_SKIP_PACKAGING") != std::string::npos);
    CHECK(mk.find("ANDROID_APK_ABIS") != std::string::npos);
    CHECK(mk.find("APK_SIGN_MODE = $(if $(filter auto,$(ANDROID_SIGN)),release,") != std::string::npos);
    CHECK(g.files["GameApk.Debug.Android"].find("APK_SIGN_MODE = $(if $(filter auto,$(ANDROID_SIGN)),debug,") != std::string::npos);
    // No Java code: the generated manifest is a pure NativeActivity one.
    const std::string manifest = g.files["GameApk.Release.AndroidManifest.xml"];
    CHECK(manifest.find("package=\"com.example.game\"") != std::string::npos);
    CHECK(manifest.find("android.app.NativeActivity") != std::string::npos);
    CHECK(manifest.find("android:value=\"game\"") != std::string::npos);
    CHECK(manifest.find("android.permission.INTERNET") != std::string::npos);
    CHECK(manifest.find("android:hasCode=\"false\"") != std::string::npos);
    CHECK(g.master.find("GameApk.Release.Android: Game.Release.Android") != std::string::npos);
}

TEST_CASE("android_apk without a package or manifest is rejected", "[android][apk][makefile_generator]") {
    AndroidOsScope scope;
    TempDir dir("sighmake_test_apk_invalid");
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Broken]
type = apk
)", dir.path.string());
    MakefileGenerator generator;
    CHECK_FALSE(generator.generate(sol, dir.path.string()));
}

TEST_CASE("split_abi_list accepts commas, spaces and semicolons", "[android][build_runner]") {
    CHECK(split_abi_list("arm64-v8a,x86_64") == std::vector<std::string>{"arm64-v8a", "x86_64"});
    CHECK(split_abi_list("arm64-v8a x86_64;x86") == std::vector<std::string>{"arm64-v8a", "x86_64", "x86"});
    CHECK(split_abi_list("").empty());
}

// ============================================================================
// Platform-scoped dependencies, sources and types
// ============================================================================

TEST_CASE("target_link_libraries and dependencies inside if(Android) are scoped to Android", "[android][scope]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64, Android

[project:Sdl]
type = external_cmake
cmake_source = vendor/sdl
cmake_outputs = libSDL3.so
public_includes = vendor/sdl/include

[project:Common]
type = lib
sources = common.cpp

[project:Headers]
type = lib
sources = headers.cpp

[project:Launcher]
type = exe
sources = main.cpp
target_link_libraries(
    Common PUBLIC
)
if(Android) {
    target_link_libraries(
        Sdl PUBLIC
    )
    dependencies = Headers
}
)", "/work/proj");
    const Project* launcher = nullptr;
    for (const auto& p : sol.projects) if (p.name == "Launcher") launcher = &p;
    REQUIRE(launcher);
    REQUIRE(launcher->project_references.size() == 3);
    for (const auto& dep : launcher->project_references) {
        if (dep.name == "Common") {
            CHECK(dep.platforms.empty());
            CHECK(dependency_applies_to_platform(dep, "x64"));
        } else {
            CHECK(dep.platforms == std::vector<std::string>{"Android"});
            CHECK(dependency_applies_to_platform(dep, "Android"));
            CHECK_FALSE(dependency_applies_to_platform(dep, "x64"));
            CHECK_FALSE(dependency_applies_to_platform(dep, "Linux"));
        }
    }
    // Usage requirements of the Android-only project only reach the Android cell.
    const auto& android_includes = launcher->configurations.at("Release|Android").cl_compile.additional_include_directories;
    const auto& desktop_includes = launcher->configurations.at("Release|x64").cl_compile.additional_include_directories;
    auto contains_sdl = [](const std::vector<std::string>& dirs) {
        return std::any_of(dirs.begin(), dirs.end(), [](std::string d) {
            std::replace(d.begin(), d.end(), '\\', '/');
            return d.find("vendor/sdl/include") != std::string::npos;
        });
    };
    CHECK(contains_sdl(android_includes));
    CHECK_FALSE(contains_sdl(desktop_includes));
}

TEST_CASE("A repeated dependency widens its platform scope", "[android][scope]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64, Android

[project:Lib]
type = lib
[project:App]
type = exe
if(Android) {
    dependencies = Lib
}
if(x64) {
    dependencies = Lib
}
)", "/work/proj");
    const auto& refs = sol.projects[1].project_references;
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].platforms.size() == 2);
    CHECK(dependency_applies_to_platform(refs[0], "x64"));
    CHECK(dependency_applies_to_platform(refs[0], "Android"));
}

TEST_CASE("Platform-scoped declarations are pruned for generators without that platform", "[android][scope]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64, Android

[project:Sdl]
type = lib
[project:Launcher]
type = exe
sources = {
    main.cpp
    droid.cpp [android]
    desktop.cpp [!android]
}
if(Android) {
    dependencies = Sdl
    sources = block.cpp
}
)", "/work/proj");
    REQUIRE(sol.projects[1].sources.size() == 4);
    prune_platform_scoped_declarations(sol, [](const std::string& platform) { return !is_unix_platform(platform); });
    CHECK(sol.projects[1].project_references.empty());
    std::vector<std::string> names;
    for (const auto& src : sol.projects[1].sources) names.push_back(fs::path(src.path).filename().string());
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"desktop.cpp", "main.cpp"});
}

TEST_CASE("Sources scoped by [android] tags and if(Android) blocks reach Android makefiles", "[android][scope][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android, Linux

[project:Native]
type = dll
sources = {
    main.cpp
    droid.cpp [android]
    desktop.cpp [!android]
}
if(Android) {
    sources = block.cpp
}
)");
    // --target-os android: [!android] files are not part of the build at all
    const std::string& mk = g.files["Native.Release.Android"];
    REQUIRE_FALSE(mk.empty());
    CHECK(mk.find("main.cpp") != std::string::npos);
    CHECK(mk.find("droid.cpp") != std::string::npos);
    CHECK(mk.find("block.cpp") != std::string::npos);
    CHECK(mk.find("desktop.cpp") == std::string::npos);
}

TEST_CASE("Sources scoped to Android are skipped in the host makefile of a mixed solution", "[android][scope][makefile_generator]") {
    TempDir dir("sighmake_test_scoped_sources");
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android, Linux

[project:Native]
type = dll
sources = {
    main.cpp
    droid.cpp [android]
}
)", dir.path.string());
    MakefileGenerator generator;
    generator.generate(sol, dir.path.string());
    const std::string host = slurp(dir.path / "build" / "Native.Release");
    const std::string android = slurp(dir.path / "build" / "Native.Release.Android");
    REQUIRE_FALSE(host.empty());
    REQUIRE_FALSE(android.empty());
    CHECK(host.find("main.cpp") != std::string::npos);
    CHECK(host.find("droid.cpp") == std::string::npos);
    CHECK(android.find("droid.cpp") != std::string::npos);
}

TEST_CASE("type can be set per platform inside if(Android)", "[android][scope]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:Game]
type = exe
sources = main.cpp
if(Android) {
    type = dll
}
)", "/work/proj");
    const auto& project = sol.projects[0];
    CHECK(project.configurations.at("Release|x64").config_type == "Application");
    CHECK(project.configurations.at("Debug|x64").config_type == "Application");
    CHECK(project.configurations.at("Release|Android").config_type == "DynamicLibrary");
    CHECK(project.configurations.at("Debug|Android").config_type == "DynamicLibrary");
}

#ifdef _WIN32
TEST_CASE("Android-only blocks do not change the desktop Visual Studio output", "[android][scope][vcxproj_generator]") {
    auto generate_launcher = [](const std::string& android_block, const std::string& name) {
        TempDir dir(name);
        std::ofstream(dir.path / "main.cpp") << "int main() { return 0; }";
        const std::string script = std::string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64, Android

[project:Sdl]
type = external_cmake
cmake_source = vendor/sdl
cmake_outputs = libSDL3.so

[project:Headers]
type = lib
platforms = Android
sources = main.cpp

[project:Launcher]
type = exe
sources = main.cpp
)") + android_block;
        BuildscriptParser parser;
        Solution sol = parser.parse_string(script, dir.path.string());
        VcxprojGenerator generator;
        generator.generate(sol, dir.path.string());
        std::string text;
        for (const auto& entry : fs::recursive_directory_iterator(dir.path)) {
            if (entry.path().filename() == "Launcher_.vcxproj" || entry.path().filename() == "Launcher.vcxproj")
                text = slurp(entry.path());
        }
        // The generated files embed the temporary directory name in some paths; normalise it.
        for (size_t pos = 0; (pos = text.find(name, pos)) != std::string::npos;) text.replace(pos, name.size(), "DIR");
        return text;
    };
    const std::string with_block = generate_launcher(R"(
if(Android) {
    target_link_libraries(
        Sdl PUBLIC
    )
    dependencies = Headers
    sources = droid.cpp
}
)", "sighmake_scope_a");
    const std::string without_block = generate_launcher("", "sighmake_scope_b");
    REQUIRE_FALSE(with_block.empty());
    CHECK(with_block == without_block);
    CHECK(with_block.find("Headers") == std::string::npos);
    CHECK(with_block.find("Sdl") == std::string::npos);
    CHECK(with_block.find("droid.cpp") == std::string::npos);
}
#endif

// ============================================================================
// external_cmake: job limit and header-only projects
// ============================================================================

TEST_CASE("external_cmake honors the make job limit", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Native]
type = external_cmake
cmake_source = vendor/native
cmake_outputs = libnative.so
)");
    const std::string& mk = g.files["Native.Release.Android"];
    CHECK(mk.find("EXT_JOBS = $(strip $(or $(SIGHMAKE_JOBS),") != std::string::npos);
    CHECK(mk.find("$(filter -j%,$(MAKEFLAGS))") != std::string::npos);
    CHECK(mk.find("EXT_PARALLEL = --parallel $(shell n='$(EXT_JOBS)'; expr") != std::string::npos);
    CHECK(mk.find("$(EXT_TARGETS) $(EXT_PARALLEL)") != std::string::npos);
}

TEST_CASE("external_cmake without outputs is a header-only project", "[android][external_cmake][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Headers]
type = external_cmake
cmake_source = vendor/headers
cmake_build = false
public_includes = vendor/headers/include

[project:Game]
type = dll
sources = main.cpp
target_link_libraries(
    Headers PUBLIC
)
)");
    const std::string& headers = g.files["Headers.Release.Android"];
    REQUIRE_FALSE(headers.empty());
    CHECK(headers.find("$(CMAKE) -S") != std::string::npos);
    CHECK(headers.find("--build") == std::string::npos);
    CHECK(headers.find("test -f") == std::string::npos);
    const std::string& game = g.files["Game.Release.Android"];
    CHECK(game.find("vendor/headers/include") != std::string::npos);
    CHECK(g.master.find("Game.Release.Android: Headers.Release.Android") != std::string::npos);
}

// ============================================================================
// apk: features, variables, optional libraries, ABI path, bundle
// ============================================================================

TEST_CASE("android_features carry extra attributes", "[android][apk][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:GameApk]
type = apk
android_package = com.example.game
android_features = glEsVersion=0x00030000, android.hardware.vulkan.version version=0x00401000, android.hardware.touchscreen:false, android.hardware.vulkan.level level=1 required=false
)");
    const std::string manifest = g.files["GameApk.Release.AndroidManifest.xml"];
    CHECK(manifest.find("<uses-feature android:glEsVersion=\"0x00030000\" android:required=\"true\" />") != std::string::npos);
    CHECK(manifest.find("<uses-feature android:name=\"android.hardware.vulkan.version\" android:version=\"0x00401000\" android:required=\"true\" />") != std::string::npos);
    CHECK(manifest.find("<uses-feature android:name=\"android.hardware.touchscreen\" android:required=\"false\" />") != std::string::npos);
    CHECK(manifest.find("<uses-feature android:name=\"android.hardware.vulkan.level\" android:level=\"1\" android:required=\"false\" />") != std::string::npos);
}

TEST_CASE("${NAME:-default} fills a buildscript value from -D data with an explicit fallback", "[android][apk][variables]") {
    auto settings_for = [](bool define) {
        BuildscriptParser parser;
        if (define) parser.set_variables({{"ORIENTATION", "portrait"}, {"EXTRA_PERMISSION", "CAMERA"}});
        auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:GameApk]
type = apk
android_package = com.example.game
android_label = ${TITLE:-My Game}
android_activity_attrs = android:screenOrientation="${ORIENTATION:-landscape}"
android_permissions = INTERNET, ${EXTRA_PERMISSION:-VIBRATE}
)", "/work/proj");
        return sol.projects[0].android_apk;
    };
    auto defaults = settings_for(false);
    CHECK(defaults.label == "My Game");
    CHECK(defaults.activity_attrs == "android:screenOrientation=\"landscape\"");
    CHECK(defaults.permissions == std::vector<std::string>{"INTERNET", "VIBRATE"});
    auto filled = settings_for(true);
    CHECK(filled.label == "My Game");
    CHECK(filled.activity_attrs == "android:screenOrientation=\"portrait\"");
    CHECK(filled.permissions == std::vector<std::string>{"INTERNET", "CAMERA"});
}

TEST_CASE("An undefined variable without a default still expands to nothing", "[android][variables]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
[project:App]
type = exe
defines = A${NOPE}B, C${NOPE:-fallback}D
)", "/work/proj");
    const auto& defines = sol.projects[0].configurations.begin()->second.cl_compile.preprocessor_definitions;
    CHECK(has_define(defines, "AB"));
    CHECK(has_define(defines, "CfallbackD"));
}

TEST_CASE("android_native_libs accepts optional entries and $(Configuration) paths", "[android][apk][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:GameApk]
type = apk
android_package = com.example.game
android_native_libs = prebuilt/$(Configuration)/$(ANDROID_ABI)/libcore.so, prebuilt/$(Configuration)/$(ANDROID_ABI)/libextra.so [optional]
)");
    const std::string& release = g.files["GameApk.Release.Android"];
    const std::string& debug = g.files["GameApk.Debug.Android"];
    CHECK(release.find("prebuilt/Release/$(ANDROID_ABI)/libcore.so") != std::string::npos);
    CHECK(debug.find("prebuilt/Debug/$(ANDROID_ABI)/libcore.so") != std::string::npos);
    CHECK(release.find("APK_OPTIONAL_LIBS = ") != std::string::npos);
    CHECK(release.find("prebuilt/Release/$(ANDROID_ABI)/libextra.so") != std::string::npos);
    // The required library is checked; the optional one is skipped when missing.
    CHECK(release.find("optional library $(lib) not found, skipped") != std::string::npos);
    CHECK(release.find("$(foreach ANDROID_ABI,$(ANDROID_APK_ABIS),$(wildcard $(APK_OPTIONAL_LIBS)))") != std::string::npos);
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android
[project:A]
type = apk
android_package = a.b
android_native_libs = x/libreq.so, x/libopt.so [optional]
)", "/work/proj");
    CHECK(sol.projects[0].android_apk.native_libs.size() == 1);
    CHECK(sol.projects[0].android_apk.optional_native_libs.size() == 1);
}

TEST_CASE("A multi-ABI package has one well-defined path", "[android][apk][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:GameApk]
type = apk
android_package = com.example.game
target_name = Game
outdir = bin/android/$(ANDROID_ABI)
)");
    const std::string& mk = g.files["GameApk.Release.Android"];
    CHECK(mk.find("APK_MULTI_ABI := $(if $(word 2,$(ANDROID_APK_ABIS)),yes)") != std::string::npos);
    CHECK(mk.find("APK_OUT_DIR := $(if $(APK_MULTI_ABI),") != std::string::npos);
    CHECK(mk.find("bin/android/universal,") != std::string::npos);
    CHECK(mk.find("bin/android/$(ANDROID_ABI))") != std::string::npos);
    CHECK(mk.find("APK := $(APK_OUT_DIR)/Game.apk") != std::string::npos);
}

TEST_CASE("android_aab adds a bundle target built with aapt2 proto, bundletool and jarsigner", "[android][apk][aab][makefile_generator]") {
    AndroidOsScope scope;
    auto g = generate(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = Android

[project:GameApk]
type = apk
android_package = com.example.game
android_aab = true
target_name = Game
android_assets = content
)");
    const std::string& mk = g.files["GameApk.Release.Android"];
    CHECK(mk.find("ANDROID_AAB ?= 1") != std::string::npos);
    CHECK(mk.find("all: $(APK)$(if $(filter 1 yes true,$(ANDROID_AAB)), $(AAB))") != std::string::npos);
    CHECK(mk.find("AAB_NAME = Game.aab") != std::string::npos);
    CHECK(mk.find("$(AAB): $(APK)") != std::string::npos);
    CHECK(mk.find("--proto-format") != std::string::npos);
    CHECK(mk.find("/aab/base/manifest\"") != std::string::npos);
    CHECK(mk.find("build-bundle --modules=") != std::string::npos);
    CHECK(mk.find("BUNDLETOOL_PATH =") != std::string::npos);
    CHECK(mk.find("bundletool not found. Set BUNDLETOOL") != std::string::npos);
    CHECK(mk.find("$(JARSIGNER)") != std::string::npos);
    CHECK(mk.find("-storepass:env ANDROID_KEYSTORE_PASSWORD") != std::string::npos);
    // Without the setting the bundle is opt-in.
    auto g2 = generate(R"(
[solution]
name = Test
configurations = Release
platforms = Android
[project:GameApk]
type = apk
android_package = com.example.game
)");
    CHECK(g2.files["GameApk.Release.Android"].find("ANDROID_AAB ?= 0") != std::string::npos);
}

// ============================================================================
// Library paths, --build hint, C# defaults
// ============================================================================

TEST_CASE("libs in a configuration block are rebased to the buildscript like libdirs and outdir", "[android][libs]") {
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:App]
type = exe

[config:*|Android]
outdir = out/android
libdirs = deps/lib
libs = deps/lib/libfoo.a, log, -llog2, $(NDK)/libbar.a
)", "/work/proj");
    const auto& cfg = sol.projects[0].configurations.at("Release|Android");
    REQUIRE(cfg.link.additional_dependencies.size() == 4);
    const auto norm = [](std::string p) { std::replace(p.begin(), p.end(), '\\', '/'); return p; };
    CHECK(norm(cfg.link.additional_dependencies[0]).find("/work/proj/deps/lib/libfoo.a") != std::string::npos);
    CHECK(fs::path(cfg.link.additional_dependencies[0]).is_absolute());
    CHECK(cfg.link.additional_dependencies[1] == "log");
    CHECK(cfg.link.additional_dependencies[2] == "-llog2");
    CHECK(cfg.link.additional_dependencies[3] == "$(NDK)/libbar.a");
    CHECK(norm(cfg.link.additional_library_directories[0]).find("/work/proj/deps/lib") != std::string::npos);
}

TEST_CASE("--build explains where it looked for the cache", "[build_runner]") {
    TempDir cwd("sighmake_test_cache_hint_cwd");
    TempDir sub("sighmake_test_cache_hint_sub");
    // no cache anywhere: names the directory it searched
    std::string message = missing_cache_message(sub.path.string(), cwd.path);
    CHECK(message.find("No sighmake cache found in") != std::string::npos);
    CHECK(message.find("sighmake_test_cache_hint_sub") != std::string::npos);
    CHECK(message.find("--build .") == std::string::npos);
    // the cache lives in the current directory: suggests `--build .`
    std::ofstream(cwd.path / BuildCache::CACHE_FILENAME) << "generator=makefile\n";
    message = missing_cache_message(sub.path.string(), cwd.path);
    CHECK(message.find("try 'sighmake --build .'") != std::string::npos);
    // already looking at the current directory: no misleading hint
    message = missing_cache_message(cwd.path.string(), cwd.path);
    CHECK(message.find("--build .") == std::string::npos);
}

TEST_CASE("C# projects default Platform so dotnet build applies per-project defines", "[csproj]") {
    TempDir dir("sighmake_test_csproj_platform");
    BuildscriptParser parser;
    auto sol = parser.parse_string(R"(
[solution]
name = Test
configurations = Debug, Release
platforms = x64

[project:Managed]
type = dll
language = C#
target_framework = net10.0
sources = Lib.cs
defines = FERRITE_AOT
)", dir.path.string());
    std::ofstream(dir.path / "Lib.cs") << "class Lib {}";
    VcxprojGenerator generator;
    const fs::path csproj = dir.path / "Managed.csproj";
    REQUIRE(generator.generate_csproj(sol.projects[0], sol, csproj.string()));
    pugi::xml_document doc;
    REQUIRE(doc.load_file(csproj.string().c_str()));
    auto platform = doc.select_node("/Project/PropertyGroup/Platform").node();
    REQUIRE(platform);
    CHECK(std::string(platform.attribute("Condition").value()) == "'$(Platform)' == ''");
    CHECK(std::string(platform.child_value()) == "x64");
    // set before the SDK props are imported so the SDK does not default it to AnyCPU first
    auto first_group = doc.select_node("/Project/PropertyGroup[1]").node();
    CHECK(first_group.child("Platform"));
    CHECK(slurp(csproj).find("FERRITE_AOT") != std::string::npos);
}
