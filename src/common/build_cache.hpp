#pragma once

#include <string>
#include <vector>
#include <optional>

namespace vcxproj {

struct BuildCache {
    std::string generator;              // "vcxproj" or "makefile"
    std::string solution_file;          // Relative path to .sln/.slnx (vcxproj only)
    std::string solution_name;
    std::string vs_installation_path;   // (vcxproj only)
    int vs_year = 0;                    // (vcxproj only)
    std::string platform_toolset;       // (vcxproj only)
    std::vector<std::string> configurations;
    std::vector<std::string> platforms;
    std::string build_dir;              // Relative path to makefile build dir (makefile only)

    // Write cache to output_dir/.sighmake_cache
    bool write(const std::string& output_dir) const;

    // Read cache from dir/.sighmake_cache
    static std::optional<BuildCache> read(const std::string& dir);

    static constexpr const char* CACHE_FILENAME = ".sighmake_cache";
};

} // namespace vcxproj
