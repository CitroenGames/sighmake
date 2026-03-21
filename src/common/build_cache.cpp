#include "pch.h"
#include "build_cache.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

static std::string join(const std::vector<std::string>& vec, const std::string& sep) {
    std::string result;
    for (size_t i = 0; i < vec.size(); i++) {
        if (i > 0) result += sep;
        result += vec[i];
    }
    return result;
}

static std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> result;
    std::string current;
    for (char c : str) {
        if (c == delim) {
            if (!current.empty()) {
                result.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

bool BuildCache::write(const std::string& output_dir) const {
    fs::path cache_path = fs::path(output_dir) / CACHE_FILENAME;

    std::ofstream out(cache_path);
    if (!out) {
        std::cerr << "Warning: Failed to write build cache: " << cache_path << "\n";
        return false;
    }

    out << "# sighmake build cache - auto-generated, do not edit\n";
    out << "generator=" << generator << "\n";
    out << "solution_name=" << solution_name << "\n";
    out << "configurations=" << join(configurations, ",") << "\n";
    out << "platforms=" << join(platforms, ",") << "\n";

    if (!solution_file.empty()) {
        out << "solution_file=" << solution_file << "\n";
    }
    if (!vs_installation_path.empty()) {
        out << "vs_installation_path=" << vs_installation_path << "\n";
    }
    if (vs_year > 0) {
        out << "vs_year=" << std::to_string(vs_year) << "\n";
    }
    if (!platform_toolset.empty()) {
        out << "platform_toolset=" << platform_toolset << "\n";
    }
    if (!build_dir.empty()) {
        out << "build_dir=" << build_dir << "\n";
    }

    return true;
}

std::optional<BuildCache> BuildCache::read(const std::string& dir) {
    fs::path cache_path = fs::path(dir) / CACHE_FILENAME;

    std::ifstream in(cache_path);
    if (!in) {
        return std::nullopt;
    }

    BuildCache cache;
    std::string line;
    while (std::getline(in, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "generator") {
            cache.generator = value;
        } else if (key == "solution_file") {
            cache.solution_file = value;
        } else if (key == "solution_name") {
            cache.solution_name = value;
        } else if (key == "vs_installation_path") {
            cache.vs_installation_path = value;
        } else if (key == "vs_year") {
            cache.vs_year = std::stoi(value);
        } else if (key == "platform_toolset") {
            cache.platform_toolset = value;
        } else if (key == "configurations") {
            cache.configurations = split(value, ',');
        } else if (key == "platforms") {
            cache.platforms = split(value, ',');
        } else if (key == "build_dir") {
            cache.build_dir = value;
        }
    }

    if (cache.generator.empty()) {
        return std::nullopt;
    }

    return cache;
}

} // namespace vcxproj
