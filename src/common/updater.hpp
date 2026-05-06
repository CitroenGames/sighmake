#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace vcxproj {
namespace updater {

struct ReleaseAsset {
    std::string platform_key;
    std::string name;
    std::string sha256;
};

struct ReleaseManifest {
    std::string version;
    std::string tag;
    std::string asset_base_url;
    std::vector<ReleaseAsset> assets;
};

struct UpdateOptions {
    std::string current_version;
    std::string repository;
    std::string executable_path;
    bool check_only = false;
    bool force = false;
    std::ostream* out = nullptr;
    std::ostream* err = nullptr;
};

std::string normalize_version(std::string version);
int compare_versions(const std::string& lhs, const std::string& rhs);

std::optional<ReleaseManifest> parse_release_manifest(const std::string& text,
                                                       std::string* error = nullptr);
const ReleaseAsset* find_asset(const ReleaseManifest& manifest,
                               const std::string& platform_key);

std::string current_platform_key();
std::string current_executable_path(const char* argv0);

bool verify_file_sha256(const std::string& path,
                        const std::string& expected_sha256,
                        std::string* error = nullptr);

int run_update(const UpdateOptions& options);

} // namespace updater
} // namespace vcxproj
