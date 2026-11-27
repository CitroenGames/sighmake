#include "pch.h"
#include "catch_amalgamated.hpp"
#include "parsers/buildscript_parser.hpp"
#include "generators/makefile_generator.hpp"
#include "generators/vcxproj_generator.hpp"
#include "common/android_evaluation.hpp"
#include "common/target_os.hpp"

using namespace vcxproj;
namespace fs = std::filesystem;

// Tests for the target-architecture features of the Makefile outputs: the `simd` mapping to GCC/Clang
// flags, if(x86_64)/if(arm64)/... blocks, and Android configurations evaluated as Android in a
// desktop-target generation.

namespace {

struct TargetOsScope {
    explicit TargetOsScope(TargetOS os) { set_target_os(os); }
    ~TargetOsScope() { set_target_os(std::nullopt); }
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

// The generator writes command-line stamps relative to the working directory: run it inside the
// temporary directory so nothing lands in the source tree.
struct ScopedCwd {
    fs::path previous;
    explicit ScopedCwd(const fs::path& dir) : previous(fs::current_path()) { fs::current_path(dir); }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

std::string slurp(const fs::path& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The lines of `text` between `ifeq ($(SIGHMAKE_TARGET_ARCH),<arch>)` and its `endif`.
std::string arch_branch(const std::string& text, const std::string& arch) {
    const std::string head = "ifeq ($(SIGHMAKE_TARGET_ARCH)," + arch + ")\n";
    const size_t start = text.find(head);
    if (start == std::string::npos) return "";
    const size_t body = start + head.size();
    const size_t end = text.find("endif\n", body);
    return text.substr(body, end == std::string::npos ? std::string::npos : end - body);
}

std::map<std::string, std::string> generate_makefiles(Solution& solution, const fs::path& dir) {
    ScopedCwd cwd(dir);
    MakefileGenerator generator;
    generator.generate(solution, dir.string());
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(dir / "build")) {
        if (entry.is_regular_file()) files[entry.path().filename().string()] = slurp(entry.path());
    }
    return files;
}

// A desktop-target generation as main() runs it: the host evaluation plus the Android evaluation.
std::map<std::string, std::string> generate_desktop_with_android(const std::string& script, const fs::path& dir,
                                                                 TargetOS host) {
    Solution solution;
    {
        TargetOsScope scope(host);
        BuildscriptParser parser;
        solution = parser.parse_string(script, dir.string());
    }
    if (solution_has_android_configurations(solution)) {
        TargetOsScope scope(TargetOS::Android);
        BuildscriptParser parser;
        merge_android_evaluation(solution, parser.parse_string(script, dir.string()));
    }
    TargetOsScope scope(host);
    return generate_makefiles(solution, dir);
}

const char* kAvx2 = "-mavx2 -mbmi -mbmi2 -mlzcnt -mf16c -mpopcnt";

}  // namespace

// ============================================================================
// simd -> GCC/Clang flags
// ============================================================================

TEST_CASE("simd values map to GCC/Clang flags for x86 targets only", "[target_arch][simd]") {
    CHECK(gnu_simd_flags("AdvancedVectorExtensions2", "x86_64") == kAvx2);
    CHECK(gnu_simd_flags("AdvancedVectorExtensions2", "x86") == kAvx2);
    CHECK(gnu_simd_flags("AdvancedVectorExtensions", "x86_64") == "-mavx -mpopcnt");
    CHECK(contains(gnu_simd_flags("AdvancedVectorExtensions512", "x86_64"), "-mavx512f"));
    CHECK(contains(gnu_simd_flags("AdvancedVectorExtensions512", "x86_64"), kAvx2));
    // SSE, SSE2 and IA32 select 32-bit x86 code generation; x86_64 always has SSE2.
    CHECK(gnu_simd_flags("StreamingSIMDExtensions2", "x86") == "-msse2 -mfpmath=sse");
    CHECK(gnu_simd_flags("StreamingSIMDExtensions2", "x86_64").empty());
    CHECK(gnu_simd_flags("NoExtensions", "x86_64").empty());
    CHECK(gnu_simd_flags("NotSet", "x86_64").empty());
    for (const char* value : {"StreamingSIMDExtensions", "StreamingSIMDExtensions2", "AdvancedVectorExtensions",
                              "AdvancedVectorExtensions2", "AdvancedVectorExtensions512", "NoExtensions"}) {
        // ARM targets never receive x86 flags, and FMA is never enabled (GCC would contract a*b+c).
        for (const char* arch : {"arm64", "arm", ""}) CHECK(gnu_simd_flags(value, arch).empty());
        for (const char* arch : {"x86_64", "x86"}) CHECK_FALSE(contains(gnu_simd_flags(value, arch), "fma"));
    }
    bool known = true;
    CHECK(gnu_simd_flags("AdvancedVectorExtensions10", "x86_64", &known).empty());
    CHECK_FALSE(known);
}

TEST_CASE("canonical_target_arch accepts the architecture names and aliases", "[target_arch]") {
    CHECK(canonical_target_arch("x86_64") == "x86_64");
    CHECK(canonical_target_arch("AMD64") == "x86_64");
    CHECK(canonical_target_arch("i686") == "x86");
    CHECK(canonical_target_arch("aarch64") == "arm64");
    CHECK(canonical_target_arch("ARM64") == "arm64");
    CHECK(canonical_target_arch("armv7") == "arm");
    CHECK(canonical_target_arch("x64").empty());  // stays the Windows platform filter
    CHECK(canonical_target_arch("linux").empty());
}

TEST_CASE("Linux makefile carries the simd flags under the x86 architecture branches", "[target_arch][simd][makefile_generator]") {
    TempDir dir("sighmake_test_arch_simd_linux");
    std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
    TargetOsScope scope(TargetOS::Linux);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Vec]
type = lib
sources = a.cpp
simd = AdvancedVectorExtensions2

[project:Plain]
type = lib
sources = a.cpp
)", dir.path.string());
    auto files = generate_makefiles(solution, dir.path);
    const std::string& vec = files["Vec.Release"];
    REQUIRE_FALSE(vec.empty());
    CHECK(contains(vec, "$(shell $(CXX) -dumpmachine 2>/dev/null)"));
    CHECK(arch_branch(vec, "x86_64") == std::string("  SIGHMAKE_ARCH_FLAGS += ") + kAvx2 + "\n");
    CHECK(arch_branch(vec, "x86") == std::string("  SIGHMAKE_ARCH_FLAGS += ") + kAvx2 + "\n");
    CHECK(arch_branch(vec, "arm64").empty());
    CHECK_FALSE(contains(vec, "-mfma"));
    // Only the architecture branches carry the flags; the base flag line references them.
    const size_t line = vec.find("SIGHMAKE_CXXFLAGS = ");
    REQUIRE(line != std::string::npos);
    const std::string cxx_line = vec.substr(line, vec.find('\n', line) - line);
    CHECK(contains(cxx_line, "$(SIGHMAKE_ARCH_FLAGS)"));
    CHECK_FALSE(contains(cxx_line, "-mavx2"));
    // A project without simd or architecture blocks is written as before.
    const std::string& plain = files["Plain.Release"];
    REQUIRE_FALSE(plain.empty());
    CHECK_FALSE(contains(plain, "SIGHMAKE_TARGET_ARCH"));
    CHECK_FALSE(contains(plain, "SIGHMAKE_ARCH_FLAGS"));
}

TEST_CASE("The command-line stamp records the architecture flags", "[target_arch][simd][makefile_generator]") {
    TempDir dir("sighmake_test_arch_stamp");
    std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
    TargetOsScope scope(TargetOS::Linux);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Vec]
type = lib
sources = a.cpp
simd = AdvancedVectorExtensions2
)", dir.path.string());
    generate_makefiles(solution, dir.path);
    std::string stamp;
    for (const auto& entry : fs::recursive_directory_iterator(dir.path)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("sighmake-cxx.", 0) == 0 && name.size() > 6 && name.substr(name.size() - 6) == ".flags")
            stamp = slurp(entry.path());
    }
    REQUIRE_FALSE(stamp.empty());
    CHECK(contains(stamp, std::string("[x86_64: ") + kAvx2 + "]"));
}

TEST_CASE("Android makefile takes the architecture from ANDROID_ABI", "[target_arch][simd][android][makefile_generator]") {
    TempDir dir("sighmake_test_arch_simd_android");
    std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
    TargetOsScope scope(TargetOS::Android);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Android

[project:Vec]
type = lib
sources = a.cpp
simd = AdvancedVectorExtensions2
)", dir.path.string());
    auto files = generate_makefiles(solution, dir.path);
    const std::string& mk = files["Vec.Release.Android"];
    REQUIRE_FALSE(mk.empty());
    CHECK(contains(mk, "SIGHMAKE_TARGET_ARCH := $(if $(filter arm64-v8a,$(ANDROID_ABI)),arm64,"));
    CHECK_FALSE(contains(mk, "-dumpmachine"));
    CHECK(contains(arch_branch(mk, "x86_64"), kAvx2));
    CHECK(arch_branch(mk, "arm64").empty());
    CHECK(arch_branch(mk, "arm").empty());
}

TEST_CASE("Makefile cells declared after simd inherit it", "[target_arch][simd][android][makefile_generator]") {
    TempDir dir("sighmake_test_arch_simd_late_cell");
    std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
    TargetOsScope scope(TargetOS::Android);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = x64

[project:Vec]
type = lib
sources = a.cpp
simd = AdvancedVectorExtensions2

[config:*|Android]
optimization = MaxSpeed
)", dir.path.string());
    auto files = generate_makefiles(solution, dir.path);
    const std::string& mk = files["Vec.Release.Android"];
    REQUIRE_FALSE(mk.empty());
    CHECK(contains(arch_branch(mk, "x86_64"), kAvx2));
}

// ============================================================================
// if(x86_64) / if(arm64) blocks
// ============================================================================

TEST_CASE("Architecture blocks become make conditionals in Linux and Android makefiles", "[target_arch][makefile_generator]") {
    const std::string script = R"(
[solution]
name = Test
configurations = Release
platforms = Linux, Android

[project:Kernels]
type = dll
sources = a.cpp
cflags = -DBASE
if(x86_64) {
    cflags = -mavx2
    defines = KERNELS_X86_64
    ldflags = -Wl,--x86-only
}
if(aarch64) {
    defines = KERNELS_ARM64
}
)";
    for (const TargetOS os : {TargetOS::Linux, TargetOS::Android}) {
        TempDir dir("sighmake_test_arch_blocks");
        std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
        TargetOsScope scope(os);
        BuildscriptParser parser;
        Solution solution = parser.parse_string(script, dir.path.string());
        auto files = generate_makefiles(solution, dir.path);
        for (const std::string name : {"Kernels.Release", "Kernels.Release.Android"}) {
            INFO(name);
            const std::string& mk = files[name];
            REQUIRE_FALSE(mk.empty());
            const std::string x86 = arch_branch(mk, "x86_64");
            CHECK(contains(x86, "SIGHMAKE_ARCH_FLAGS += -mavx2 -DKERNELS_X86_64"));
            CHECK(contains(x86, "SIGHMAKE_ARCH_LDFLAGS += -Wl,--x86-only"));
            CHECK(contains(arch_branch(mk, "arm64"), "-DKERNELS_ARM64"));
            CHECK_FALSE(contains(arch_branch(mk, "arm64"), "-mavx2"));
            CHECK(contains(mk, "-DBASE"));
            const size_t line = mk.find("SIGHMAKE_LDFLAGS = ");
            REQUIRE(line != std::string::npos);
            CHECK(contains(mk.substr(line, mk.find('\n', line) - line), "$(SIGHMAKE_ARCH_LDFLAGS)"));
        }
    }
}

TEST_CASE("Unsupported settings inside an architecture block are ignored", "[target_arch]") {
    TargetOsScope scope(TargetOS::Linux);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux

[project:Kernels]
type = lib
if(x86_64) {
    includes = include/x86
    cflags = -mavx2
}
)", ".");
    REQUIRE(solution.projects.size() == 1);
    const auto& cfg = solution.projects[0].configurations.at("Release|Linux");
    CHECK(cfg.cl_compile.additional_include_directories.empty());
    REQUIRE(cfg.arch_settings.count("x86_64") == 1);
    CHECK(cfg.arch_settings.at("x86_64").cflags == "-mavx2");
}

#ifdef _WIN32
TEST_CASE("Architecture blocks select the matching Visual Studio platform", "[target_arch][vcxproj_generator]") {
    TempDir dir("sighmake_test_arch_vcxproj");
    std::ofstream(dir.path / "a.cpp") << "int a() { return 1; }";
    TargetOsScope scope(TargetOS::Windows);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Win32, x64

[project:Kernels]
type = lib
sources = a.cpp
if(x86_64) {
    defines = KERNELS_X86_64
}
if(x86) {
    defines = KERNELS_X86
}
if(arm64) {
    defines = KERNELS_ARM64
}
)", dir.path.string());
    REQUIRE(solution.projects.size() == 1);
    const auto& project = solution.projects[0];
    // No ARM64 cell is created: the solution has no ARM64 platform.
    CHECK(project.configurations.count("Release|ARM64") == 0);
    auto has = [](const std::vector<std::string>& defs, const std::string& d) {
        return std::find(defs.begin(), defs.end(), d) != defs.end();
    };
    const auto& x64 = project.configurations.at("Release|x64").cl_compile.preprocessor_definitions;
    const auto& win32 = project.configurations.at("Release|Win32").cl_compile.preprocessor_definitions;
    CHECK(has(x64, "KERNELS_X86_64"));
    CHECK_FALSE(has(x64, "KERNELS_X86"));
    CHECK(has(win32, "KERNELS_X86"));
    CHECK_FALSE(has(win32, "KERNELS_X86_64"));
    CHECK(project.configurations.at("Release|x64").arch_settings.empty());
}
#endif

// ============================================================================
// Android configurations of a desktop-target generation
// ============================================================================

TEST_CASE("Android makefiles of a Linux generation do not receive if(linux) blocks", "[target_arch][android][makefile_generator]") {
    TempDir dir("sighmake_test_android_eval");
    for (const char* f : {"a.cpp", "linux_only.cpp", "droid.cpp"}) std::ofstream(dir.path / f) << "int x() { return 1; }";
    const std::string script = R"(
[solution]
name = Test
configurations = Release
platforms = x64, macOS

[project:Helper]
type = lib
sources = a.cpp

[project:Engine]
type = dll
sources = a.cpp
simd = AdvancedVectorExtensions2
cflags = -DBASE
if(posix) {
    cflags = -DPOSIX_FLAG
}
if(Linux) {
    cflags = -DLINUX_FLAG -mavx2
    defines = LINUX_DEFINE
    sources = linux_only.cpp
    target_link_libraries(Helper PUBLIC)
}
if(Android) {
    defines = ANDROID_DEFINE
    sources = droid.cpp
}

[config:*|Android]
optimization = MaxSpeed
)";
    auto files = generate_desktop_with_android(script, dir.path, TargetOS::Linux);
    const std::string& desktop = files["Engine.Release"];
    const std::string& android = files["Engine.Release.Android"];
    REQUIRE_FALSE(desktop.empty());
    REQUIRE_FALSE(android.empty());
    // Desktop: the Linux evaluation, unchanged.
    CHECK(contains(desktop, "-DLINUX_FLAG -mavx2"));
    CHECK(contains(desktop, "-DLINUX_DEFINE"));
    CHECK(contains(desktop, "linux_only.cpp"));
    CHECK_FALSE(contains(desktop, "ANDROID_DEFINE"));
    CHECK_FALSE(contains(desktop, "droid.cpp"));
    // Android: the Android evaluation. No Linux block, posix blocks still apply.
    CHECK_FALSE(contains(android, "LINUX_FLAG"));
    CHECK_FALSE(contains(android, "LINUX_DEFINE"));
    CHECK_FALSE(contains(android, "linux_only.cpp"));
    CHECK_FALSE(contains(android, "Helper"));
    CHECK(contains(android, "-DPOSIX_FLAG"));
    CHECK(contains(android, "-DANDROID_DEFINE"));
    CHECK(contains(android, "droid.cpp"));
    // -mavx2 reaches Android only through simd, inside the x86 ABI branches.
    const size_t line = android.find("SIGHMAKE_CXXFLAGS = ");
    REQUIRE(line != std::string::npos);
    CHECK_FALSE(contains(android.substr(line, android.find('\n', line) - line), "-mavx2"));
    CHECK(arch_branch(android, "arm64").empty());
    CHECK(contains(arch_branch(android, "x86_64"), kAvx2));
}

TEST_CASE("An Android evaluation keeps projects a desktop if() block filtered off Android", "[target_arch][android][makefile_generator]") {
    TempDir dir("sighmake_test_android_eval_filter");
    std::ofstream(dir.path / "a.cpp") << "int x() { return 1; }";
    const std::string script = R"(
[solution]
name = Test
configurations = Release
platforms = x64, macOS

[project:Engine]
type = lib
sources = a.cpp
if(Linux) {
    platforms = !Android
}

[config:*|Android]
optimization = MaxSpeed
)";
    auto files = generate_desktop_with_android(script, dir.path, TargetOS::Linux);
    CHECK_FALSE(files["Engine.Release"].empty());
    CHECK_FALSE(files["Engine.Release.Android"].empty());
}

TEST_CASE("merge_android_evaluation without an Android evaluation drops the Android configurations", "[target_arch][android]") {
    TargetOsScope scope(TargetOS::Linux);
    BuildscriptParser parser;
    Solution solution = parser.parse_string(R"(
[solution]
name = Test
configurations = Release
platforms = Linux, Android

[project:Engine]
type = lib
)", ".");
    REQUIRE(solution_has_android_configurations(solution));
    merge_android_evaluation(solution, Solution{});
    CHECK_FALSE(solution_has_android_configurations(solution));
    CHECK(solution.projects[0].configurations.count("Release|Linux") == 1);
}
