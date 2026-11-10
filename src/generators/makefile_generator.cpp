#include "pch.h"
#include "target_receipt_support.hpp"
#include "makefile_generator.hpp"
#include "common/target_os.hpp"
#include "vcxproj_generator.hpp"
#include "common/android_sdk.hpp"
#include "common/build_cache.hpp"
#include "common/string_utils.hpp"
#include "common/file_types.hpp"
#include "common/compiler_flags.hpp"
#include "common/language_standards.hpp"
#include "common/output_manifest.hpp"

namespace vcxproj {

namespace {

std::map<std::string, const Project*> build_project_lookup(const Solution& solution) {
    std::map<std::string, const Project*> lookup;
    for (const auto& project : solution.projects) {
        if (!project.is_package_project) {
            lookup[project.name] = &project;
        }
    }
    return lookup;
}

const Project* find_project(const std::map<std::string, const Project*>& lookup, const std::string& name) {
    auto it = lookup.find(name);
    return it != lookup.end() ? it->second : nullptr;
}

template <typename T>
const T* find_config_setting(const std::map<std::string, T>& settings, const std::string& config_key) {
    auto it = settings.find(config_key);
    if (it != settings.end()) {
        return &it->second;
    }

    it = settings.find(ALL_CONFIGS);
    if (it != settings.end()) {
        return &it->second;
    }

    // Older generator code used this literal; keep it as a compatibility fallback.
    it = settings.find("ALL_CONFIGS");
    if (it != settings.end()) {
        return &it->second;
    }

    return nullptr;
}

// Host tag of the prebuilt LLVM toolchain shipped with the Android NDK.
// Determined by the machine running sighmake (Apple hosts use the x86_64
// directory even on arm64 - the NDK ships universal binaries there).
constexpr const char* ANDROID_HOST_TAG =
#if defined(_WIN32)
    "windows-x86_64";
#elif defined(__APPLE__)
    "darwin-x86_64";
#else
    "linux-x86_64";
#endif

std::string make_project_config_target(const Project& project, const std::string& config_name,
                                       bool android = false) {
    return project.name + "." + config_name + (android ? ".Android" : "");
}

// How well a non-Windows platform matches the host the makefiles are generated on. Linux and macOS
// configs share one makefile name (App.Release), so when a project declares both, the host's own
// platform wins and the other host's platform is used only as a fallback.
int makefile_platform_rank(const std::string& platform) {
    const std::string p = to_lower(platform);
    const bool apple = p == "macos" || p == "darwin" || p == "osx";
#ifdef __APPLE__
    const bool native = apple;
    const bool foreign = is_linux_platform(p);
#else
    const bool native = is_linux_platform(p);
    const bool foreign = apple;
#endif
    return native ? 2 : (foreign ? 0 : 1);
}

// Find a config key the makefile generator can build for this config name.
// android=true selects Android configs; android=false selects the other
// non-Windows configs (Linux/macOS). They get separate makefiles so a solution
// can target both without the outputs colliding.
std::optional<std::string> find_makefile_config_key(const Project& project, const std::string& config_name,
                                                    bool android) {
    std::optional<std::string> best;
    int best_rank = -1;
    for (const auto& [config_key, config] : project.configurations) {
        (void)config;
        size_t pipe_pos = config_key.find('|');
        std::string cfg = pipe_pos != std::string::npos
            ? config_key.substr(0, pipe_pos)
            : config_key;
        std::string platform = pipe_pos != std::string::npos
            ? config_key.substr(pipe_pos + 1)
            : "";
        if (cfg == config_name && !is_windows_platform(platform) &&
            is_android_platform(platform) == android) {
            const int rank = makefile_platform_rank(platform);
            if (rank > best_rank) {
                best = config_key;
                best_rank = rank;
            }
        }
    }
    return best;
}

// Android loaders (System.loadLibrary, APK packaging) require the "lib" prefix
// on shared libraries.
std::string decorate_target_name(const std::string& name, const std::string& config_type, bool android) {
    if (android && config_type == "DynamicLibrary" && name.rfind("lib", 0) != 0) {
        return "lib" + name;
    }
    return name;
}

// Whether a configuration strips its linked executables and shared libraries (`strip`, default
// true for "Release" only). A stripping configuration also compiles without -g: the debug
// information would never reach a binary and only costs compile time, memory and disk.
// `strip = false` keeps the symbols and compiles with -g again.
bool makefile_strips_outputs(const Configuration& config, const std::string& config_name) {
    return config.link.strip.value_or(config_name == "Release");
}

// Default output/intermediate directories. Android builds get an ABI-specific
// subdirectory ($(ANDROID_ABI) expands when make runs) so different ABIs don't
// overwrite each other's artifacts.
std::string default_makefile_out_dir(const std::string& config_name, bool android) {
    return android ? "build/" + config_name + "/android/$(ANDROID_ABI)"
                   : "build/" + config_name;
}

std::string default_makefile_int_dir(const std::string& config_name, const std::string& project_name,
                                     bool android) {
    return default_makefile_out_dir(config_name, android) + "/obj/" + project_name;
}

// Output directories that contain Make variables (for example bin/android/$(ANDROID_ABI)) are
// kept verbatim by the parser, which leaves a relative spelling anchored to the working directory
// of the sighmake run. Anchor it to the buildscript instead so the result does not depend on
// where sighmake was started. Paths that begin with a variable are left untouched.
std::string anchor_variable_dir(const std::string& dir, const Project& project,
                                const std::string& config_key) {
    namespace fs = std::filesystem;
    if (dir.empty() || dir.find("$(") == std::string::npos) return dir;

    // MSBuild-style tokens are expanded here so one outdir line can serve every configuration
    // and platform (outdir = bin/$(Platform)/$(Configuration)). Make variables such as
    // $(ANDROID_ABI) are left for make to expand.
    std::string expanded = dir;
    const auto [config_name, platform_name] = parse_config_key(config_key);
    auto replace_all = [&](const std::string& token, const std::string& value) {
        size_t pos = 0;
        while ((pos = expanded.find(token, pos)) != std::string::npos) {
            expanded.replace(pos, token.size(), value);
            pos += value.size();
        }
    };
    replace_all("$(Configuration)", config_name);
    replace_all("$(Platform)", platform_name);
    replace_all("$(PlatformName)", platform_name);
    replace_all("$(ProjectName)", project.name);

    if (expanded.rfind("$(", 0) == 0 || project.buildscript_path.empty()) return expanded;
    fs::path p(expanded);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return expanded;
    try {
        return fs::absolute(fs::path(project.buildscript_path) / p).lexically_normal().generic_string();
    } catch (...) {
        return expanded;
    }
}

// Writes the Android ABI/API variables, NDK discovery and toolchain directory shared by every
// Android makefile (compiled projects and external CMake builds).
static void write_android_setup(std::ostream& out) {
    out << "# Android NDK toolchain (requires NDK r19 or newer)\n";
    out << "# Override at build time, e.g.: make ANDROID_ABI=x86_64 ANDROID_API=26\n";
    out << "ANDROID_ABI ?= arm64-v8a\n";
    out << "ANDROID_API ?= 24\n";
    out << "# NDK lookup: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_NDK, the newest\n";
    out << "# $(ANDROID_HOME)/ndk/<version> (or ANDROID_SDK_ROOT), then the NDK found at generation time.\n";
    out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
    out << "  ANDROID_NDK_HOME := $(ANDROID_NDK_ROOT)\n";
    out << "endif\n";
    out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
    out << "  ANDROID_NDK_HOME := $(ANDROID_NDK)\n";
    out << "endif\n";
    out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
    out << "  ANDROID_NDK_HOME := $(lastword $(sort $(wildcard $(ANDROID_HOME)/ndk/*) $(wildcard $(ANDROID_SDK_ROOT)/ndk/*)))\n";
    out << "endif\n";
    {
        // Last resort: the NDK sighmake found while generating (Android Studio's default
        // install location is only known here, not in the environment of `make`).
        std::string generated_ndk;
        if (auto ndk = android_sdk::find_ndk()) generated_ndk = ndk->generic_string();
        if (!generated_ndk.empty()) {
            out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
            out << "  ANDROID_NDK_HOME := $(wildcard " << generated_ndk << ")\n";
            out << "endif\n";
        }
    }
    out << "ifeq ($(strip $(ANDROID_NDK_HOME)),)\n";
    out << "  ifneq ($(MAKECMDGOALS),clean)\n";
    out << "    $(error ANDROID_NDK_HOME is not set and no NDK was found. Set ANDROID_NDK_HOME or ANDROID_HOME)\n";
    out << "  endif\n";
    out << "endif\n";
    out << "ifeq ($(ANDROID_ABI),arm64-v8a)\n";
    out << "  ANDROID_TRIPLE := aarch64-linux-android\n";
    out << "else ifeq ($(ANDROID_ABI),armeabi-v7a)\n";
    out << "  ANDROID_TRIPLE := armv7a-linux-androideabi\n";
    out << "else ifeq ($(ANDROID_ABI),x86_64)\n";
    out << "  ANDROID_TRIPLE := x86_64-linux-android\n";
    out << "else ifeq ($(ANDROID_ABI),x86)\n";
    out << "  ANDROID_TRIPLE := i686-linux-android\n";
    out << "else\n";
    out << "  $(error Unsupported ANDROID_ABI '$(ANDROID_ABI)'. Supported: arm64-v8a, armeabi-v7a, x86_64, x86)\n";
    out << "endif\n";
    out << "ANDROID_TOOLCHAIN := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/"
        << ANDROID_HOST_TAG << "/bin\n";
}

std::string stable_hash8(const std::string& value) {
    unsigned int hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }

    std::stringstream ss;
    ss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << hash;
    return ss.str();
}

std::string sanitize_object_stem(const std::string& source_identity) {
    std::string normalized = source_identity;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const size_t slash = normalized.find_last_of('/');
    const size_t dot = normalized.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        normalized.erase(dot);
    }

    std::vector<std::string> components;
    std::stringstream parts(normalized);
    std::string component;
    while (std::getline(parts, component, '/')) {
        if (component.empty() || component == ".") {
            continue;
        }

        if (component == "..") {
            components.push_back("__");
            continue;
        }

        std::string sanitized;
        for (unsigned char c : component) {
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
                sanitized.push_back(static_cast<char>(c));
            } else {
                sanitized.push_back('_');
            }
        }

        if (sanitized.empty() || sanitized == "." || sanitized == "..") {
            sanitized = "_";
        }
        components.push_back(sanitized);
    }

    if (components.empty()) {
        components.push_back("source");
    }

    components.back() += "_" + stable_hash8(normalized);

    std::string result;
    for (const auto& part : components) {
        if (!result.empty()) result += "/";
        result += part;
    }
    return result + ".o";
}

// A path written as a make target or prerequisite: spaces are escaped so make keeps it one word.
// Recipes refer to such files through quoted automatic variables ("$<").
std::string make_escape_spaces(const std::string& path) {
    std::string result;
    for (char c : path) {
        if (c == ' ') result += '\\';
        result += c;
    }
    return result;
}

bool has_extension_case_insensitive(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) {
        return false;
    }

    std::string suffix = to_lower(path.substr(path.size() - ext.size()));
    return suffix == ext;
}

static bool write_target_receipt_support(
    const Project& project, const std::string& makefile_path) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::absolute(makefile_path).parent_path();

    const fs::path py_script_path = directory / "write-sighmake-target-receipt.py";
    {
        std::ofstream script(py_script_path, std::ios::binary | std::ios::trunc);
        if (!script) return false;
        record_generated_output(py_script_path);
        script << R"PY(import argparse
import hashlib
import json
import os
import stat
import sys

def canonical_architecture(value):
    normalized = value.strip().lower()
    aliases = {
        "amd64": "x64",
        "x86_64": "x64",
        "i386": "x86",
        "i486": "x86",
        "i586": "x86",
        "i686": "x86",
        "aarch64": "ARM64",
        "arm64": "ARM64",
        "armv7": "ARM",
        "armv7l": "ARM",
    }
    return aliases.get(normalized, value)

def file_record(path):
    if not os.path.isfile(path):
        sys.exit(f"target receipt input is missing: {path}")
    size = os.path.getsize(path)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    mode = stat.S_IMODE(os.stat(path).st_mode)
    return size, h.hexdigest().lower(), mode

def main():
    parser = argparse.ArgumentParser(description="Write sighmake target receipt")
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--primary-artifact", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    primary_size, primary_sha256, primary_mode = file_record(args.primary_artifact)

    runtime = []
    skipped = []
    if os.path.isfile(args.metadata):
        with open(args.metadata, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split("|")
                if len(parts) != 4:
                    sys.exit(f"invalid target receipt metadata: {line}")
                name, source, stage_path, req_str = parts
                required = (req_str == "1")
                if not os.path.isfile(source):
                    if required:
                        sys.exit(f"required runtime dependency is missing: {source}")
                    skipped.append({
                        "Name": name,
                        "Source": source,
                        "StagePath": stage_path,
                        "Reason": "MissingOptional"
                    })
                    continue
                size, sha256, unix_mode = file_record(source)
                runtime.append({
                    "Name": name,
                    "Source": source,
                    "StagePath": stage_path,
                    "Required": required,
                    "Size": size,
                    "SHA256": sha256,
                    "UnixMode": unix_mode
                })

    runtime.sort(key=lambda x: (x["StagePath"].lower(), x["Name"].lower(), x["Source"].lower()))
    skipped.sort(key=lambda x: (x["StagePath"].lower(), x["Name"].lower(), x["Source"].lower()))

    receipt = {
        "FormatVersion": 2,
        "Target": args.target,
        "Platform": args.platform,
        "Architecture": canonical_architecture(args.architecture),
        "Configuration": args.configuration,
        "PrimaryArtifact": os.path.basename(args.primary_artifact),
        "PrimaryArtifactSize": primary_size,
        "PrimaryArtifactSHA256": primary_sha256,
        "PrimaryArtifactUnixMode": primary_mode,
        "RuntimeDependencies": runtime,
        "SkippedRuntimeDependencies": skipped
    }

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    candidate = f"{args.output}.candidate-{os.getpid()}"
    try:
        with open(candidate, "w", encoding="utf-8") as f:
            json.dump(receipt, f, indent=2)
            f.write("\n")
        with open(candidate, "r", encoding="utf-8") as f:
            json.load(f)
        os.replace(candidate, args.output)
    finally:
        if os.path.exists(candidate):
            try:
                os.remove(candidate)
            except OSError:
                pass

if __name__ == "__main__":
    main()
)PY";
        if (!script) return false;
    }

    const fs::path ps_script_path = directory / "Write-SighmakeTargetReceipt.ps1";
    {
        std::ofstream script(ps_script_path, std::ios::binary | std::ios::trunc);
        if (!script) return false;
        record_generated_output(ps_script_path);
        script << R"PS([CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Metadata,
    [Parameter(Mandatory=$true)][string]$Target,
    [Parameter(Mandatory=$true)][string]$Platform,
    [Parameter(Mandatory=$true)][string]$Architecture,
    [Parameter(Mandatory=$true)][string]$Configuration,
    [Parameter(Mandatory=$true)][string]$PrimaryArtifact,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

)PS" << sighmake::target_receipt_file_record_powershell << R"PS(

$primary = New-FileRecord $PrimaryArtifact
$runtime = @()
$skipped = @()
if (Test-Path -LiteralPath $Metadata -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $Metadata) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line.Split('|')
        if ($parts.Count -ne 4) { throw "invalid target receipt metadata: $line" }
        $required = $parts[3] -eq '1'
        if (-not (Test-Path -LiteralPath $parts[1] -PathType Leaf)) {
            if ($required) { throw "required runtime dependency is missing: $($parts[1])" }
            $skipped += [ordered]@{ Name=$parts[0]; Source=$parts[1]; StagePath=$parts[2]; Reason='MissingOptional' }
            continue
        }
        $record = New-FileRecord $parts[1]
        $runtime += [ordered]@{
            Name = $parts[0]
            Source = $parts[1]
            StagePath = $parts[2]
            Required = $required
            Size = $record.Size
            SHA256 = $record.SHA256
        }
    }
}

# Ordinal ordering on lower-cased StagePath/Name/Source; the package validator compares ordinally,
# while Sort-Object is culture-sensitive and ignores '-' and '_'.
function Sort-ReceiptRecords($Records) {
    $list = [Collections.Generic.List[object]]::new()
    foreach ($r in $Records) { $list.Add($r) }
    $list.Sort([Comparison[object]]{
        param($a, $b)
        foreach ($k in 'StagePath', 'Name', 'Source') {
            $c = [string]::CompareOrdinal(([string]$a[$k]).ToLowerInvariant(), ([string]$b[$k]).ToLowerInvariant())
            if ($c -ne 0) { return $c }
        }
        return 0
    })
    return $list.ToArray()
}

$receipt = [ordered]@{
    FormatVersion = 1
    Target = $Target
    Platform = $Platform
    Architecture = $Architecture
    Configuration = $Configuration
    PrimaryArtifact = [IO.Path]::GetFileName($PrimaryArtifact)
    PrimaryArtifactSize = $primary.Size
    PrimaryArtifactSHA256 = $primary.SHA256
    RuntimeDependencies = @(Sort-ReceiptRecords $runtime)
    SkippedRuntimeDependencies = @(Sort-ReceiptRecords $skipped)
}

$parent = Split-Path -Parent $Output
if ($parent) { $null = New-Item -ItemType Directory -Force -Path $parent }
$candidate = "$Output.candidate-$PID-$([DateTime]::UtcNow.Ticks)"
try {
    $json = $receipt | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($candidate, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    $null = Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json
    Move-Item -LiteralPath $candidate -Destination $Output -Force
} finally {
    if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Force }
}
)PS";
        if (!script) return false;
    }

    std::vector<RuntimeDependency> dependencies = project.runtime_dependencies;
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
        return std::tie(left.stage_path, left.name, left.source) <
            std::tie(right.stage_path, right.name, right.source);
    });
    const fs::path metadata_path = directory / (project.name + ".runtime-dependencies.txt");
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) return false;
    record_generated_output(metadata_path);
    for (const RuntimeDependency& dependency : dependencies) {
        if (dependency.name.find('|') != std::string::npos ||
            dependency.source.find('|') != std::string::npos ||
            dependency.stage_path.find('|') != std::string::npos ||
            dependency.name.find_first_of("\r\n") != std::string::npos ||
            dependency.source.find_first_of("\r\n") != std::string::npos ||
            dependency.stage_path.find_first_of("\r\n") != std::string::npos) {
            std::cerr << "Error: runtime dependency metadata contains a reserved delimiter\n";
            return false;
        }
        metadata << dependency.name << '|' << dependency.source << '|' <<
            dependency.stage_path << '|' << (dependency.required ? '1' : '0') << '\n';
    }
    return static_cast<bool>(metadata);
}

} // namespace

// Convert Windows path to Unix path (backslash to forward slash)
std::string MakefileGenerator::to_unix_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// Helper to compute relative path from makefile directory
std::string MakefileGenerator::compute_relative_path(const std::string& path, const std::filesystem::path& makefile_dir) {
    namespace fs = std::filesystem;

    if (path.empty()) return ".";

    try {
        // These paths often describe outputs that do not exist yet.  Use a
        // purely lexical calculation: fs::relative() consults the filesystem
        // through weakly_canonical() and can return an empty path for missing
        // directories (including Android's literal $(ANDROID_ABI) segment).
        fs::path abs_target = fs::absolute(path).lexically_normal();
        fs::path abs_base = fs::absolute(makefile_dir).lexically_normal();

        // Reconcile equivalent symlink spellings before the lexical comparison.
        // macOS commonly reports its temporary directory as /var/... while
        // canonical source paths use /private/var/.... Keep paths containing
        // Make variables purely lexical because those paths intentionally do
        // not exist yet.
        if (path.find("$(") == std::string::npos &&
            makefile_dir.string().find("$(") == std::string::npos) {
            std::error_code target_ec;
            std::error_code base_ec;
            fs::path canonical_target = fs::weakly_canonical(abs_target, target_ec);
            fs::path canonical_base = fs::weakly_canonical(abs_base, base_ec);
            if (!target_ec && !base_ec &&
                !canonical_target.empty() && !canonical_base.empty()) {
                abs_target = std::move(canonical_target);
                abs_base = std::move(canonical_base);
            }
        }

        fs::path rel_path = abs_target.lexically_relative(abs_base);
        if (!rel_path.empty()) {
            return to_unix_path(rel_path.string());
        }

        // Different roots (for example C: and D:) cannot be expressed
        // relatively.  Keep the normalized absolute path in that case.
        return to_unix_path(abs_target.string());
    } catch (...) {
        // Preserve the caller's path if absolute-path construction itself
        // fails, while still emitting Makefile-friendly separators.
        return to_unix_path(path);
    }
}

std::string MakefileGenerator::make_object_path(const SourceFile& src, const std::string& src_relative,
                                                const std::string& config_key, const std::string& int_dir,
                                                const std::filesystem::path& makefile_dir,
                                                const std::string& buildscript_dir) {
    if (const auto* object_file = find_config_setting(src.settings.object_file, config_key);
        object_file && !object_file->empty()) {
        std::string obj_path = to_unix_path(*object_file);
        // $(IntDir) already expands to a path relative to the makefile directory.
        const bool relative_to_makefile = obj_path.find("$(IntDir)") != std::string::npos;
        // MSBuild-style macros (object_file = $(IntDir)name.obj) name the configuration's
        // intermediate directory; make has no such variables.
        const auto [config_name, platform_name] = parse_config_key(config_key);
        for (const auto& [token, value] : {std::pair<std::string, std::string>{"$(IntDir)", int_dir},
                                           {"$(Configuration)", config_name},
                                           {"$(Platform)", platform_name}}) {
            for (size_t pos = 0; (pos = obj_path.find(token, pos)) != std::string::npos; pos += value.size()) {
                obj_path.replace(pos, token.size(), value);
            }
        }
        if (has_extension_case_insensitive(obj_path, ".obj")) {
            obj_path.replace(obj_path.size() - 4, 4, ".o");
        }

        std::filesystem::path path(obj_path);
        if (path.is_absolute()) {
            return compute_relative_path(obj_path, makefile_dir);
        }

        // Other relative paths are relative to the buildscript, like every other path setting.
        if (!relative_to_makefile && !buildscript_dir.empty() && obj_path.rfind("$(", 0) != 0) {
            return compute_relative_path((std::filesystem::path(buildscript_dir) / obj_path).string(), makefile_dir);
        }

        return obj_path;
    }

    return int_dir + sanitize_object_stem(src_relative);
}

// Strip .lib or .dll extension from library names
std::string MakefileGenerator::strip_lib_extension(const std::string& lib) {
    std::string result = lib;

    // Remove .lib extension if present
    if (result.size() > 4 && result.substr(result.size() - 4) == ".lib") {
        result = result.substr(0, result.size() - 4);
    }

    // Remove .dll extension if present
    if (result.size() > 4 && result.substr(result.size() - 4) == ".dll") {
        result = result.substr(0, result.size() - 4);
    }

    // Remove .a extension if present
    if (result.size() > 2 && result.substr(result.size() - 2) == ".a") {
        result = result.substr(0, result.size() - 2);
    }

    // Remove shared-library extensions (libz.so -> -lz)
    if (result.size() > 3 && result.substr(result.size() - 3) == ".so") {
        result = result.substr(0, result.size() - 3);
    }
    if (result.size() > 6 && result.substr(result.size() - 6) == ".dylib") {
        result = result.substr(0, result.size() - 6);
    }

    // Remove lib prefix if present (e.g., libpthread -> pthread)
    if (result.size() > 3 && result.substr(0, 3) == "lib") {
        result = result.substr(3);
    }

    return result;
}

const std::set<std::string>& MakefileGenerator::pic_static_libraries(const Solution& solution,
                                                                    const std::string& config_key) {
    if (pic_cache_solution_ != &solution) {
        pic_cache_.clear();
        pic_cache_solution_ = &solution;
    }
    auto it = pic_cache_.find(config_key);
    if (it == pic_cache_.end()) {
        it = pic_cache_.emplace(config_key, static_libraries_linked_into_shared_libraries(solution, config_key)).first;
    }
    return it->second;
}

// pch_header is a buildscript setting, so a relative name is looked up next to the buildscript and
// then in the include directories (sources usually name it the way they include it). A header found
// in neither place is returned unchanged.
static std::string resolve_pch_header(const std::string& header, const Project& project,
                                      const Configuration& config) {
    namespace fs = std::filesystem;
    if (header.empty() || fs::path(header).is_absolute() || project.buildscript_path.empty()) return header;
    std::vector<fs::path> candidates{fs::path(project.buildscript_path) / header};
    for (const auto& inc : config.cl_compile.additional_include_directories) {
        for (const auto& part : split_semicolons(inc)) {
            fs::path dir(part);
            if (dir.is_relative()) dir = fs::path(project.buildscript_path) / dir;
            candidates.push_back(dir / header);
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.lexically_normal().generic_string();
    }
    return header;
}

// Determine if PCH is enabled for a configuration and return the PCH header path
std::pair<bool, std::string> MakefileGenerator::get_pch_info(const Configuration& config) {
    bool has_pch = !config.cl_compile.pch.mode.empty() &&
                   config.cl_compile.pch.mode != "NotUsing";
    std::string header = config.cl_compile.pch.header;
    return {has_pch, header};
}

// Check if a source file uses, creates, or doesn't use PCH
// Returns: (mode, header) tuple
std::tuple<std::string, std::string> MakefileGenerator::get_file_pch_mode(
    const SourceFile& src,
    const std::string& config_key,
    const Configuration& config) {

    if (const auto* pch = find_config_setting(src.settings.pch, config_key);
        pch && !pch->mode.empty()) {
        return {pch->mode, pch->header};
    }

    // Fall back to project-level PCH settings
    return {config.cl_compile.pch.mode, config.cl_compile.pch.header};
}

// Get all compiler flags for a configuration
std::string MakefileGenerator::get_compiler_flags(const Configuration& config, const Project& project,
                                                   const std::filesystem::path& makefile_dir, bool c_flags,
                                                   const std::string& config_name,
                                                   const std::string& pic_flags) {
    std::stringstream ss;

    // Language standard. Mixed C/C++ projects need distinct flags for each
    // compiler. Empty values fall back to the shared defaults (C17 / C++17).
    if (c_flags) {
        ss << lang::c_standard_to_gnu_flag(project.c_standard) << " ";
    } else {
        ss << lang::cpp_standard_to_gnu_flag(config.cl_compile.language_standard) << " ";
    }

    // Optimization
    if (!config.cl_compile.optimization.empty()) {
        ss << flags::optimization_to_gnu_flag(config.cl_compile.optimization) << " ";
    }

    // Debug information (not for configurations whose outputs are stripped)
    if (!config.cl_compile.debug_information_format.empty() && !makefile_strips_outputs(config, config_name)) {
        ss << "-g ";
    }

    // Warning level
    if (!config.cl_compile.warning_level.empty()) {
        ss << flags::warning_level_to_gnu_flags(config.cl_compile.warning_level) << " ";
    }

    // Position-independent code: shared libraries, the static libraries linked into them, and
    // whatever `position_independent` forces. Decided by the caller from the dependency graph;
    // static libraries linked only into executables stay non-PIC (cheaper code, no GOT loads).
    ss << pic_flags;

    // Include directories - convert to relative paths
    for (const auto& inc : config.cl_compile.additional_include_directories) {
        for (const auto& part : split_semicolons(inc)) {
            std::string inc_path = compute_relative_path(part, makefile_dir);
            ss << "-I\"" << inc_path << "\" ";
        }
    }

    // Preprocessor definitions
    // Release gains NDEBUG and Debug gains _DEBUG unless the project sets either itself.
    for (const auto& def : flags::with_default_build_defines(config_name, config.cl_compile.preprocessor_definitions)) {
        ss << "-D" << def << " ";
    }

    // UTF-8 source encoding (ensure source files are read as UTF-8)
    if (config.cl_compile.utf8_source.value_or(false)) {
        ss << "-finput-charset=UTF-8 -fexec-charset=UTF-8 ";
    }

    // Additional options (raw flags)
    if (!config.cl_compile.additional_options.empty()) {
        ss << config.cl_compile.additional_options << " ";
    }

    // Function-level linking (allows linker to remove unused functions)
    if (config.cl_compile.function_level_linking.value_or(false)) {
        ss << "-ffunction-sections ";
    }

    // Data sections (allows linker to remove unused data)
    if (config.link.enable_comdat_folding.value_or(false) || config.link.optimize_references.value_or(false)) {
        ss << "-fdata-sections ";
    }

    return ss.str();
}

// Get linker flags (library directories)
std::string MakefileGenerator::get_linker_flags(const Configuration& config, const std::filesystem::path& makefile_dir,
                                                bool android) {
    std::stringstream ss;

    // Android always links ELF binaries with lld, even when the makefile is
    // generated on a macOS host, so Mach-O style flags only apply to
    // non-Android builds generated on Apple hosts.
#ifdef __APPLE__
    const bool macho_linker = !android;
#else
    const bool macho_linker = false;
    (void)android;
#endif

    // Library directories - convert to relative paths
    for (const auto& libdir : config.link.additional_library_directories) {
        for (const auto& part : split_semicolons(libdir)) {
            std::string lib_path = compute_relative_path(part, makefile_dir);
            ss << "-L\"" << lib_path << "\" ";
        }
    }

    // Additional linker options
    if (!config.link.additional_options.empty()) {
        ss << config.link.additional_options << " ";
    }

    // Garbage collection of unused sections (equivalent to MSVC optimize_references + enable_comdat_folding)
    if (config.link.optimize_references.value_or(false) || config.link.enable_comdat_folding.value_or(false)) {
        ss << (macho_linker ? "-Wl,-dead_strip " : "-Wl,--gc-sections ");
    }

    // Base address
    if (!config.link.base_address.empty()) {
        if (!macho_linker) {
            ss << "-Wl,--image-base=" << config.link.base_address << " ";
        }
    }

    // Module definition file (.def)
    if (!config.link.module_definition_file.empty()) {
        std::string def_path = compute_relative_path(config.link.module_definition_file, makefile_dir);
        if (macho_linker) {
            ss << "-Wl,-exported_symbols_list," << def_path << " ";
        } else {
            ss << "-Wl,--version-script=" << def_path << " ";
        }
    }

    // Ignore all default libraries
    if (config.link.ignore_all_default_libraries) {
        ss << "-nodefaultlibs ";
    }

    return ss.str();
}

// Get linker libraries
std::string MakefileGenerator::get_linker_libs(const Configuration& config, const std::filesystem::path& makefile_dir) {
    std::stringstream ss;

    // Additional dependencies (libraries)
    for (const auto& lib : config.link.additional_dependencies) {
        for (const auto& part : split_semicolons(lib)) {
            // Skip empty library names
            if (part.empty()) continue;

            // Pass through flags that start with - (e.g., -framework Metal)
            if (part[0] == '-') {
                ss << part << " ";
                continue;
            }

            std::string libname = strip_lib_extension(part);
            if (libname.empty()) continue;

            // Check if it's a full path to a .lib or .a file
            if (part.find('/') != std::string::npos || part.find('\\') != std::string::npos) {
                // A path: the parser rebases buildscript-relative entries to absolute paths, which are
                // written relative to the makefile like library directories are.
                const bool absolute = std::filesystem::path(part).is_absolute() && part.find("$(") == std::string::npos;
                ss << to_unix_path(absolute && !makefile_dir.empty() ? compute_relative_path(part, makefile_dir) : part) << " ";
            } else {
                // It's a library name, use -l flag
                ss << "-l" << libname << " ";
            }
        }
    }

    return ss.str();
}

// Libraries produced by an external CMake build that dependants link against (static and shared
// libraries; other produced files are only copied to the output directory).
static std::vector<std::string> external_cmake_link_outputs(const Project& project) {
    std::vector<std::string> result;
    for (const auto& output : project.external_cmake.outputs) {
        const std::string name = std::filesystem::path(output).filename().string();
        const std::string ext = to_lower(std::filesystem::path(name).extension().string());
        if (ext == ".a" || ext == ".so" || ext == ".dylib" || ext == ".lib") result.push_back(name);
    }
    return result;
}

static bool looks_like_commit_id(const std::string& ref) {
    if (ref.size() < 7 || ref.size() > 40) return false;
    return std::all_of(ref.begin(), ref.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// A project of type external_cmake: one makefile per configuration that (optionally) clones the
// pinned git source, runs `cmake` configure + build into a per-configuration (and, for Android,
// per-ABI) build directory, and copies the declared outputs into the project's outdir where
// dependants pick them up as link inputs.
bool MakefileGenerator::generate_external_cmake_makefile(const Project& project, const Configuration& config,
                                                         const std::string& config_key,
                                                         const std::string& output_path) {
    namespace fs = std::filesystem;
    const auto& ext = project.external_cmake;
    const std::string config_name = parse_config_key(config_key).first;
    const std::string platform = parse_config_key(config_key).second;
    const bool android = is_android_platform(platform);

    if (ext.empty()) {
        std::cerr << "Error: external_cmake project '" << project.name
                  << "' needs cmake_source (a local path) or cmake_git (a repository URL).\n";
        return false;
    }
    if (!ext.source_dir.empty() && !ext.git_url.empty()) {
        std::cerr << "Error: external_cmake project '" << project.name
                  << "' sets both cmake_source and cmake_git; choose one.\n";
        return false;
    }
    if (ext.git_url.empty() == false && ext.git_ref.empty()) {
        std::cerr << "Warning: external_cmake project '" << project.name
                  << "' has no cmake_git_tag; the repository's default branch is not a pinned version.\n";
    }

    fs::path makefile_dir = fs::path(output_path).parent_path();
    if (makefile_dir.empty()) makefile_dir = ".";

    std::string out_dir = compute_relative_path(
        config.out_dir.empty() ? default_makefile_out_dir(config_name, android)
                               : anchor_variable_dir(config.out_dir, project, config_key), makefile_dir);
    std::string int_dir = compute_relative_path(
        config.int_dir.empty() ? default_makefile_int_dir(config_name, project.name, android)
                               : anchor_variable_dir(config.int_dir, project, config_key), makefile_dir);
    while (out_dir.size() > 1 && out_dir.back() == '/') out_dir.pop_back();
    while (int_dir.size() > 1 && int_dir.back() == '/') int_dir.pop_back();

    const bool use_git = !ext.git_url.empty();
    std::string source_dir;
    if (use_git) {
        source_dir = ext.checkout_dir.empty() ? "_external/" + project.name + "/source"
                                              : compute_relative_path(ext.checkout_dir, makefile_dir);
    } else {
        source_dir = compute_relative_path(ext.source_dir, makefile_dir);
    }
    while (source_dir.size() > 1 && source_dir.back() == '/') source_dir.pop_back();
    const std::string cmake_source = ext.source_subdir.empty() ? "$(EXT_SOURCE)"
                                                               : "$(EXT_SOURCE)/" + ext.source_subdir;

    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: Failed to create Makefile: " << output_path << "\n";
        return false;
    }

    out << "# Auto-generated Makefile for " << project.name << " (" << config_name
        << (android ? ", Android" : "") << ") - external CMake project\n";
    out << "# Generated by sighmake\n\n";
    out << "MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))\n\n";

    if (android) {
        write_android_setup(out);
        out << "ANDROID_MAX_PAGE_SIZE ?= 16384\n";
    }

    out << "CMAKE ?= cmake\n";
    out << "GIT ?= git\n";
    out << "EXT_BUILD_TYPE = " << (ext.build_type.empty() ? config_name : ext.build_type) << "\n";
    out << "EXT_CONFIG = " << (ext.build_type.empty() ? config_name : ext.build_type) << "\n";

    // Generator: the buildscript's cmake_generator, else Ninja when installed, else Unix Makefiles.
    out << "CMAKE_GENERATOR ?= " << ext.generator << "\n";
    out << "ifeq ($(strip $(CMAKE_GENERATOR)),)\n";
    out << "  ifneq ($(strip $(shell ninja --version 2>/dev/null)),)\n";
    out << "    CMAKE_GENERATOR := Ninja\n";
    out << "  else\n";
    out << "    CMAKE_GENERATOR := Unix Makefiles\n";
    out << "  endif\n";
    out << "endif\n";
    out << "EXT_GENERATOR_ARGS :=\n";
    out << "ifeq ($(CMAKE_GENERATOR),Unix Makefiles)\n";
    out << "  EXT_GENERATOR_ARGS := -DCMAKE_MAKE_PROGRAM=\"$(MAKE)\"\n";
    out << "endif\n";

    out << "EXT_PLATFORM_ARGS :=\n";
    if (android) {
        out << "EXT_PLATFORM_ARGS := -DCMAKE_TOOLCHAIN_FILE=\"$(ANDROID_NDK_HOME)/build/cmake/android.toolchain.cmake\""
               " -DANDROID_ABI=$(ANDROID_ABI) -DANDROID_PLATFORM=android-$(ANDROID_API)\n";
        out << "ifneq ($(filter arm64-v8a x86_64,$(ANDROID_ABI)),)\n";
        out << "  ifneq ($(strip $(ANDROID_MAX_PAGE_SIZE)),)\n";
        out << "    EXT_PLATFORM_ARGS += -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON\n";
        out << "  endif\n";
        out << "endif\n";
    }
    // Honor the make job limit: `sighmake --build --parallel N` passes SIGHMAKE_JOBS=N (and -j N), a plain
    // `make -jN` is read from MAKEFLAGS. Without a limit cmake gets a bare --parallel.
    out << "EXT_JOBS = $(strip $(or $(SIGHMAKE_JOBS),$(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS))),$(patsubst j%,%,$(filter j%,$(firstword $(MAKEFLAGS))))))\n";
    out << "EXT_PARALLEL = --parallel $(shell n='$(EXT_JOBS)'; expr \"$$n\" : '[0-9][0-9]*$$' >/dev/null && echo $$n)\n";
    out << "EXT_OPTIONS =";
    for (const auto& option : ext.options) out << " " << option;
    out << "\n";
    out << "EXT_TARGETS =";
    for (const auto& target : ext.targets) out << " --target " << target;
    out << "\n\n";

    out << "EXT_SOURCE = " << source_dir << "\n";
    out << "EXT_BUILD_DIR = " << int_dir << "\n";
    out << "EXT_OUT_DIR = " << out_dir << "\n";
    std::string first_output;
    out << "EXT_OUTPUTS =";
    for (const auto& output : ext.outputs) {
        const std::string base = fs::path(output).filename().string();
        if (first_output.empty()) first_output = base;
        out << " $(EXT_OUT_DIR)/" << base;
    }
    out << "\n";
    out << "TARGET = $(EXT_OUT_DIR)/" << (first_output.empty() ? project.name + ".cmake-build" : first_output) << "\n\n";

    out << ".PHONY: all clean external-build\n";
    out << ".DEFAULT_GOAL := all\n\n";
    out << "all: external-build\n\n";

    if (use_git) {
        // The checkout is pinned: a stamp file records "<url>@<ref>" and the source is fetched
        // again whenever the buildscript pins something else.
        const std::string pin = ext.git_url + "@" + ext.git_ref;
        out << "EXT_PIN = " << pin << "\n";
        out << "EXT_STAMP = $(EXT_SOURCE).pin\n";
        out << "ifneq ($(shell cat \"$(EXT_STAMP)\" 2>/dev/null),$(EXT_PIN))\n";
        out << ".PHONY: $(EXT_STAMP)\n";
        out << "endif\n";
        out << "$(EXT_STAMP):\n";
        out << "\t@echo \"Fetching " << project.name << " (" << pin << ")\"\n";
        out << "\t@rm -rf \"$(EXT_SOURCE)\"\n";
        out << "\t@mkdir -p \"$(dir $(EXT_SOURCE))\"\n";
        if (ext.git_ref.empty()) {
            out << "\t$(GIT) clone -q --depth 1 " << ext.git_url << " \"$(EXT_SOURCE)\"\n";
        } else if (looks_like_commit_id(ext.git_ref)) {
            out << "\t$(GIT) init -q \"$(EXT_SOURCE)\"\n";
            out << "\t$(GIT) -C \"$(EXT_SOURCE)\" remote add origin " << ext.git_url << "\n";
            out << "\t$(GIT) -C \"$(EXT_SOURCE)\" fetch -q --depth 1 origin " << ext.git_ref << "\n";
            out << "\t$(GIT) -C \"$(EXT_SOURCE)\" checkout -q FETCH_HEAD\n";
        } else {
            out << "\t$(GIT) clone -q --depth 1 --branch " << ext.git_ref << " " << ext.git_url
                << " \"$(EXT_SOURCE)\"\n";
        }
        out << "\t@echo \"$(EXT_PIN)\" > \"$@\"\n\n";
        out << "external-build: $(EXT_STAMP)\n";
    } else {
        out << "external-build:\n";
    }

    out << "\t@mkdir -p \"$(EXT_BUILD_DIR)\" \"$(EXT_OUT_DIR)\"\n";
    out << "\t$(CMAKE) -S \"" << cmake_source << "\" -B \"$(EXT_BUILD_DIR)\" -G \"$(CMAKE_GENERATOR)\""
           " -DCMAKE_BUILD_TYPE=$(EXT_BUILD_TYPE) $(EXT_GENERATOR_ARGS) $(EXT_PLATFORM_ARGS) $(EXT_OPTIONS)\n";
    if (ext.build) {
        out << "\t$(CMAKE) --build \"$(EXT_BUILD_DIR)\" --config $(EXT_CONFIG) $(EXT_TARGETS) $(EXT_PARALLEL)\n";
    }
    for (const auto& output : ext.outputs) {
        const std::string base = fs::path(output).filename().string();
        out << "\t@test -f \"$(EXT_BUILD_DIR)/" << output << "\" || { echo \"sighmake: the CMake build of "
            << project.name << " did not produce " << output << "\"; exit 1; }\n";
        out << "\t@cmp -s \"$(EXT_BUILD_DIR)/" << output << "\" \"$(EXT_OUT_DIR)/" << base
            << "\" 2>/dev/null || cp -f \"$(EXT_BUILD_DIR)/" << output << "\" \"$(EXT_OUT_DIR)/" << base << "\"\n";
    }
    out << "\n";

    out << "clean:\n";
    out << "\trm -rf \"$(EXT_BUILD_DIR)\"";
    if (!ext.outputs.empty()) out << " $(EXT_OUTPUTS)";
    out << "\n";
    out.close();

    std::cout << "Generated: " << output_path << "\n";
    return true;
}

static std::string xml_attr_escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// AndroidManifest.xml of an android_apk project. The launcher activity defaults to
// android.app.NativeActivity, which loads lib<native_lib>.so and needs no Java code.
static std::string generate_android_manifest(const Project& project, bool has_code) {
    const auto& apk = project.android_apk;
    const std::string activity = apk.activity.empty() ? "android.app.NativeActivity" : apk.activity;
    const bool native_activity = activity == "android.app.NativeActivity";

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml << "<!-- Generated by sighmake for " << xml_attr_escape(project.name)
        << ". Change the buildscript (android_* settings) or use android_manifest. -->\n";
    xml << "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n";
    xml << "    package=\"" << xml_attr_escape(apk.package) << "\">\n";
    // uses-feature entries: `glEsVersion=0x00030000` (an attribute only), `name`, `name:false`, and
    // `name key=value ...` for features that carry more attributes (for example
    // `android.hardware.vulkan.version version=0x00401000`). `required=false` is accepted as a token.
    for (const auto& feature : apk.features) {
        std::istringstream tokens(feature);
        std::string first;
        tokens >> first;
        if (first.empty()) continue;
        std::vector<std::pair<std::string, std::string>> attributes;
        bool required = true;
        const size_t eq = first.find('=');
        if (eq != std::string::npos) {
            attributes.emplace_back(first.substr(0, eq), first.substr(eq + 1));
        } else {
            if (first.size() > 6 && first.compare(first.size() - 6, 6, ":false") == 0) {
                first.resize(first.size() - 6);
                required = false;
            }
            attributes.emplace_back("name", first);
        }
        for (std::string token; tokens >> token;) {
            const size_t token_eq = token.find('=');
            if (token_eq == std::string::npos) continue;
            const std::string key = token.substr(0, token_eq);
            const std::string value = token.substr(token_eq + 1);
            if (key == "required") {
                required = !(value == "false" || value == "0" || value == "no");
            } else {
                attributes.emplace_back(key, value);
            }
        }
        xml << "    <uses-feature";
        for (const auto& [key, value] : attributes) {
            xml << " android:" << xml_attr_escape(key) << "=\"" << xml_attr_escape(value) << "\"";
        }
        xml << " android:required=\"" << (required ? "true" : "false") << "\" />\n";
    }
    for (const auto& permission : apk.permissions) {
        const std::string name = permission.find('.') == std::string::npos
            ? "android.permission." + permission : permission;
        xml << "    <uses-permission android:name=\"" << xml_attr_escape(name) << "\" />\n";
    }
    xml << "    <application android:label=\""
        << xml_attr_escape(apk.label.empty() ? project.name : apk.label) << "\"";
    if (apk.application_attrs.find("hasCode") == std::string::npos) {
        xml << " android:hasCode=\"" << (has_code ? "true" : "false") << "\"";
    }
    if (apk.application_attrs.find("extractNativeLibs") == std::string::npos) {
        // Libraries are stored uncompressed and 16 KB aligned, so the loader maps them straight
        // out of the APK instead of extracting copies at install time.
        xml << " android:extractNativeLibs=\"" << (apk.extract_native_libs ? "true" : "false") << "\"";
    }
    if (!apk.icon.empty()) xml << " android:icon=\"" << xml_attr_escape(apk.icon) << "\"";
    if (!apk.application_attrs.empty()) xml << " " << apk.application_attrs;
    xml << ">\n";
    xml << "        <activity android:name=\"" << xml_attr_escape(activity) << "\" android:exported=\"true\"";
    if (apk.activity_attrs.find("configChanges") == std::string::npos) {
        xml << " android:configChanges=\"orientation|screenSize|screenLayout|keyboard|keyboardHidden|"
               "navigation|uiMode|density\"";
    }
    if (!apk.activity_attrs.empty()) xml << " " << apk.activity_attrs;
    xml << ">\n";
    if (native_activity) {
        xml << "            <meta-data android:name=\"android.app.lib_name\" android:value=\""
            << xml_attr_escape(apk.native_lib) << "\" />\n";
    }
    xml << "            <intent-filter>\n";
    xml << "                <action android:name=\"android.intent.action.MAIN\" />\n";
    xml << "                <category android:name=\"android.intent.category.LAUNCHER\" />\n";
    xml << "            </intent-filter>\n";
    xml << "        </activity>\n";
    xml << "    </application>\n";
    xml << "</manifest>\n";
    return xml.str();
}

// An android_apk project: one makefile per configuration that assembles, aligns, signs and verifies
// <outdir>/<target_name>.apk from the native libraries of the projects it depends on.
bool MakefileGenerator::generate_android_apk_makefile(const Project& project, const Configuration& config,
                                                      const std::string& config_key,
                                                      const std::string& output_path,
                                                      const ProjectLookup& project_lookup,
                                                      const Solution& solution) {
    namespace fs = std::filesystem;
    const auto& apk = project.android_apk;
    const std::string config_name = parse_config_key(config_key).first;
    const std::string platform = parse_config_key(config_key).second;
    const bool android = is_android_platform(platform);
    if (!android) {
        std::cerr << "Error: android_apk project '" << project.name << "' requires the Android platform.\n";
        return false;
    }
    if (apk.package.empty() && apk.manifest.empty()) {
        std::cerr << "Error: android_apk project '" << project.name
                  << "' needs android_package (or android_manifest).\n";
        return false;
    }

    fs::path makefile_dir = fs::path(output_path).parent_path();
    if (makefile_dir.empty()) makefile_dir = ".";

    std::string out_dir = compute_relative_path(
        config.out_dir.empty() ? default_makefile_out_dir(config_name, android)
                               : anchor_variable_dir(config.out_dir, project, config_key), makefile_dir);
    std::string int_dir = compute_relative_path(
        config.int_dir.empty() ? default_makefile_int_dir(config_name, project.name, android)
                               : anchor_variable_dir(config.int_dir, project, config_key), makefile_dir);
    while (out_dir.size() > 1 && out_dir.back() == '/') out_dir.pop_back();
    while (int_dir.size() > 1 && int_dir.back() == '/') int_dir.pop_back();
    const std::string apk_name = (config.target_name.empty() ? project.name : config.target_name) + ".apk";

    // Native libraries to package: shared libraries and external CMake .so outputs reachable from
    // the project through its dependencies (static libraries are only traversed).
    std::vector<std::string> libs;
    std::set<std::string> visited;
    std::function<void(const Project&)> collect = [&](const Project& from) {
        for (const auto& dep : from.project_references) {
            if (!dependency_applies_to_config(dep, config_key)) continue;
            if (!visited.insert(dep.name).second) continue;
            const Project* dep_project = find_project(project_lookup, dep.name);
            if (!dep_project) continue;
            const auto dep_it = dep_project->configurations.find(config_key);
            if (dep_it == dep_project->configurations.end()) continue;
            const Configuration& dep_config = dep_it->second;
            std::string dep_out = dep_config.out_dir.empty()
                ? default_makefile_out_dir(config_name, android)
                : anchor_variable_dir(dep_config.out_dir, *dep_project, config_key);
            if (!dep_out.empty() && dep_out.back() != '/' && dep_out.back() != '\\') dep_out += '/';
            if (dep_config.config_type == "DynamicLibrary") {
                std::string name = decorate_target_name(
                    dep_config.target_name.empty() ? dep_project->name : dep_config.target_name,
                    dep_config.config_type, android);
                libs.push_back(compute_relative_path(
                    dep_out + name + (dep_config.target_ext.empty() ? ".so" : dep_config.target_ext), makefile_dir));
            } else if (dep_config.config_type == "ExternalCMake") {
                for (const auto& output : dep_project->external_cmake.outputs) {
                    const std::string base = fs::path(output).filename().string();
                    if (to_lower(fs::path(base).extension().string()) == ".so") {
                        libs.push_back(compute_relative_path(dep_out + base, makefile_dir));
                    }
                }
            }
            collect(*dep_project);
        }
    };
    collect(project);
    for (const auto& extra : apk.native_libs) {
        libs.push_back(compute_relative_path(anchor_variable_dir(extra, project, config_key), makefile_dir));
    }
    std::vector<std::string> optional_libs;
    for (const auto& extra : apk.optional_native_libs) {
        optional_libs.push_back(compute_relative_path(anchor_variable_dir(extra, project, config_key), makefile_dir));
    }
    if (libs.empty() && optional_libs.empty()) {
        std::cerr << "Warning: android_apk project '" << project.name
                  << "' has no native libraries (add shared-library projects to target_link_libraries).\n";
    }

    const bool has_code = !apk.java.empty() || !apk.jars.empty() ||
        (!apk.activity.empty() && apk.activity != "android.app.NativeActivity");

    // Manifest: user-provided or generated next to the makefiles.
    std::string manifest_path;
    if (!apk.manifest.empty()) {
        // A custom manifest may use ${NAME} / ${NAME:-default}; those are expanded here (from -D and
        // find_package variables) into a copy next to the makefiles. A manifest without them is used in place.
        std::ifstream source(apk.manifest, std::ios::binary);
        if (!source) {
            std::cerr << "Error: Cannot read android_manifest " << apk.manifest << "\n";
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        if (text.find("${") == std::string::npos) {
            manifest_path = compute_relative_path(apk.manifest, makefile_dir);
        } else {
            const std::string manifest_name = project.name + "." + config_name + ".AndroidManifest.xml";
            std::ofstream manifest(makefile_dir / manifest_name, std::ios::binary);
            if (!manifest) {
                std::cerr << "Error: Failed to write " << (makefile_dir / manifest_name).string() << "\n";
                return false;
            }
            manifest << expand_variables(text, solution.variables);
            manifest.close();
            record_generated_output(makefile_dir / manifest_name);
            manifest_path = manifest_name;
        }
    } else {
        const std::string manifest_name = project.name + "." + config_name + ".AndroidManifest.xml";
        std::ofstream manifest(makefile_dir / manifest_name);
        if (!manifest) {
            std::cerr << "Error: Failed to write " << (makefile_dir / manifest_name).string() << "\n";
            return false;
        }
        manifest << generate_android_manifest(project, has_code);
        manifest.close();
        record_generated_output(makefile_dir / manifest_name);
        manifest_path = manifest_name;
    }

    // Tools found now are embedded as defaults; environment variables and the make command line
    // override them and the makefile falls back to a lookup when nothing was found.
    std::string sdk_default, build_tools_default, jar_default, api_default;
    if (auto sdk = android_sdk::find_sdk()) {
        sdk_default = sdk->generic_string();
        if (auto build_tools = android_sdk::find_build_tools(*sdk)) build_tools_default = build_tools->generic_string();
        if (auto platform_jar = android_sdk::find_platform_jar(*sdk)) {
            jar_default = platform_jar->jar.generic_string();
            api_default = platform_jar->api;
        }
    }

    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: Failed to create Makefile: " << output_path << "\n";
        return false;
    }

    out << "# Auto-generated Makefile for " << project.name << " (" << config_name
        << ", Android) - APK package\n";
    out << "# Generated by sighmake\n\n";
    out << "MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))\n";
    out << "APK_SELF := $(lastword $(MAKEFILE_LIST))\n\n";
    write_android_setup(out);
    out << "\n";
    out << "# Android SDK build tools (aapt2, zipalign, apksigner, d8) and the platform jar\n";
    out << "ANDROID_SDK := $(firstword $(ANDROID_HOME) $(ANDROID_SDK_ROOT) " << sdk_default << ")\n";
    if (!build_tools_default.empty()) {
        out << "ANDROID_BUILD_TOOLS ?= " << build_tools_default << "\n";
    }
    out << "ifeq ($(strip $(ANDROID_BUILD_TOOLS)),)\n";
    out << "  ANDROID_BUILD_TOOLS := $(lastword $(sort $(wildcard $(ANDROID_SDK)/build-tools/*)))\n";
    out << "endif\n";
    if (!jar_default.empty()) {
        out << "ANDROID_JAR ?= " << jar_default << "\n";
    }
    out << "ifeq ($(strip $(ANDROID_JAR)),)\n";
    out << "  ANDROID_JAR := $(lastword $(sort $(wildcard $(ANDROID_SDK)/platforms/android-*/android.jar)))\n";
    out << "endif\n";
    out << "ifeq ($(strip $(ANDROID_BUILD_TOOLS)),)\n";
    out << "  ifneq ($(MAKECMDGOALS),clean)\n";
    out << "    $(error Android SDK build-tools not found. Set ANDROID_HOME (or ANDROID_BUILD_TOOLS))\n";
    out << "  endif\n";
    out << "endif\n";
    out << "ifeq ($(OS),Windows_NT)\n";
    out << "  EXE := .exe\n";
    out << "  PATHSEP := ;\n";
    out << "else\n";
    out << "  EXE :=\n";
    out << "  PATHSEP := :\n";
    out << "endif\n";
    out << "JAVA_BIN := $(if $(JAVA_HOME),$(subst \\,/,$(JAVA_HOME))/bin/)\n";
    out << "JAVA ?= $(JAVA_BIN)java\n";
    out << "JAVAC ?= $(JAVA_BIN)javac\n";
    out << "JAR ?= $(JAVA_BIN)jar\n";
    out << "KEYTOOL ?= $(JAVA_BIN)keytool\n";
    out << "AAPT2 = \"$(ANDROID_BUILD_TOOLS)/aapt2$(EXE)\"\n";
    out << "ZIPALIGN = \"$(ANDROID_BUILD_TOOLS)/zipalign$(EXE)\"\n";
    out << "APKSIGNER = \"$(JAVA)\" -jar \"$(ANDROID_BUILD_TOOLS)/lib/apksigner.jar\"\n";
    out << "D8 = \"$(JAVA)\" -cp \"$(ANDROID_BUILD_TOOLS)/lib/d8.jar\" com.android.tools.r8.D8\n";
    out << "# 16 KB alignment of the stored .so files needs zipalign from build-tools 35 or newer.\n";
    out << "ZIPALIGN_FLAGS := $(if $(findstring -P,$(shell $(ZIPALIGN) 2>&1)),-P 16,-p)\n\n";

    out << "# Which ABIs go into the package. Every listed ABI must have been built already.\n";
    out << "ANDROID_APK_ABIS ?= $(ANDROID_ABI)\n";
    out << "# Release builds are signed with the keystore named by ANDROID_KEYSTORE; other\n";
    out << "# configurations with the debug keystore. ANDROID_SIGN=debug|release overrides.\n";
    out << "ANDROID_SIGN ?= auto\n";
    out << "APK_SIGN_MODE = $(if $(filter auto,$(ANDROID_SIGN)),"
        << (config_name == "Release" ? "release" : "debug") << ",$(ANDROID_SIGN))\n";
    out << "ANDROID_DEBUG_KEYSTORE ?= $(or $(HOME),$(USERPROFILE))/.android/debug.keystore\n\n";

    out << "APK_NAME = " << apk_name << "\n";
    // Output path of the package. With one ABI it is <outdir>/<name>.apk. A package holding several ABIs
    // (--abi a,b) is written once, to <outdir>/<name>.apk with $(ANDROID_ABI) replaced by "universal",
    // so it never lands in the directory of whichever ABI happened to be built last.
    std::string multi_abi_out_dir = out_dir;
    for (size_t pos = 0; (pos = multi_abi_out_dir.find("$(ANDROID_ABI)", pos)) != std::string::npos;) {
        multi_abi_out_dir.replace(pos, 14, "universal");
        pos += 9;
    }
    out << "APK_MULTI_ABI := $(if $(word 2,$(ANDROID_APK_ABIS)),yes)\n";
    out << "APK_OUT_DIR := $(if $(APK_MULTI_ABI)," << multi_abi_out_dir << "," << out_dir << ")\n";
    out << "APK := $(APK_OUT_DIR)/" << apk_name << "\n";
    out << "AAB_NAME = " << (config.target_name.empty() ? project.name : config.target_name) << ".aab\n";
    out << "AAB := $(APK_OUT_DIR)/$(AAB_NAME)\n";
    out << "# Set ANDROID_AAB=1 (or android_aab = true) to also build the Android App Bundle.\n";
    out << "ANDROID_AAB ?= " << (apk.aab ? "1" : "0") << "\n";
    out << "TARGET = $(APK)\n";
    out << "APK_STAGE := " << int_dir << "/apk\n";
    out << "APK_MANIFEST = " << manifest_path << "\n";
    out << "APK_MIN_SDK = " << (apk.min_sdk.empty() ? "$(ANDROID_API)" : apk.min_sdk) << "\n";
    out << "APK_TARGET_SDK = " << (!apk.target_sdk.empty() ? apk.target_sdk
                                    : (api_default.empty() ? "34" : api_default)) << "\n";
    out << "APK_VERSION_CODE = " << apk.version_code << "\n";
    out << "APK_VERSION_NAME = " << apk.version_name << "\n";
    out << "APK_CPP_SHARED = " << apk.cpp_shared << "\n";
    out << "APK_DEBUG_FLAG = " << (config_name == "Release" ? "" : "--debug-mode") << "\n";
    out << "# Native libraries per ABI ($(ANDROID_ABI) is rebound for every packaged ABI)\n";
    // Paths may contain spaces; make word lists cannot, so spaces are carried as @SP@ and restored by $(call sp,...)
    const auto encode_space = [](std::string value) {
        for (size_t pos = 0; (pos = value.find(' ', pos)) != std::string::npos; pos += 4) value.replace(pos, 1, "@SP@");
        return value;
    };
    out << "sp = $(subst @SP@, ,$(1))\n";
    out << "spesc = $(subst @SP@,\\ ,$(1))\n";
    out << "APK_LIBS =";
    for (const auto& lib : libs) out << " " << encode_space(lib);
    out << "\n";
    out << "# Optional libraries are packaged when the file exists and skipped otherwise\n";
    out << "APK_OPTIONAL_LIBS =";
    for (const auto& lib : optional_libs) out << " " << encode_space(lib);
    out << "\n";
    if (!apk.res_dir.empty()) out << "APK_RES = " << compute_relative_path(apk.res_dir, makefile_dir) << "\n";
    out << "APK_ASSET_DIRS =";
    for (const auto& dir : apk.assets) out << " " << encode_space(compute_relative_path(dir, makefile_dir));
    out << "\n";
    out << "APK_JAVA_DIRS =";
    for (const auto& dir : apk.java) out << " " << encode_space(compute_relative_path(dir, makefile_dir));
    out << "\n";
    out << "APK_JARS =";
    for (const auto& jar : apk.jars) out << " " << encode_space(compute_relative_path(jar, makefile_dir));
    out << "\n\n";

    out << "# $(ANDROID_NDK_HOME) layout: <triple> directory names of the C++ runtime\n";
    out << "android_triple = $(if $(filter arm64-v8a,$(1)),aarch64-linux-android,$(if $(filter armeabi-v7a,$(1)),"
           "arm-linux-androideabi,$(if $(filter x86,$(1)),i686-linux-android,x86_64-linux-android)))\n";
    out << "APK_STL = $(ANDROID_TOOLCHAIN)/../sysroot/usr/lib/$(call android_triple,$(ANDROID_ABI))/libc++_shared.so\n\n";

    out << ".PHONY: all clean\n";
    out << ".DEFAULT_GOAL := all\n\n";
    out << "ifneq ($(strip $(SIGHMAKE_SKIP_PACKAGING)),)\n";
    out << "all:\n";
    out << "\t@echo \"Skipping packaging of $(APK_NAME) (SIGHMAKE_SKIP_PACKAGING is set)\"\n";
    out << "else\n";
    out << "all: $(APK)$(if $(filter 1 yes true,$(ANDROID_AAB)), $(AAB))\n";
    out << "endif\n\n";
    out << "aab: $(AAB)\n";
    out << ".PHONY: aab\n\n";

    // Files that change without being named as prerequisites: package again when any is newer.
    out << "APK_TREES = $(APK_ASSET_DIRS) $(APK_JAVA_DIRS)" << (apk.res_dir.empty() ? "" : " $(APK_RES)") << "\n";
    out << "ifneq ($(strip $(APK_TREES)),)\n";
    out << "  ifneq ($(strip $(shell find $(foreach t,$(APK_TREES),\"$(call sp,$(t))\") -type f -newer \"$(APK)\" 2>/dev/null | head -n 1)),)\n";
    out << "    .PHONY: $(APK)\n";
    out << "  endif\n";
    out << "endif\n\n";

    const std::string stage = "$(APK_STAGE)";
    out << "$(APK): $(APK_SELF) $(APK_MANIFEST) $(foreach lib,$(APK_LIBS),$(call spesc,$(lib))) "
           "$(foreach ANDROID_ABI,$(ANDROID_APK_ABIS),$(foreach lib,$(APK_OPTIONAL_LIBS),$(wildcard $(call spesc,$(lib)))))\n";
    out << "\t@echo \"Packaging $(APK_NAME)\"\n";
    out << "\t@rm -rf \"" << stage << "\" && mkdir -p \"" << stage << "/content\" \"$(dir $@)\"\n";
    if (!apk.res_dir.empty()) {
        out << "\t$(AAPT2) compile --dir \"$(APK_RES)\" -o \"" << stage << "/res.zip\"\n";
    }
    out << "\t$(AAPT2) link -o \"" << stage << "/base.apk\" -I \"$(ANDROID_JAR)\" --manifest \"$(APK_MANIFEST)\""
           " --min-sdk-version $(APK_MIN_SDK) --target-sdk-version $(APK_TARGET_SDK)"
           " --version-code $(APK_VERSION_CODE) --version-name \"$(APK_VERSION_NAME)\" $(APK_DEBUG_FLAG)";
    if (!apk.res_dir.empty()) out << " -R \"" << stage << "/res.zip\" --auto-add-overlay";
    out << "\n";
    if (has_code) {
        out << "\t@mkdir -p \"" << stage << "/classes\" \"" << stage << "/dex\"\n";
        if (!apk.java.empty()) {
            out << "\t@find $(foreach d,$(APK_JAVA_DIRS),\"$(call sp,$(d))\") -name \"*.java\" 2>/dev/null | sed 's/.*/\"&\"/' > \"" << stage << "/java.rsp\"\n";
            out << "\t@if [ -s \"" << stage << "/java.rsp\" ]; then echo \"Compiling Java sources\" && "
                   "\"$(JAVAC)\" -encoding UTF-8 --release 8 -Xlint:-options -classpath "
                   "\"$(ANDROID_JAR)$(foreach jar,$(APK_JARS),$(PATHSEP)$(call sp,$(jar)))\" -d \"" << stage << "/classes\" @\""
                << stage << "/java.rsp\" && \"$(JAR)\" --create --file \"" << stage
                << "/classes.jar\" --no-manifest -C \"" << stage << "/classes\" .; fi\n";
        }
        out << "\t@set -- $(foreach j,$(APK_JARS),\"$(call sp,$(j))\"); if [ -f \"" << stage << "/classes.jar\" ]; then set -- \""
            << stage << "/classes.jar\" \"$$@\"; fi; test $$# -gt 0 || { echo \"sighmake: " << project.name
            << " needs Java code (android_java or android_jars) for its activity\"; exit 1; }; "
               "$(D8) --lib \"$(ANDROID_JAR)\" --min-api $(APK_MIN_SDK) $(if $(APK_DEBUG_FLAG),--debug,--release)"
               " --output \"" << stage << "/dex\" \"$$@\"\n";
        out << "\t@cp -f \"" << stage << "/dex/\"*.dex \"" << stage << "/content/\"\n";
    }
    out << "\t@$(foreach ANDROID_ABI,$(ANDROID_APK_ABIS),mkdir -p \"" << stage << "/content/lib/$(ANDROID_ABI)\"; "
           "$(foreach lib,$(APK_LIBS),test -f \"$(call sp,$(lib))\" || { echo \"sighmake: $(call sp,$(lib)) not found - build ABI "
           "$(ANDROID_ABI) first\"; exit 1; }; cp -f \"$(call sp,$(lib))\" \"" << stage << "/content/lib/$(ANDROID_ABI)/\"; ) "
           "$(foreach lib,$(APK_OPTIONAL_LIBS),if [ -f \"$(call sp,$(lib))\" ]; then cp -f \"$(call sp,$(lib))\" \"" << stage
        << "/content/lib/$(ANDROID_ABI)/\"; else echo \"sighmake: optional library $(call sp,$(lib)) not found, skipped\"; fi; ) "
           "if [ \"$(APK_CPP_SHARED)\" = \"true\" ] || { [ \"$(APK_CPP_SHARED)\" = \"auto\" ] && "
           "grep -qa \"libc++_shared.so\" \"" << stage << "/content/lib/$(ANDROID_ABI)\"/*.so 2>/dev/null; }; then "
           "cp -f \"$(APK_STL)\" \"" << stage << "/content/lib/$(ANDROID_ABI)/\"; fi; )\n";
    if (!apk.assets.empty()) {
        out << "\t@mkdir -p \"" << stage << "/content/assets\"\n";
        out << "\t@$(foreach asset_dir,$(APK_ASSET_DIRS),cp -R \"$(call sp,$(asset_dir))/.\" \"" << stage << "/content/assets/\"; )\n";
    }
    out << "\t@echo \"Adding libraries, dex and assets uncompressed\"\n";
    out << "\t\"$(JAR)\" --update --file \"" << stage << "/base.apk\" --no-manifest --no-compress -C \"" << stage
        << "/content\" .\n";
    out << "\t$(ZIPALIGN) -f $(ZIPALIGN_FLAGS) 4 \"" << stage << "/base.apk\" \"" << stage << "/aligned.apk\"\n";
    out << "\t@if [ \"$(APK_SIGN_MODE)\" = \"release\" ]; then \\\n";
    out << "\t  test -n \"$$ANDROID_KEYSTORE\" || { echo \"sighmake: release signing needs ANDROID_KEYSTORE and "
           "ANDROID_KEYSTORE_PASSWORD (optional: ANDROID_KEY_ALIAS, ANDROID_KEY_PASSWORD); use ANDROID_SIGN=debug "
           "for a debug-signed package\"; exit 1; }; \\\n";
    out << "\t  echo \"Signing $(APK_NAME) with $$ANDROID_KEYSTORE\"; \\\n";
    out << "\t  $(APKSIGNER) sign --ks \"$$ANDROID_KEYSTORE\" --ks-pass env:ANDROID_KEYSTORE_PASSWORD"
           "$(if $(ANDROID_KEY_ALIAS), --ks-key-alias \"$(ANDROID_KEY_ALIAS)\")"
           "$(if $(ANDROID_KEY_PASSWORD), --key-pass env:ANDROID_KEY_PASSWORD) --v4-signing-enabled false"
           " --out \"$@\" \"" << stage << "/aligned.apk\"; \\\n";
    out << "\telse \\\n";
    out << "\t  test -f \"$(ANDROID_DEBUG_KEYSTORE)\" || { echo \"Creating debug keystore $(ANDROID_DEBUG_KEYSTORE)\"; "
           "mkdir -p \"$(dir $(ANDROID_DEBUG_KEYSTORE))\" && \"$(KEYTOOL)\" -genkeypair -keystore "
           "\"$(ANDROID_DEBUG_KEYSTORE)\" -storepass android -keypass android -alias androiddebugkey -keyalg RSA "
           "-keysize 2048 -validity 10000 -dname \"CN=Android Debug,O=Android,C=US\"; }; \\\n";
    out << "\t  echo \"Signing $(APK_NAME) with the debug keystore\"; \\\n";
    out << "\t  $(APKSIGNER) sign --ks \"$(ANDROID_DEBUG_KEYSTORE)\" --ks-pass pass:android --key-pass pass:android"
           " --ks-key-alias androiddebugkey --v4-signing-enabled false --out \"$@\" \"" << stage << "/aligned.apk\"; \\\n";
    out << "\tfi\n";
    out << "\t$(APKSIGNER) verify --min-sdk-version $(APK_MIN_SDK) \"$@\"\n";
    out << "\t$(ZIPALIGN) -c $(ZIPALIGN_FLAGS) 4 \"$@\"\n";
    out << "\t@echo \"Built $@\"\n\n";

    // Android App Bundle: the base module is the staged APK content in bundle layout (resources in protobuf
    // form from `aapt2 link --proto-format`, manifest/, dex/, lib/, assets/), assembled by bundletool and
    // signed with jarsigner. Built with ANDROID_AAB=1 / android_aab = true, or `make aab`.
    {
        std::string aab_rules = R"MK(
# bundletool: BUNDLETOOL (a bundletool-all-<version>.jar or a launcher script), else bundletool /
# bundletool.jar / bundletool-all.jar found on PATH.
JARSIGNER ?= $(JAVA_BIN)jarsigner
BUNDLETOOL_PATH = $(strip $(if $(strip $(BUNDLETOOL)),$(BUNDLETOOL),$(shell IFS=:; for d in $$PATH; do for n in bundletool bundletool.jar bundletool-all.jar; do if [ -f "$$d/$$n" ]; then echo "$$d/$$n"; exit 0; fi; done; done)))
BUNDLETOOL_RUN = $(if $(filter %.jar,$(BUNDLETOOL_PATH)),"$(JAVA)" -jar "$(BUNDLETOOL_PATH)","$(BUNDLETOOL_PATH)")

$(AAB): $(APK)
@TAB@@test -n "$(BUNDLETOOL_PATH)" || { echo "sighmake: bundletool not found. Set BUNDLETOOL to bundletool-all-<version>.jar (or a bundletool launcher) or put bundletool on PATH. Download: https://github.com/google/bundletool/releases"; exit 1; }
@TAB@@test -d "@STAGE@/content" || { echo "sighmake: @STAGE@/content is missing - run the APK build first (make clean, then make)"; exit 1; }
@TAB@@echo "Bundling $(AAB_NAME)"
@TAB@@rm -rf "@STAGE@/aab" && mkdir -p "@STAGE@/aab/base/manifest" "@STAGE@/aab/base/dex"
@TAB@$(AAPT2) link --proto-format -o "@STAGE@/aab/base-proto.apk" -I "$(ANDROID_JAR)" --manifest "$(APK_MANIFEST)" --min-sdk-version $(APK_MIN_SDK) --target-sdk-version $(APK_TARGET_SDK) --version-code $(APK_VERSION_CODE) --version-name "$(APK_VERSION_NAME)" $(APK_DEBUG_FLAG)@RESLINK@
@TAB@cd "@STAGE@/aab/base" && "$(JAR)" xf "$(abspath @STAGE@/aab/base-proto.apk)"
@TAB@@mv "@STAGE@/aab/base/AndroidManifest.xml" "@STAGE@/aab/base/manifest/AndroidManifest.xml"
@TAB@@for f in "@STAGE@/content/"*.dex; do if [ -f "$$f" ]; then cp -f "$$f" "@STAGE@/aab/base/dex/"; fi; done
@TAB@@if [ -d "@STAGE@/content/lib" ]; then cp -R "@STAGE@/content/lib" "@STAGE@/aab/base/lib"; fi
@TAB@@if [ -d "@STAGE@/content/assets" ]; then cp -R "@STAGE@/content/assets" "@STAGE@/aab/base/assets"; fi
@TAB@@printf '%s' '{"optimizations":{"uncompressNativeLibraries":{"enabled":true}}}' > "@STAGE@/aab/BundleConfig.json"
@TAB@"$(JAR)" --create --file "@STAGE@/aab/base.zip" --no-manifest -C "@STAGE@/aab/base" .
@TAB@@rm -f "$@" && mkdir -p "$(dir $@)"
@TAB@$(BUNDLETOOL_RUN) build-bundle --modules="@STAGE@/aab/base.zip" --config="@STAGE@/aab/BundleConfig.json" --output="$@"
@TAB@@if [ "$(APK_SIGN_MODE)" = "release" ]; then \
@TAB@  test -n "$$ANDROID_KEYSTORE" && test -n "$(ANDROID_KEY_ALIAS)" || { echo "sighmake: release signing of the bundle needs ANDROID_KEYSTORE, ANDROID_KEYSTORE_PASSWORD and ANDROID_KEY_ALIAS (optional: ANDROID_KEY_PASSWORD); use ANDROID_SIGN=debug for a debug-signed bundle"; exit 1; }; \
@TAB@  echo "Signing $(AAB_NAME) with $$ANDROID_KEYSTORE"; \
@TAB@  "$(JARSIGNER)" -keystore "$$ANDROID_KEYSTORE" -storepass:env ANDROID_KEYSTORE_PASSWORD$(if $(ANDROID_KEY_PASSWORD), -keypass:env ANDROID_KEY_PASSWORD) -sigalg SHA256withRSA -digestalg SHA-256 "$@" "$(ANDROID_KEY_ALIAS)" >/dev/null; \
@TAB@else \
@TAB@  test -f "$(ANDROID_DEBUG_KEYSTORE)" || { echo "sighmake: debug keystore $(ANDROID_DEBUG_KEYSTORE) not found - build the APK first"; exit 1; }; \
@TAB@  echo "Signing $(AAB_NAME) with the debug keystore"; \
@TAB@  "$(JARSIGNER)" -keystore "$(ANDROID_DEBUG_KEYSTORE)" -storepass android -keypass android -sigalg SHA256withRSA -digestalg SHA-256 "$@" androiddebugkey >/dev/null; \
@TAB@fi
@TAB@"$(JARSIGNER)" -verify "$@" >/dev/null
@TAB@@echo "Built $@"

)MK";
        const auto replace_all = [&](const std::string& token, const std::string& value) {
            for (size_t pos = 0; (pos = aab_rules.find(token, pos)) != std::string::npos; pos += value.size()) {
                aab_rules.replace(pos, token.size(), value);
            }
        };
        replace_all("@TAB@", "\t");
        replace_all("@STAGE@", stage);
        replace_all("@RESLINK@", apk.res_dir.empty() ? "" : " -R \"" + stage + "/res.zip\" --auto-add-overlay");
        out << aab_rules;
    }

    out << "clean:\n";
    out << "\trm -rf \"" << stage << "\" \"$(APK)\" \"$(AAB)\"\n";
    out.close();

    std::cout << "Generated: " << output_path << "\n";
    return true;
}

// Generate a single Makefile for a project and configuration
bool MakefileGenerator::generate_makefile(const Project& project, const Solution& solution,
                                         const std::string& config_key, const std::string& output_path) {
    auto project_lookup = build_project_lookup(solution);
    pic_cache_.clear();  // Single-project entry point: never reuse a graph from an earlier call
    pic_cache_solution_ = &solution;
    return generate_makefile_with_lookup(project, solution, config_key, output_path, project_lookup);
}

bool MakefileGenerator::generate_makefile_with_lookup(const Project& project, const Solution& solution,
                                                      const std::string& config_key, const std::string& output_path,
                                                      const MakefileGenerator::ProjectLookup& project_lookup) {
    namespace fs = std::filesystem;

    // Get configuration
    auto it = project.configurations.find(config_key);
    if (it == project.configurations.end()) {
        std::cerr << "Error: Configuration '" << config_key << "' not found for project '" << project.name << "'\n";
        return false;
    }
    const Configuration& config = it->second;

    if (detect_project_language(project) == "C#") {
        // dotnet builds the generated .csproj; the platform is the one the project file was written for.
        const auto [csharp_config, csharp_platform] = parse_config_key(config_key);
        std::string dotnet_platform = csharp_platform;
        for (const auto& [key, cell] : project.configurations) {
            (void)cell;
            const auto [name, platform] = parse_config_key(key);
            if (name == csharp_config && !is_unix_platform(platform) && (dotnet_platform == csharp_platform || platform == "x64")) {
                dotnet_platform = platform;
            }
        }
        std::ofstream csharp_out(output_path);
        if (!csharp_out) {
            std::cerr << "Error: Failed to create Makefile: " << output_path << "\n";
            return false;
        }
        const std::string csproj = VcxprojGenerator::csproj_filename(project);
        csharp_out << "# Auto-generated Makefile for " << project.name << " (" << csharp_config << ") - C# project\n";
        csharp_out << "# Generated by sighmake\n\n";
        csharp_out << "DOTNET ?= dotnet\n";
        csharp_out << "TARGET = " << csproj << "\n\n";
        csharp_out << ".PHONY: all clean\n";
        csharp_out << ".DEFAULT_GOAL := all\n\n";
        csharp_out << "all:\n";
        csharp_out << "\t$(DOTNET) build \"" << csproj << "\" -c " << csharp_config << " -p:Platform=" << dotnet_platform << "\n\n";
        csharp_out << "clean:\n";
        csharp_out << "\t$(DOTNET) clean \"" << csproj << "\" -c " << csharp_config << " -p:Platform=" << dotnet_platform << "\n";
        csharp_out.close();
        std::cout << "Generated: " << output_path << "\n";
        return true;
    }
    if (config.config_type == "ExternalCMake") {
        return generate_external_cmake_makefile(project, config, config_key, output_path);
    }
    if (config.config_type == "AndroidApk") {
        return generate_android_apk_makefile(project, config, config_key, output_path, project_lookup, solution);
    }

    // Parse config key into config and platform
    std::string config_name, platform;
    size_t pipe_pos = config_key.find('|');
    if (pipe_pos != std::string::npos) {
        config_name = config_key.substr(0, pipe_pos);
        platform = config_key.substr(pipe_pos + 1);
    } else {
        config_name = config_key;
        platform = "x64";
    }

    const bool android = is_android_platform(platform);

    // Determine target name and extension
    std::string target_name = decorate_target_name(
        config.target_name.empty() ? project.name : config.target_name, config.config_type, android);
    std::string target_ext = config.target_ext;
    if (target_ext.empty()) {
        if (config.config_type == "Application") {
            target_ext = "";  // No extension for executables on Linux
        } else if (config.config_type == "Driver") {
            target_ext = ".sys";
        } else if (config.config_type == "DynamicLibrary") {
            if (android) {
                target_ext = ".so";  // Android always uses ELF .so, even when generated on macOS
            } else {
#ifdef __APPLE__
                target_ext = ".dylib";
#else
                target_ext = ".so";
#endif
            }
        } else if (config.config_type == "StaticLibrary") {
            target_ext = ".a";
        }
    }

    // Strip .exe extension for Makefiles (Linux)
    if (target_ext == ".exe") {
        target_ext = "";
    }

    // Get the directory containing the makefile for computing relative paths
    fs::path makefile_dir = fs::path(output_path).parent_path();
    if (makefile_dir.empty()) {
        makefile_dir = ".";
    }

    // Output and intermediate directories - convert to relative paths
    std::string out_dir = compute_relative_path(
        config.out_dir.empty() ? default_makefile_out_dir(config_name, android)
                               : anchor_variable_dir(config.out_dir, project, config_key), makefile_dir);
    std::string int_dir = compute_relative_path(
        config.int_dir.empty() ? default_makefile_int_dir(config_name, project.name, android)
                               : anchor_variable_dir(config.int_dir, project, config_key), makefile_dir);

    // Ensure directories end with /
    if (!out_dir.empty() && out_dir.back() != '/') out_dir += '/';
    if (!int_dir.empty() && int_dir.back() != '/') int_dir += '/';

    // Full target path
    std::string target = out_dir + target_name + target_ext;

    // Open output file
    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: Failed to create Makefile: " << output_path << "\n";
        return false;
    }

    // Write header
    out << "# Auto-generated Makefile for " << project.name << " (" << config_name
        << (android ? ", Android" : "") << ")\n";
    out << "# Generated by sighmake\n\n";

    out << "MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))\n";
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary") {
        // Receipts are written by a Python script. Pick the first interpreter that actually runs:
        // on Windows hosts python3 is often only the Microsoft Store stub, which fails.
        out << "ifeq ($(origin PYTHON),undefined)\n";
        out << "  PYTHON := $(or $(firstword $(foreach py,python3 python py,$(if $(findstring sighmake-py,$(shell $(py) -c \"print('sighmake-py')\" 2>/dev/null)),$(py)))),python3)\n";
        out << "endif\n";
    }
    out << "PYTHON ?= python3\n\n";

    // Language a C/C++ source is compiled as: its extension unless `compile_as` (per file, else the
    // configuration's) asks for the other language. Returned as a representative extension.
    auto compile_language_ext = [&](const SourceFile& src) {
        std::string ext = file_types::lowercase_extension(src.path);
        if (src.type != FileType::ClCompile) return ext;
        const std::string* compile_as = find_config_setting(src.settings.compile_as, config_key);
        if (!compile_as || compile_as->empty()) compile_as = &config.cl_compile.compile_as;
        if (*compile_as == "CompileAsCpp" && file_types::is_c_source(ext)) return std::string(".cpp");
        if (*compile_as == "CompileAsC" && file_types::is_cpp_source(ext)) return std::string(".c");
        return ext;
    };

    // Determine compiler
    bool has_cpp_files = false;
    bool has_c_files = false;
    bool has_objcxx_files = false;
    bool has_nasm_files = false;
    for (const auto& src : project.sources) {
        if (!source_applies_to_platform(src, parse_config_key(config_key).second)) continue;
        if (src.type == FileType::ClCompile) {
            std::string ext = compile_language_ext(src);
            if (file_types::is_cpp_source(ext)) {
                has_cpp_files = true;
            } else if (file_types::is_c_source(ext)) {
                has_c_files = true;
            }
        } else if (src.type == FileType::ObjCxx) {
            has_objcxx_files = true;
        } else if (src.type == FileType::NASM) {
            has_nasm_files = true;
        }
    }

    const bool links_binary = config.config_type == "Application" ||
                              config.config_type == "DynamicLibrary" ||
                              config.config_type == "Driver";
    const bool emits_target_receipt = config.config_type == "Application" ||
                                      config.config_type == "DynamicLibrary";
    const bool links_with_cxx = has_cpp_files || has_objcxx_files;

    // Compiler variables (their literal text is also recorded in the command-line stamps)
    std::string cxx_cmd = "$(CXX)", cc_cmd = "$(CC)", ar_cmd = "ar";
    if (android) {
        // Android NDK toolchain. The --target wrappers are what the NDK's own
        // clang launcher scripts do, but calling clang directly works on every
        // host OS (the .cmd wrappers are Windows-only, the bare ones POSIX-only).
        write_android_setup(out);
        cxx_cmd = "\"$(ANDROID_TOOLCHAIN)/clang++\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)";
        cc_cmd = "\"$(ANDROID_TOOLCHAIN)/clang\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)";
        ar_cmd = "\"$(ANDROID_TOOLCHAIN)/llvm-ar\"";
        out << "CXX = \"$(ANDROID_TOOLCHAIN)/clang++\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)\n";
        out << "CC = \"$(ANDROID_TOOLCHAIN)/clang\" --target=$(ANDROID_TRIPLE)$(ANDROID_API)\n";
        out << "AR = \"$(ANDROID_TOOLCHAIN)/llvm-ar\"\n";
        out << "STRIP = \"$(ANDROID_TOOLCHAIN)/llvm-strip\"\n";
        if (links_binary) {
            out << "# 16 KB page alignment (Android 15+ devices; NDK r27 does not do this by default).\n";
            out << "# Applies to the 64-bit ABIs; override with ANDROID_MAX_PAGE_SIZE=4096, or empty to disable.\n";
            out << "ANDROID_MAX_PAGE_SIZE ?= 16384\n";
            out << "ANDROID_LINK_FLAGS :=\n";
            out << "ifneq ($(filter arm64-v8a x86_64,$(ANDROID_ABI)),)\n";
            out << "  ifneq ($(strip $(ANDROID_MAX_PAGE_SIZE)),)\n";
            out << "    ANDROID_LINK_FLAGS += -Wl,-z,max-page-size=$(ANDROID_MAX_PAGE_SIZE)\n";
            out << "  endif\n";
            out << "endif\n";
        }
    } else {
        if (links_with_cxx && !solution.cxx_compiler.empty()) {
            cxx_cmd = solution.cxx_compiler;
            out << "CXX = " << solution.cxx_compiler << "\n";
        } else if (links_with_cxx) {
#ifdef __APPLE__
            cxx_cmd = "clang++";
            out << "CXX = clang++\n";
#else
            cxx_cmd = "g++";
            out << "CXX = g++\n";
#endif
        }
        // C and assembler-only binaries still need a linker driver. Also emit
        // CC for empty binary targets so the generated recipe is syntactically
        // complete instead of starting with an empty $(CC).
        if ((has_c_files || (links_binary && !links_with_cxx)) && !solution.c_compiler.empty()) {
            cc_cmd = solution.c_compiler;
            out << "CC = " << solution.c_compiler << "\n";
        } else if (has_c_files || (links_binary && !links_with_cxx)) {
#ifdef __APPLE__
            cc_cmd = "clang";
            out << "CC = clang\n";
#else
            cc_cmd = "gcc";
            out << "CC = gcc\n";
#endif
        }
    }
    std::string nasm_cmd = "nasm", nasmflags_text;
    if (has_nasm_files) {
        std::string nasm_exe = config.nasm.path.empty() ? "nasm" : config.nasm.path;
        nasm_cmd = nasm_exe;
        out << "NASM = " << nasm_exe << "\n";
    }

    // Position-independent code (see project_uses_position_independent_code). PIC objects also get
    // -fno-semantic-interposition on ELF targets unless the project sets semantic_interposition =
    // true: without it GCC must assume every exported function can be replaced at load time, so it
    // inlines less across functions and reaches globals through the GOT. Mach-O (two-level
    // namespaces) has no such interposition model, so Apple targets do not need the flag.
    const bool position_independent = project_uses_position_independent_code(
        solution, project, config_key, &pic_static_libraries(solution, config_key));
#ifdef __APPLE__
    const bool elf_target = android;
#else
    const bool elf_target = true;
#endif
    const bool no_semantic_interposition = position_independent && elf_target && !project.semantic_interposition;
    std::string pic_flags;
    if (position_independent) {
        pic_flags = "-fPIC ";
        if (no_semantic_interposition) pic_flags += "$(SIGHMAKE_NO_SEMANTIC_INTERPOSITION) ";
    }
    // ELF shared libraries also link with -Wl,-Bsymbolic-functions: calls between functions of the
    // library bind inside it at link time instead of through the PLT. Like -fno-semantic-interposition
    // this gives up replacing the library's functions from outside (LD_PRELOAD), so the same
    // `semantic_interposition = true` opts out. Data symbols keep default binding (copy relocations).
    const bool bsymbolic_functions = config.config_type == "DynamicLibrary" && elf_target &&
                                     !project.semantic_interposition;
    const std::string shared_link_flags =
        bsymbolic_functions ? "-shared $(SIGHMAKE_BSYMBOLIC_FUNCTIONS)" : "-shared";

    // Compiler flags
    std::string cxxflags = get_compiler_flags(config, project, makefile_dir, false, config_name, pic_flags);
    std::string cflags = get_compiler_flags(config, project, makefile_dir, true, config_name, pic_flags);
    std::string ldflags = get_linker_flags(config, makefile_dir, android);
    std::string ldlibs = get_linker_libs(config, makefile_dir);

    struct ArchiveEntry {
        std::string project_name;
        std::string path;
        bool whole_archive;
    };
    std::vector<ArchiveEntry> dep_archives;

    // Add project reference outputs (.a files) to the link line. PUBLIC and
    // INTERFACE dependencies propagate normally. PRIVATE dependencies of a
    // static library are link-only requirements of its final consumer, so they
    // must propagate as well (matching CMake's $<LINK_ONLY:...> semantics).
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        // Collect all project .a files by traversing the dependency tree
        std::set<std::string> visited_deps;

        // Helper to compute a project's output archive path
        auto get_archive_path = [&](const Project& dep_proj, const std::string& cfg_key,
                                     const std::string& cfg_name) -> std::vector<std::string> {
            auto dep_config_it = dep_proj.configurations.find(cfg_key);
            if (dep_config_it == dep_proj.configurations.end()) return {};
            const auto& dep_config = dep_config_it->second;

            // An external CMake build exposes the libraries it produces (copied into its outdir).
            if (dep_config.config_type == "ExternalCMake") {
                std::string ext_out_dir = dep_config.out_dir.empty()
                    ? default_makefile_out_dir(cfg_name, android)
                    : anchor_variable_dir(dep_config.out_dir, dep_proj, cfg_key);
                if (!ext_out_dir.empty() && ext_out_dir.back() != '/' && ext_out_dir.back() != '\\')
                    ext_out_dir += '/';
                std::vector<std::string> linked;
                for (const auto& output : external_cmake_link_outputs(dep_proj)) {
                    linked.push_back(ext_out_dir + output);
                }
                return linked;
            }

            if (dep_config.config_type != "StaticLibrary" &&
                dep_config.config_type != "DynamicLibrary") return {};

            std::string dep_target_name = decorate_target_name(
                dep_config.target_name.empty() ? dep_proj.name : dep_config.target_name,
                dep_config.config_type, android);
            std::string dep_target_ext = dep_config.target_ext;
            if (dep_target_ext.empty()) {
                if (dep_config.config_type == "StaticLibrary") {
                    dep_target_ext = ".a";
                } else if (android) {
                    dep_target_ext = ".so";
                } else {
#ifdef __APPLE__
                    dep_target_ext = ".dylib";
#else
                    dep_target_ext = ".so";
#endif
                }
            }

            std::string dep_out_dir = dep_config.out_dir.empty()
                ? default_makefile_out_dir(cfg_name, android)
                : anchor_variable_dir(dep_config.out_dir, dep_proj, cfg_key);
            if (!dep_out_dir.empty() && dep_out_dir.back() != '/' && dep_out_dir.back() != '\\')
                dep_out_dir += '/';

            return {dep_out_dir + dep_target_name + dep_target_ext};
        };

        // Recursive traversal: visit a dependency and its transitive PUBLIC deps
        // whole_archive flag only applies to the direct dependency, not transitive ones
        std::function<void(const std::string&, bool, bool)> collect_deps =
            [&](const std::string& dep_name, bool add_locally, bool is_whole_archive) {
            if (visited_deps.count(dep_name)) {
                // A dependency can first be reached transitively and later be
                // listed directly with WHOLE_ARCHIVE. Preserve the stronger
                // direct-link semantics in that case.
                if (is_whole_archive) {
                    for (auto& archive : dep_archives) {
                        if (archive.project_name == dep_name) {
                            archive.whole_archive = true;
                        }
                    }
                }
                return;
            }
            visited_deps.insert(dep_name);

            const Project* dep_proj = find_project(project_lookup, dep_name);
            if (!dep_proj) return;

            const auto dep_config_it = dep_proj->configurations.find(config_key);
            const bool is_static_library = dep_config_it != dep_proj->configurations.end() &&
                dep_config_it->second.config_type == "StaticLibrary";

            // Add this project's archive if it's a library
            if (add_locally) {
                for (const auto& archive : get_archive_path(*dep_proj, config_key, config_name)) {
                    dep_archives.push_back({dep_name, compute_relative_path(archive, makefile_dir),
                                            is_whole_archive});
                }
            }

            // A static library does not perform a final link, so its PRIVATE
            // libraries remain required when a final binary consumes it.
            // Shared-library PRIVATE dependencies stay encapsulated.
            for (const auto& sub_dep : dep_proj->project_references) {
                if (!dependency_applies_to_config(sub_dep, config_key)) continue;
                if (sub_dep.visibility == DependencyVisibility::PRIVATE && !is_static_library) continue;
                if (!sub_dep.link_library_dependencies) continue;
                collect_deps(sub_dep.name, true, false);
            }
        };

        // Start from the project's direct dependencies
        for (const auto& dep : project.project_references) {
            if (!dependency_applies_to_config(dep, config_key)) continue;
            if (dep.visibility == DependencyVisibility::INTERFACE) continue;
            if (!dep.link_library_dependencies) continue;
            collect_deps(dep.name, true, dep.whole_archive);
        }

        // Prepend archives to ldlibs (reverse order so direct deps come first)
        for (auto archive_it = dep_archives.rbegin(); archive_it != dep_archives.rend(); ++archive_it) {
            if (archive_it->whole_archive) {
#ifdef __APPLE__
                const bool macho_linker = !android;
#else
                const bool macho_linker = false;
#endif
                if (macho_linker) {
                    ldlibs = "-Wl,-force_load," + archive_it->path + " " + ldlibs;
                } else {
                    ldlibs = "-Wl,--whole-archive " + archive_it->path + " -Wl,--no-whole-archive " + ldlibs;
                }
            } else {
                ldlibs = archive_it->path + " " + ldlibs;
            }
        }
    }

    if (no_semantic_interposition && (has_cpp_files || has_objcxx_files || has_c_files)) {
        // GCC has the flag since 5. Clang rejects it before 10 (NDK r19-r21 ship clang 8/9), so for
        // clang the compiler is asked once when make reads this file; an unsupported compiler (or a
        // host shell that cannot run the probe) leaves the variable empty. Setting the variable on
        // the make command line (empty to disable) overrides both.
        const bool probe_cxx = has_cpp_files || has_objcxx_files;
        const std::string& probe_cmd = probe_cxx ? cxx_cmd : cc_cmd;
        const bool clang_family = android || probe_cmd.find("clang") != std::string::npos;
        out << "# Position-independent code without semantic interposition (semantic_interposition = true opts out)\n";
        if (clang_family) {
            out << "ifeq ($(origin SIGHMAKE_NO_SEMANTIC_INTERPOSITION),undefined)\n";
            out << "  SIGHMAKE_NO_SEMANTIC_INTERPOSITION := $(shell echo | " << probe_cmd
                << " -fno-semantic-interposition -fsyntax-only -x " << (probe_cxx ? "c++" : "c")
                << " - 2>/dev/null && echo -fno-semantic-interposition)\n";
            out << "endif\n";
        } else {
            out << "SIGHMAKE_NO_SEMANTIC_INTERPOSITION ?= -fno-semantic-interposition\n";
        }
    }
    if (bsymbolic_functions) {
        // The linker is asked once when make reads this file (GNU ld, gold, lld and mold accept the
        // flag; anything else leaves the variable empty). Setting the variable on the make command
        // line (empty to disable) overrides the probe.
        const std::string& link_cmd = links_with_cxx ? cxx_cmd : cc_cmd;
        out << "# Shared library calls bind locally (semantic_interposition = true opts out)\n";
        out << "ifeq ($(origin SIGHMAKE_BSYMBOLIC_FUNCTIONS),undefined)\n";
        out << "  SIGHMAKE_BSYMBOLIC_FUNCTIONS := $(shell echo | " << link_cmd
            << " -x c - -shared -nostdlib -Wl,-Bsymbolic-functions -o /dev/null 2>/dev/null"
            << " && echo -Wl,-Bsymbolic-functions)\n";
        out << "endif\n";
    }
    if (has_cpp_files || has_objcxx_files) {
        out << "CXXFLAGS = " << cxxflags << "\n";
    }
    if (has_c_files) {
        out << "CFLAGS = " << cflags << "\n";
    }
    if (has_objcxx_files) {
        out << "OBJCXXFLAGS = $(CXXFLAGS)";
        if (!config.cl_compile.objcxx_flags.empty()) {
            out << " " << config.cl_compile.objcxx_flags;
        }
        out << "\n";
    }

    if (has_nasm_files) {
        // Build NASMFLAGS from config
        std::string nasm_fmt = config.nasm.format;
        if (nasm_fmt.empty()) nasm_fmt = "elf64";  // Default for Makefile (Linux)

        std::string nasmflags = "-f " + nasm_fmt;
        for (const auto& inc : config.nasm.include_directories) {
            std::string rel_inc = compute_relative_path(inc, makefile_dir);
            nasmflags += " -I" + rel_inc + "/";
        }
        for (const auto& def : config.nasm.preprocessor_definitions) {
            nasmflags += " -D" + def;
        }
        if (!config.nasm.additional_options.empty()) {
            nasmflags += " " + config.nasm.additional_options;
        }
        nasmflags_text = nasmflags;
        out << "NASMFLAGS = " << nasmflags << "\n";
    }

    if (!ldflags.empty()) {
        out << "LDFLAGS = " << ldflags << "\n";
    }
    if (!ldlibs.empty()) {
        out << "LDLIBS = " << ldlibs << "\n";
    }
    if (!dep_archives.empty()) {
        out << "PROJECT_DEPS =";
        for (const auto& archive : dep_archives) {
            out << " \\\n  " << archive.path;
        }
        out << "\n";
    }

    out << "\n";

    // Output paths
    out << "# Output\n";
    out << "TARGET = " << target << "\n";
    out << "OBJ_DIR = " << int_dir << "\n";
    if (emits_target_receipt) {
        out << "SIGHMAKE_TARGET_RECEIPT = " << out_dir << project.name << ".targetreceipt.json\n";
        out << "SIGHMAKE_RECEIPT_ARCHITECTURE ?= "
            << (android ? "$(ANDROID_ABI)" : "$(shell uname -m)") << "\n";
    }
    out << "\n";

    // Check if PCH is enabled for this configuration
    auto [has_pch, pch_header] = get_pch_info(config);
    std::string pch_header_path;
    std::string pch_output_path;
    std::string pch_include_base; // The path to include (without .gch extension)

    if (has_pch && !pch_header.empty()) {
        // Compute relative path to PCH header
        pch_header_path = compute_relative_path(resolve_pch_header(pch_header, project, config), makefile_dir);

        // PCH output path: $(OBJ_DIR)/pch_filename.gch
        pch_output_path = int_dir + fs::path(pch_header).filename().string() + ".gch";

        // The include base is the path without .gch extension (for -include flag)
        pch_include_base = int_dir + fs::path(pch_header).filename().string();

        // Write PCH variables
        out << "# Precompiled header\n";
        out << "PCH_HEADER = " << make_escape_spaces(pch_header_path) << "\n";
        out << "PCH_OUTPUT = " << pch_output_path << "\n\n";
    }

    // Collect source files and generate object file list
    std::vector<std::string> obj_files;
    std::vector<std::pair<std::string, std::string>> source_to_obj; // (source, object)
    std::vector<bool> source_uses_pch; // Track which files use PCH
    std::vector<std::string> source_lang_ext; // Language the file compiles as (see compile_language_ext)
    std::vector<std::string> source_extra_flags; // Per-file flags: -x for compile_as, per-file defines

    for (const auto& src : project.sources) {
        if (!source_applies_to_platform(src, parse_config_key(config_key).second)) continue;
        if (src.type == FileType::ClCompile || src.type == FileType::ObjCxx || src.type == FileType::NASM) {
            // Check if excluded for this config
            const bool* excluded = find_config_setting(src.settings.excluded, config_key);
            if (excluded && *excluded) {
                continue; // Skip excluded files
            }

            // Check PCH mode for this file
            auto [mode, header] = get_file_pch_mode(src, config_key, config);

            // Skip files with "Create" mode - in GCC, we compile the header directly
            if (mode == "Create") {
                continue;
            }

            // Generate object file path
            // Convert source path to relative (using the same logic as output dirs)
            std::string src_relative = compute_relative_path(src.path, makefile_dir);
            std::string obj_path = make_object_path(src, src_relative, config_key, int_dir, makefile_dir,
                                                  project.buildscript_path);

            const std::string lang_ext = compile_language_ext(src);

            // Determine if this file uses PCH. The header is precompiled as C++, which a C
            // translation unit cannot use: C files compile without it.
            bool uses_pch = has_pch && (mode == "Use" || (mode.empty() && !header.empty())) &&
                            !file_types::is_c_source(lang_ext);
            std::string extra_flags;
            if (lang_ext != file_types::lowercase_extension(src.path)) {
                extra_flags += file_types::is_c_source(lang_ext) ? " -x c" : " -x c++";
            }
            if (const auto* defines = find_config_setting(src.settings.preprocessor_defines, config_key)) {
                for (const auto& def : *defines) {
                    if (!def.empty()) extra_flags += " -D" + def;
                }
            }

            obj_files.push_back(obj_path);
            source_to_obj.push_back({src_relative, obj_path});
            source_uses_pch.push_back(uses_pch);
            source_lang_ext.push_back(lang_ext);
            source_extra_flags.push_back(extra_flags);
        }
    }

    // Object files list
    out << "# Object files\n";
    out << "OBJS =";
    for (const auto& obj : obj_files) {
        out << " \\\n  " << obj;
    }
    out << "\n\n";

    // Command-line stamps. Make only compares timestamps, so an object whose compiler flags or
    // defines changed (but whose source did not) would stay stale. sighmake writes one stamp file
    // per language, and one for the link step, at generation time, and only when the command line
    // differs from the stored content: a stamp's mtime therefore changes exactly when its command
    // line changed. Objects (and the target) list the stamp as an ordinary prerequisite, so nothing
    // extra runs at make time. Changes made only on the make command line or in the environment
    // (make CXXFLAGS+=...) are not tracked; buildscript changes always go through regeneration.
    bool stamp_cxx = false, stamp_c = false, stamp_objcxx = false, stamp_nasm = false;
    // Per-file flags are recorded in the stamp of the file's language, so changing them rebuilds.
    std::string per_file_cxx, per_file_c, per_file_objcxx;
    for (size_t i = 0; i < source_to_obj.size(); ++i) {
        const std::string& e = source_lang_ext[i];
        const std::string per_file = source_extra_flags[i].empty()
            ? "" : " [" + source_to_obj[i].second + ":" + source_extra_flags[i] + "]";
        if (file_types::is_cpp_source(e)) { stamp_cxx = true; per_file_cxx += per_file; }
        else if (file_types::is_c_source(e)) { stamp_c = true; per_file_c += per_file; }
        else if (file_types::is_objcxx_source(e)) { stamp_objcxx = true; per_file_objcxx += per_file; }
        else if (e == ".asm" || e == ".nasm") stamp_nasm = true;
    }
    const bool stamp_link = config.config_type == "Application" || config.config_type == "DynamicLibrary" ||
                            config.config_type == "Driver" || config.config_type == "StaticLibrary";
    const std::string pch_stamp_suffix =
        (has_pch && !pch_include_base.empty()) ? (" -include \"" + pch_include_base + "\"") : "";
    struct StampDef { const char* var; std::string file; std::string text; };
    std::vector<StampDef> stamps;
    {
        // Stamp names carry the configuration so configurations sharing one intermediate
        // directory do not rewrite each other's stamps on every generation.
        std::string cfg_tag;
        for (char ch : config_key) cfg_tag += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '-';
        auto stamp_file = [&](const char* lang) { return std::string("sighmake-") + lang + "." + cfg_tag + ".flags"; };
        std::string objcxx_extra = config.cl_compile.objcxx_flags.empty() ? "" : " " + config.cl_compile.objcxx_flags;
        if (stamp_cxx) stamps.push_back({"SIGHMAKE_CXX_STAMP", stamp_file("cxx"), cxx_cmd + " " + cxxflags + pch_stamp_suffix + per_file_cxx});
        if (stamp_c) stamps.push_back({"SIGHMAKE_C_STAMP", stamp_file("c"), cc_cmd + " " + cflags + pch_stamp_suffix + per_file_c});
        if (stamp_objcxx) stamps.push_back({"SIGHMAKE_OBJCXX_STAMP", stamp_file("objcxx"), cxx_cmd + " " + cxxflags + objcxx_extra + pch_stamp_suffix + per_file_objcxx});
        if (stamp_nasm) stamps.push_back({"SIGHMAKE_NASM_STAMP", stamp_file("nasm"), nasm_cmd + " " + nasmflags_text});
        if (stamp_link) {
            std::string link_text;
            if (config.config_type == "StaticLibrary") {
                link_text = ar_cmd + " rcs";
            } else {
                link_text = (links_with_cxx ? cxx_cmd : cc_cmd) +
                    (config.config_type == "DynamicLibrary" ? " " + shared_link_flags : "") + " " + ldflags +
                    (android ? " $(ANDROID_LINK_FLAGS)" : "") + " " + ldlibs;
            }
            stamps.push_back({"SIGHMAKE_LINK_STAMP", stamp_file("link"), link_text});
        }

        // The Makefile is run with its own directory as the working directory (make -C), and the
        // intermediate directory is relative to it unless absolute. A directory that depends on
        // make variables cannot be resolved here: such configurations get no stamps.
        bool dir_ok = int_dir.find('$') == std::string::npos;
        const fs::path stamp_dir = fs::path(makefile_dir) / fs::path(int_dir);
        std::vector<StampDef> written;
        for (auto& st : stamps) {
            if (!dir_ok) break;
            const fs::path stamp_path = stamp_dir / st.file;
            const std::string content = st.text + "\n";
            std::error_code ec;
            std::string existing;
            {
                std::ifstream in(stamp_path, std::ios::binary);
                if (in) existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (existing != content) {
                fs::create_directories(stamp_dir, ec);
                std::ofstream sf(stamp_path, std::ios::binary | std::ios::trunc);
                sf << content;
                if (!sf) continue;
            }
            written.push_back(st);
        }
        stamps.swap(written);
    }
    auto stamp_var_for = [&](const std::string& e) -> const char* {
        for (const auto& st : stamps) {
            std::string v = st.var;
            if (file_types::is_cpp_source(e) && v == "SIGHMAKE_CXX_STAMP") return "$(SIGHMAKE_CXX_STAMP)";
            if (file_types::is_c_source(e) && v == "SIGHMAKE_C_STAMP") return "$(SIGHMAKE_C_STAMP)";
            if (file_types::is_objcxx_source(e) && v == "SIGHMAKE_OBJCXX_STAMP") return "$(SIGHMAKE_OBJCXX_STAMP)";
            if ((e == ".asm" || e == ".nasm") && v == "SIGHMAKE_NASM_STAMP") return "$(SIGHMAKE_NASM_STAMP)";
        }
        return "";
    };
    const bool link_stamped = std::any_of(stamps.begin(), stamps.end(),
        [](const StampDef& st) { return std::string(st.var) == "SIGHMAKE_LINK_STAMP"; });
    const bool cxx_stamped = std::any_of(stamps.begin(), stamps.end(),
        [](const StampDef& st) { return std::string(st.var) == "SIGHMAKE_CXX_STAMP"; });
    if (!stamps.empty()) {
        out << "# Command-line stamps (written by sighmake at generation time when flags change)\n";
        for (const auto& st : stamps) {
            out << st.var << " = $(OBJ_DIR)" << st.file << "\n";
        }
        out << "\n";
    }

    // Build events. A command may span several lines; each becomes its own recipe line. An event
    // with use_in_build = false is not run.
    const std::string pre_build_command = config.pre_build_event.use_in_build ? config.pre_build_event.command : "";
    const std::string pre_link_command = config.pre_link_event.use_in_build ? config.pre_link_event.command : "";
    const std::string post_build_command = config.post_build_event.use_in_build ? config.post_build_event.command : "";
    auto write_event_command = [&out](const std::string& command) {
        std::istringstream lines(command);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            out << "\t" << line << "\n";
        }
    };

    // Phony targets
    out << ".PHONY: all clean";
    if (!pre_build_command.empty()) out << " prebuild";
    if (emits_target_receipt) out << " sighmake-target-receipt";
    out << "\n\n";

    out << ".DEFAULT_GOAL := all\n\n";

    // Pre-build event
    if (!pre_build_command.empty()) {
        out << "# Pre-build event\n";
        out << "prebuild:\n";
        write_event_command(pre_build_command);
        out << "\n";
    }

    // Default target
    out << "all: " << (emits_target_receipt ? "sighmake-target-receipt" : "$(TARGET)") << "\n\n";

    // Stamp rules. Normally the stamp already exists and make only stats it. If it is missing
    // (for example after "make clean" removed the intermediate directory) an empty one is
    // created; the next generation rewrites it and the objects rebuild once.
    if (!stamps.empty()) {
        for (const auto& st : stamps) {
            out << "$(" << st.var << "):\n";
            out << "\t@mkdir -p $(dir $@) && touch $@\n\n";
        }
        if (link_stamped) {
            out << "$(TARGET): $(SIGHMAKE_LINK_STAMP)\n\n";
        }
    }

    // PCH compilation rule
    if (has_pch && !pch_header_path.empty()) {
        out << "# Precompiled header compilation\n";
        out << "$(PCH_OUTPUT): $(PCH_HEADER)" << (cxx_stamped ? " $(SIGHMAKE_CXX_STAMP)" : "") << "\n";
        out << "\t@mkdir -p $(dir $@)\n";
        out << "\t$(CXX) $(CXXFLAGS) -x c++-header -MMD -MP -MF $@.d -o $@ \"$<\"\n\n";
    }

    // Link rule
    if (!pre_build_command.empty()) {
        out << "$(OBJS)";
        if (has_pch && !pch_header_path.empty()) {
            out << " $(PCH_OUTPUT)";
        }
        out << ": | prebuild\n\n";
        out << "$(TARGET): $(OBJS)";
        if (!dep_archives.empty()) {
            out << " $(PROJECT_DEPS)";
        }
        out << " | prebuild\n";
    } else {
        out << "$(TARGET): $(OBJS)";
        if (!dep_archives.empty()) {
            out << " $(PROJECT_DEPS)";
        }
        out << "\n";
    }
    out << "\t@mkdir -p $(dir $@)\n";

    // Pre-link event
    if (!pre_link_command.empty()) {
        write_event_command(pre_link_command);
    }

    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        // Link executable or shared library
        std::string compiler = links_with_cxx ? "$(CXX)" : "$(CC)";
#ifdef __APPLE__
        // ld64 resolves static archives regardless of their order on the command line.
        const char* ldlibs = "$(LDLIBS)";
#else
        // GNU ld and lld scan each static archive once, in order; a group rescans them until no
        // new symbols resolve, so dependency order between archives does not matter.
        const char* ldlibs = "-Wl,--start-group $(LDLIBS) -Wl,--end-group";
#endif
        if (android) {
            // The Android toolchain is lld/GNU style whatever the host is: always group archives.
            ldlibs = "-Wl,--start-group $(LDLIBS) -Wl,--end-group";
            // Objects and libraries go through a response file (GNU make writes it while it expands
            // the recipe, so the output directory is created in the same step): link lines of
            // large projects exceed the Windows command-line limit. Shared libraries get a
            // soname equal to their file name, which is what the Android loader and the APK
            // packager (lib<name>.so) expect; ANDROID_LINK_FLAGS carries the page-size alignment.
            out << "\t$(shell mkdir -p $(dir $@))$(file >$@.rsp,$(OBJS) " << ldlibs << ")\n";
            if (config.config_type == "DynamicLibrary") {
                out << "\t" << compiler << " " << shared_link_flags
                    << " -Wl,-soname,$(notdir $@) $(LDFLAGS) $(ANDROID_LINK_FLAGS) -o $@ @$@.rsp\n";
            } else {
                out << "\t" << compiler << " $(LDFLAGS) $(ANDROID_LINK_FLAGS) -o $@ @$@.rsp\n";
            }
            out << "\t@rm -f $@.rsp\n";
        } else if (config.config_type == "DynamicLibrary") {
            out << "\t" << compiler << " " << shared_link_flags << " $(LDFLAGS) -o $@ $(OBJS) " << ldlibs << "\n";
        } else {
            out << "\t" << compiler << " $(LDFLAGS) -o $@ $(OBJS) " << ldlibs << "\n";
        }
    } else if (config.config_type == "StaticLibrary") {
        // Create static library (Android uses the NDK's llvm-ar via $(AR))
        // ar replaces listed members but retains removed source objects unless recreated.
        out << "\trm -f $@\n";
        if (android) {
            // Long object lists overflow the Windows command-line limit when cross-compiling from a
            // Windows host; pass them through a response file written by GNU make.
            // $(file ...) is expanded before any recipe line runs, so the output directory is
            // created during expansion (the plain mkdir line above would run too late).
            out << "\t$(shell mkdir -p $(dir $@))$(file >$@.rsp,$(OBJS))\n";
            out << "\t$(AR) rcs $@ @$@.rsp\n";
            out << "\t@rm -f $@.rsp\n";
        } else {
            out << "\tar rcs $@ $(OBJS)\n";
        }
    }

    // Strip symbols from executables and shared libraries (`strip`, default Release only).
    // On Linux, debug symbols are embedded in the binary unlike Windows .pdb files
    if (config.config_type == "Application" || config.config_type == "DynamicLibrary" || config.config_type == "Driver") {
        if (makefile_strips_outputs(config, parse_config_key(config_key).first)) {
            if (android) {
                // Host strip can't handle Android ELF binaries; use the NDK's llvm-strip
                out << "\t$(STRIP) --strip-unneeded $@\n";
            } else {
#ifdef __APPLE__
                if (config.config_type == "DynamicLibrary") {
                    out << "\tstrip -x $@\n";
                } else {
                    out << "\tstrip $@\n";
                }
#else
                out << "\tstrip $@\n";
#endif
            }
        }
    }

    // Post-build event (sees the finished binary, after stripping)
    if (!post_build_command.empty()) {
        write_event_command(post_build_command);
    }

    if (emits_target_receipt) {
        std::string receipt_platform;
        if (android) {
            receipt_platform = "Android";
        } else {
            std::string lower_plat = to_lower(platform);
            if (lower_plat == "linux") {
                receipt_platform = "Linux";
            } else if (lower_plat == "macos" || lower_plat == "darwin" || lower_plat == "osx") {
                receipt_platform = "macOS";
            } else {
#ifdef __APPLE__
                receipt_platform = "macOS";
#else
                receipt_platform = "Linux";
#endif
            }
        }

        // Keep receipt validation separate from the link recipe. The phony target deliberately
        // runs on every normal build so dependency changes, newly-present optional files, and a
        // prior failed receipt are observed even when the primary artifact is already up to date.
        out << "\n# Declarative target receipt\n";
        out << "sighmake-target-receipt: $(TARGET)\n";
        out << "\t@$(PYTHON) \"$(MAKEFILE_DIR)write-sighmake-target-receipt.py\" "
            << "--metadata \"$(MAKEFILE_DIR)" << project.name << ".runtime-dependencies.txt\" "
            << "--target \"" << project.name << "\" "
            << "--platform \"" << receipt_platform << "\" "
            << "--architecture \"$(SIGHMAKE_RECEIPT_ARCHITECTURE)\" "
            << "--configuration \"" << config_name << "\" "
            << "--primary-artifact \"$(TARGET)\" "
            << "--output \"$(SIGHMAKE_TARGET_RECEIPT)\"\n";
    }

    out << "\n";

    // Compilation rules for each source file
    for (size_t i = 0; i < source_to_obj.size(); ++i) {
        const auto& [src, obj] = source_to_obj[i];
        bool uses_pch = source_uses_pch[i];

        const std::string& ext = source_lang_ext[i];

        std::string compiler;
        std::string flags;
        bool is_nasm = false;
        if (file_types::is_cpp_source(ext)) {
            compiler = "$(CXX)";
            flags = "$(CXXFLAGS)";
        } else if (file_types::is_c_source(ext)) {
            compiler = "$(CC)";
            flags = "$(CFLAGS)";
        } else if (file_types::is_objcxx_source(ext)) {
            compiler = "$(CXX)";
            flags = "$(OBJCXXFLAGS)";
        } else if (ext == ".asm" || ext == ".nasm") {
            compiler = "$(NASM)";
            flags = "$(NASMFLAGS)";
            is_nasm = true;
        } else {
            continue; // Skip unknown file types
        }

        // Write dependency line - add PCH dependency if file uses it
        out << obj << ": " << make_escape_spaces(src);
        if (!is_nasm && uses_pch && has_pch) {
            out << " $(PCH_OUTPUT)";
        }
        if (const std::string sv = stamp_var_for(ext); !sv.empty()) out << " " << sv;
        out << "\n";

        out << "\t@mkdir -p $(dir $@)\n";

        if (is_nasm) {
            out << "\t" << compiler << " " << flags << " -o $@ \"$<\"\n\n";
        } else {
            out << "\t" << compiler << " " << flags << source_extra_flags[i];

            // Add -include flag to force PCH inclusion for files that use it
            if (uses_pch && has_pch && !pch_include_base.empty()) {
                out << " -include \"" << pch_include_base << "\"";
            }

            out << " -MMD -MP -c -o $@ \"$<\"\n\n";
        }
    }

    // Clean rule
    out << "clean:\n";
    if (has_pch && !pch_output_path.empty()) {
        out << "\trm -rf $(OBJ_DIR) $(TARGET) $(PCH_OUTPUT)";
    } else {
        out << "\trm -rf $(OBJ_DIR) $(TARGET)";
    }
    if (emits_target_receipt) out << " $(SIGHMAKE_TARGET_RECEIPT)";
    out << "\n\n";

    // Include dependency files
    if (!obj_files.empty()) {
        out << "# Include dependencies\n";
        out << "-include $(OBJS:.o=.d)\n";
    }
    if (has_pch && !pch_header_path.empty()) {
        // Headers included by the precompiled header rebuild it (and so every object using it)
        out << "-include $(PCH_OUTPUT).d\n";
    }

    if (emits_target_receipt && !write_target_receipt_support(project, output_path)) {
        return false;
    }

    out.close();

    std::cout << "Generated: " << output_path << "\n";
    return true;
}

// Generate all Makefiles for a solution
bool MakefileGenerator::generate(Solution& solution, const std::string& output_dir) {
    namespace fs = std::filesystem;

    // Create output directory if it doesn't exist
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        try {
            fs::create_directories(output_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create output directory: " << e.what() << "\n";
            return false;
        }
    }

    // Create build directory
    fs::path build_dir = fs::path(output_dir) / "build";
    if (!fs::exists(build_dir)) {
        try {
            fs::create_directories(build_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to create build directory: " << e.what() << "\n";
            return false;
        }
    }

    std::cout << "Generating Makefiles for solution: " << solution.name << "\n";
    pic_cache_.clear();  // The solution may have changed since a previous generate()
    pic_cache_solution_ = &solution;

    // C# projects are built by `dotnet build` on their generated .csproj. Give each configuration a cell
    // for the host platform (copied from the project's existing cell) so the master Makefile lists it, and
    // write the project files next to the makefiles.
    {
        const std::string host_platform = target_os() == TargetOS::MacOS ? "macOS" : "Linux";
        VcxprojGenerator csproj_writer;
        for (auto& project : solution.projects) {
            if (project.is_package_project || detect_project_language(project) != "C#") continue;
            std::map<std::string, std::string> source_cell;  // config name -> key of an existing cell
            std::set<std::string> has_unix_cell;
            for (const auto& [key, cfg] : project.configurations) {
                (void)cfg;
                const auto [config_name, platform] = parse_config_key(key);
                if (is_unix_platform(platform)) has_unix_cell.insert(config_name);
                else if (!source_cell.count(config_name) || platform == "x64") source_cell[config_name] = key;
            }
            for (const auto& [config_name, key] : source_cell) {
                if (has_unix_cell.count(config_name)) continue;
                project.configurations[config_name + "|" + host_platform] = project.configurations.at(key);
            }
            if (!csproj_writer.generate_csproj(project, solution,
                    (build_dir / VcxprojGenerator::csproj_filename(project)).string())) {
                return false;
            }
        }
    }

    auto project_lookup = build_project_lookup(solution);

    // Generate Makefiles for each project and configuration
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;  // Skip synthetic find_package projects
        // Generate a Makefile for each configuration
        for (const auto& [config_key, config] : project.configurations) {
            // Parse config key
            std::string config_name, platform;
            size_t pipe_pos = config_key.find('|');
            if (pipe_pos != std::string::npos) {
                config_name = config_key.substr(0, pipe_pos);
                platform = config_key.substr(pipe_pos + 1);
            } else {
                config_name = config_key;
            }

            // Skip Windows configs for makefile generator
            if (is_windows_platform(platform)) continue;
            // Only the best host match among configs sharing this makefile is generated.
            if (find_makefile_config_key(project, config_name, is_android_platform(platform)) != config_key) continue;

            // Generate Makefile path (Android configs get their own makefiles,
            // e.g. App.Debug.Android, so they don't collide with Linux/macOS ones)
            std::string makefile_name =
                make_project_config_target(project, config_name, is_android_platform(platform));
            fs::path makefile_path = build_dir / makefile_name;

            if (!generate_makefile_with_lookup(project, solution, config_key, makefile_path.string(), project_lookup)) {
                return false;
            }
            record_generated_output(makefile_path);
        }
    }

    // Generate master Makefile to build all projects
    if (!generate_master_makefile(solution, output_dir)) {
        return false;
    }

    std::cout << "Makefile generation complete!\n";

    // Write build cache for --build support
    {
        BuildCache cache;
        cache.generator = "makefile";
        cache.solution_name = solution.name;
        cache.configurations = solution.configurations;
        for (const auto& p : solution.platforms) {
            if (!is_windows_platform(p)) {
                cache.platforms.push_back(p);
            }
        }
        cache.build_dir = "build";
        cache.write(output_dir);
    }

    return true;
}

bool MakefileGenerator::generate_master_makefile(const Solution& solution, const std::string& output_dir) {
    namespace fs = std::filesystem;

    fs::path build_dir = fs::path(output_dir) / "build";
    fs::path makefile_path = build_dir / "Makefile";

    std::ofstream out(makefile_path);
    if (!out) {
        std::cerr << "Error: Failed to create master Makefile: " << makefile_path << "\n";
        return false;
    }
    record_generated_output(makefile_path);

    // Collect unique config names (without platform), skipping Windows platforms.
    // Android configs are tracked separately - they build through the NDK and get
    // their own <Config>.Android targets.
    std::set<std::string> configs;
    std::set<std::string> android_configs;
    for (const auto& project : solution.projects) {
        if (project.is_package_project) continue;  // Skip synthetic find_package projects
        for (const auto& [config_key, config] : project.configurations) {
            size_t pipe_pos = config_key.find('|');
            std::string config_name = (pipe_pos != std::string::npos)
                ? config_key.substr(0, pipe_pos)
                : config_key;
            std::string platform = (pipe_pos != std::string::npos)
                ? config_key.substr(pipe_pos + 1)
                : "";
            if (is_windows_platform(platform)) continue;  // Skip Windows configs
            if (is_android_platform(platform)) {
                android_configs.insert(config_name);
            } else {
                configs.insert(config_name);
            }
        }
    }

    if ((configs.empty() && android_configs.empty()) || solution.projects.empty()) {
        // Nothing to build
        out << "# Empty solution - no targets\n";
        out << "all:\n\t@echo \"No projects to build\"\n";
        std::cerr << "Warning: No non-Windows platforms found. Makefile has no targets.\n";
        std::cerr << "  Hint: Add 'Linux' or 'Android' to your platforms list, e.g.: platforms = x64, Linux\n";
        return true;
    }

    // Determine default config (prefer Debug, otherwise first alphabetically)
    std::string default_config = configs.count("Debug") ? "Debug" : (configs.empty() ? "" : *configs.begin());
    std::string default_android_config = android_configs.count("Debug")
        ? "Debug" : (android_configs.empty() ? "" : *android_configs.begin());
    auto project_lookup = build_project_lookup(solution);

    std::vector<const Project*> buildable_projects;
    for (const auto& project : solution.projects) {
        if (!project.is_package_project) {
            buildable_projects.push_back(&project);
        }
    }

    out << "# Master Makefile - generated by sighmake\n";
    out << "# Build all projects with: make\n";
    out << "# Build specific config:   make Release\n";
    out << "# Build specific project:  make ProjectName\n";
    if (!android_configs.empty()) {
        out << "# Build for Android:       make android\n";
        out << "# Android config/ABI:      make Release.Android ANDROID_ABI=x86_64 ANDROID_API=26\n";
    }
    out << "# Clean all:               make clean\n";
    out << "# Install:                 sudo make install\n";
    out << "# Install to custom dir:   make install PREFIX=/opt/myapp\n\n";

    out << "PREFIX ?= /usr/local\n\n";

    // Probe for a working Python once and hand it to every per-project makefile (they only probe
    // themselves when PYTHON is not already set), instead of once per project.
    bool any_receipts = false;
    for (const auto* proj : buildable_projects) {
        for (const auto& [key, cfg] : proj->configurations) {
            (void)key;
            if (cfg.config_type == "Application" || cfg.config_type == "DynamicLibrary") any_receipts = true;
        }
    }
    if (any_receipts) {
        out << "ifeq ($(origin PYTHON),undefined)\n";
        out << "  PYTHON := $(or $(firstword $(foreach py,python3 python py,$(if $(findstring sighmake-py,$(shell $(py) -c \"print('sighmake-py')\" 2>/dev/null)),$(py)))),python3)\n";
        out << "endif\n";
        out << "export PYTHON\n\n";
    }

    // .PHONY targets
    out << ".PHONY: all clean install uninstall";
    for (const auto& cfg : configs) {
        out << " " << cfg;
    }
    if (!android_configs.empty()) {
        out << " android";
        for (const auto& cfg : android_configs) {
            out << " " << cfg << ".Android";
        }
    }
    for (const auto& proj : solution.projects) {
        if (proj.is_package_project) continue;
        out << " " << proj.name;
    }
    for (const auto& cfg : configs) {
        for (const auto* proj : buildable_projects) {
            if (find_makefile_config_key(*proj, cfg, false)) {
                out << " " << make_project_config_target(*proj, cfg);
            }
        }
    }
    for (const auto& cfg : android_configs) {
        for (const auto* proj : buildable_projects) {
            if (find_makefile_config_key(*proj, cfg, true)) {
                out << " " << make_project_config_target(*proj, cfg, true);
            }
        }
    }
    out << "\n\n";

    // Default target. Solutions with only Android platforms default to the
    // Android build; otherwise desktop configs stay the default and Android is
    // built explicitly via `make android` / `make <Config>.Android`.
    if (!default_config.empty()) {
        out << "all: " << default_config << "\n\n";
    } else {
        out << "all: " << default_android_config << ".Android\n\n";
    }

    // Convenience alias for the default Android config
    if (!android_configs.empty()) {
        out << "android: " << default_android_config << ".Android\n\n";
    }

    // Per-configuration targets (e.g., make Debug, make Release, make Debug.Android)
    auto write_config_targets = [&](const std::set<std::string>& config_set, bool android) {
        for (const auto& cfg : config_set) {
            out << cfg << (android ? ".Android" : "") << ":";
            for (const auto* proj : buildable_projects) {
                if (find_makefile_config_key(*proj, cfg, android)) {
                    out << " " << make_project_config_target(*proj, cfg, android);
                }
            }
            out << "\n\n";

            for (const auto* proj : buildable_projects) {
                if (!find_makefile_config_key(*proj, cfg, android)) {
                    continue;
                }

                const std::string target = make_project_config_target(*proj, cfg, android);
                std::set<std::string> dependencies;
                for (const auto& dep : proj->project_references) {
                    if (const auto proj_key = find_makefile_config_key(*proj, cfg, android);
                        proj_key && !dependency_applies_to_config(dep, *proj_key)) {
                        continue;
                    }
                    const Project* dep_project = find_project(project_lookup, dep.name);
                    if (!dep_project || !find_makefile_config_key(*dep_project, cfg, android)) {
                        continue;
                    }
                    dependencies.insert(make_project_config_target(*dep_project, cfg, android));
                }

                out << target << ":";
                for (const auto& dep_target : dependencies) {
                    out << " " << dep_target;
                }
                out << "\n";
                out << "\t$(MAKE) -f " << target << "\n\n";
            }
        }
    };
    write_config_targets(configs, false);
    write_config_targets(android_configs, true);

    // Per-project targets (builds default config; falls back to the Android
    // default when the project only has Android configurations)
    for (const auto* proj : buildable_projects) {
        out << proj->name << ":";
        if (!default_config.empty() && find_makefile_config_key(*proj, default_config, false)) {
            out << " " << make_project_config_target(*proj, default_config);
        } else if (!default_android_config.empty() &&
                   find_makefile_config_key(*proj, default_android_config, true)) {
            out << " " << make_project_config_target(*proj, default_android_config, true);
        }
        out << "\n\n";
    }

    // Clean target
    out << "clean:\n";
    for (const auto* proj : buildable_projects) {
        for (const auto& cfg : configs) {
            if (find_makefile_config_key(*proj, cfg, false)) {
                out << "\t-$(MAKE) -f " << make_project_config_target(*proj, cfg) << " clean\n";
            }
        }
        for (const auto& cfg : android_configs) {
            if (find_makefile_config_key(*proj, cfg, true)) {
                out << "\t-$(MAKE) -f " << make_project_config_target(*proj, cfg, true) << " clean\n";
            }
        }
    }
    out << "\n";

    // Install target - install executable targets from Release (or default) config.
    // Android configs are excluded: their binaries target devices, not the host.
    if (!configs.empty()) {
        std::string install_config = configs.count("Release") ? "Release" : default_config;

        // Collect installable targets (executables and shared libraries)
        struct InstallTarget {
            std::string binary_path;  // Relative to build dir
            std::string name;         // Binary name for destination
        };
        std::vector<InstallTarget> exe_targets;
        std::vector<InstallTarget> lib_targets;

        for (const auto& proj : solution.projects) {
            if (proj.is_package_project) continue;
            // The configuration whose makefile is generated (Android device binaries are never
            // installed on the host).
            if (const auto install_key = find_makefile_config_key(proj, install_config, false)) {
                const std::string& config_key = *install_key;
                const Configuration& config = proj.configurations.at(config_key);
                const std::string& cfg_name = install_config;

                std::string target_name = config.target_name.empty() ? proj.name : config.target_name;
                std::string target_ext = config.target_ext;
                if (target_ext.empty()) {
                    if (config.config_type == "DynamicLibrary") {
#ifdef __APPLE__
                        target_ext = ".dylib";
#else
                        target_ext = ".so";
#endif
                    }
                } else if (config.config_type == "Application" && target_ext == ".exe") {
                    target_ext.clear();
                }
                const std::string binary_name = target_name + target_ext;
                std::string out_dir = config.out_dir.empty() ? "build/" + cfg_name
                                                             : anchor_variable_dir(config.out_dir, proj, config_key);
                // Make path relative to build dir
                fs::path out_path = fs::path(out_dir);
                std::string rel_out = out_path.is_absolute()
                    ? compute_relative_path(out_path.string(), build_dir)
                    : compute_relative_path((fs::path(output_dir) / out_dir).string(), build_dir);
                if (!rel_out.empty() && rel_out.back() != '/') rel_out += '/';

                if (config.config_type == "Application") {
                    exe_targets.push_back({rel_out + binary_name, binary_name});
                } else if (config.config_type == "DynamicLibrary") {
                    lib_targets.push_back({rel_out + binary_name, binary_name});
                }
            }
        }

        if (!exe_targets.empty() || !lib_targets.empty()) {
            out << "install: " << install_config << "\n";
            if (!exe_targets.empty()) {
                out << "\tinstall -d $(DESTDIR)$(PREFIX)/bin\n";
                for (const auto& t : exe_targets) {
                    out << "\tinstall -m 755 " << t.binary_path << " $(DESTDIR)$(PREFIX)/bin/" << t.name << "\n";
                }
            }
            if (!lib_targets.empty()) {
                out << "\tinstall -d $(DESTDIR)$(PREFIX)/lib\n";
                for (const auto& t : lib_targets) {
                    out << "\tinstall -m 755 " << t.binary_path << " $(DESTDIR)$(PREFIX)/lib/" << t.name << "\n";
                }
            }
            out << "\n";

            out << "uninstall:\n";
            for (const auto& t : exe_targets) {
                out << "\trm -f $(DESTDIR)$(PREFIX)/bin/" << t.name << "\n";
            }
            for (const auto& t : lib_targets) {
                out << "\trm -f $(DESTDIR)$(PREFIX)/lib/" << t.name << "\n";
            }
            out << "\n";
        }
    }

    std::cout << "Generated master Makefile: " << makefile_path << "\n";
    return true;
}

std::optional<OutputManifestScope> MakefileGenerator::output_manifest_scope(const std::string& output_dir) const {
    namespace fs = std::filesystem;
    OutputManifestScope scope;
    scope.directory = fs::path(output_dir.empty() ? "." : output_dir) / "build";
    scope.key = "makefile " + target_os_manifest_name();
    scope.probable_search_depth = 0;
    scope.probable_output = [](const fs::path& absolute, const std::string& relative) {
        auto ends_with = [&](const std::string& suffix) {
            return relative.size() >= suffix.size() &&
                relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        // Per-project Makefiles (<Project>.<Config>[.Android]) carry the sighmake header.
        if (ends_with(".AndroidManifest.xml") || ends_with(".runtime-dependencies.txt") ||
            relative == "write-sighmake-target-receipt.py" || relative == "Write-SighmakeTargetReceipt.ps1") {
            return true;
        }
        if (ends_with(VcxprojGenerator::csproj_suffix())) return true;
        return has_sighmake_header(absolute);
    };
    return scope;
}

// Register this generator with the factory
REGISTER_GENERATOR(MakefileGenerator, "makefile");

} // namespace vcxproj
