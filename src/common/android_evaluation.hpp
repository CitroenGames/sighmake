#pragma once

#include "common/project_types.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace vcxproj {

// A buildscript is evaluated for one target OS (--target-os, default: the host), but a solution can
// also have Android configurations, which the Makefile generator writes as <Project>.<Config>.Android
// makefiles. Those must see the buildscript as Android sees it: if(linux)/if(macos)/if(windows) are
// false and !android is false for them, exactly as in a --target-os android generation. A desktop
// evaluation cannot provide that (its OS blocks apply to every configuration), so the generator
// evaluates the buildscript a second time for Android and takes the Android configurations, the
// Android-scoped sources and dependency edges, and the Android-only projects from that evaluation.

// Does the solution have, or did it declare before its `platforms` filters, an Android configuration?
inline bool solution_has_android_configurations(const Solution& solution) {
    if (solution.android_configurations_declared) return true;
    for (const auto& project : solution.projects) {
        for (const auto& [key, cfg] : project.configurations) {
            (void)cfg;
            if (is_android_platform(parse_config_key(key).second)) return true;
        }
    }
    return false;
}

namespace detail {

inline bool has_desktop_configuration(const Project& project) {
    for (const auto& [key, cfg] : project.configurations) {
        (void)cfg;
        if (!is_android_platform(parse_config_key(key).second)) return true;
    }
    return false;
}

inline bool has_android_configuration(const Project& project) {
    for (const auto& [key, cfg] : project.configurations) {
        (void)cfg;
        if (is_android_platform(parse_config_key(key).second)) return true;
    }
    return false;
}

inline void erase_android_configurations(Project& project) {
    for (auto it = project.configurations.begin(); it != project.configurations.end();) {
        if (is_android_platform(parse_config_key(it->first).second)) it = project.configurations.erase(it);
        else ++it;
    }
}

// A host declaration keeps its scope minus Android: an unscoped one becomes "every platform but Android".
inline void exclude_android(std::vector<std::string>& platforms) {
    if (!platform_list_selects(platforms, "Android")) return;
    platforms.push_back("!Android");
}

// An Android-evaluation declaration that applies to Android becomes Android-only; others are dropped.
template <typename T>
void append_android_scoped(std::vector<T>& target, const std::vector<T>& android_items) {
    for (const auto& item : android_items) {
        if (!platform_list_selects(item.platforms, "Android")) continue;
        T copy = item;
        copy.platforms = {"Android"};
        target.push_back(copy);
    }
}

}  // namespace detail

// Replace the Android part of `host` (a desktop evaluation) with that of `android` (an evaluation of
// the same buildscript for --target-os android). Desktop configurations and everything they use are
// unchanged. A project that exists only in one evaluation keeps only its own side.
inline void merge_android_evaluation(Solution& host, const Solution& android) {
    std::map<std::string, const Project*> android_projects;
    for (const auto& project : android.projects) android_projects[project.name] = &project;

    std::map<std::string, bool> in_host;
    for (auto& project : host.projects) {
        in_host[project.name] = true;
        if (project.is_package_project) continue;
        const auto it = android_projects.find(project.name);
        const Project* other = it == android_projects.end() ? nullptr : it->second;
        const bool android_side = other && !other->is_package_project && detail::has_android_configuration(*other);

        if (!detail::has_desktop_configuration(project)) {
            // Android-only in the host evaluation: the Android evaluation's project replaces it.
            if (android_side) {
                project = *other;
                for (auto cfg = project.configurations.begin(); cfg != project.configurations.end();) {
                    if (!is_android_platform(parse_config_key(cfg->first).second)) cfg = project.configurations.erase(cfg);
                    else ++cfg;
                }
            } else {
                detail::erase_android_configurations(project);
            }
            continue;
        }

        const bool had_android = detail::has_android_configuration(project);
        detail::erase_android_configurations(project);
        if (!had_android && !android_side) continue;
        for (auto& source : project.sources) detail::exclude_android(source.platforms);
        for (auto& dep : project.project_references) detail::exclude_android(dep.platforms);
        if (!android_side) continue;
        for (const auto& [key, cfg] : other->configurations) {
            if (is_android_platform(parse_config_key(key).second)) project.configurations[key] = cfg;
        }
        detail::append_android_scoped(project.sources, other->sources);
        detail::append_android_scoped(project.project_references, other->project_references);
    }

    // Projects that only the Android evaluation declares (or keeps: e.g. a `platforms` filter in an
    // if(linux) block) are added with their Android configurations.
    for (const auto& project : android.projects) {
        if (in_host.count(project.name) || project.is_package_project) continue;
        if (!detail::has_android_configuration(project)) continue;
        Project copy = project;
        for (auto cfg = copy.configurations.begin(); cfg != copy.configurations.end();) {
            if (!is_android_platform(parse_config_key(cfg->first).second)) cfg = copy.configurations.erase(cfg);
            else ++cfg;
        }
        host.projects.push_back(copy);
    }

    // From here on the Android configurations are the ones present (the merge consumed the declaration).
    host.android_configurations_declared = false;

    // The Android platform is listed when the Android evaluation has one (the build cache reads it).
    for (const auto& platform : android.platforms) {
        if (is_android_platform(platform) &&
            std::find(host.platforms.begin(), host.platforms.end(), platform) == host.platforms.end()) {
            host.platforms.push_back(platform);
        }
    }
}

}  // namespace vcxproj
