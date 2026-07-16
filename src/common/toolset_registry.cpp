#include "pch.h"
#include "toolset_registry.hpp"
#include "string_utils.hpp"

namespace vcxproj {

namespace {

// Single data table for all known toolsets, newest first. Add new toolsets
// here — the registry derives every lookup (id, year, alias) from this list.
struct ToolsetDef {
    const char* id;           // "v143"
    const char* vs_version;   // "Visual Studio 2022"
    int year;                 // 2022
    bool legacy;              // True for older/unsupported versions
    const char* alias;        // Normalized toolchain name ("msvc2022"), nullptr = none
    bool canonical_for_year;  // Resolves year -> id (at most one per year)
};

// Visual Studio 2026: Microsoft hasn't announced the toolset number yet, so
// both v144 and v145 are supported, with v145 as the canonical choice.
// Future: other toolchains (e.g., {"gcc-13", ..., "gcc13", ...}).
const ToolsetDef kToolsetDefs[] = {
    {"v145",    "Visual Studio 2026",             2026, false, "msvc2026",   true},
    {"v144",    "Visual Studio 2026 (alternate)", 2026, false, nullptr,      false},
    {"v143",    "Visual Studio 2022",             2022, false, "msvc2022",   true},
    {"v142",    "Visual Studio 2019",             2019, false, "msvc2019",   true},
    {"v141",    "Visual Studio 2017",             2017, false, "msvc2017",   true},
    {"v140",    "Visual Studio 2015",             2015, false, "msvc2015",   true},
    {"v120",    "Visual Studio 2013",             2013, true,  "msvc2013",   true},
    {"v110",    "Visual Studio 2012",             2012, true,  "msvc2012",   true},
    {"v110_xp", "Visual Studio 2012 (XP)",        2012, true,  "msvc2012xp", false},
    {"v100",    "Visual Studio 2010",             2010, true,  "msvc2010",   true},
};

} // namespace

ToolsetRegistry& ToolsetRegistry::instance() {
    static ToolsetRegistry registry;
    return registry;
}

ToolsetRegistry::ToolsetRegistry() : default_toolset_("") {
    for (const auto& def : kToolsetDefs) {
        toolsets_[def.id] = {def.id, def.vs_version, def.year, def.legacy};
        if (def.canonical_for_year) {
            year_to_id_[def.year] = def.id;
        }
        if (def.alias) {
            toolchain_to_toolset_[def.alias] = def.id;
        }
    }
}

std::optional<std::string> ToolsetRegistry::resolve(const std::string& input) const {
    if (input.empty()) {
        return default_toolset_;
    }

    // Convert to lowercase for case-insensitive matching
    std::string lower_input = to_lower(input);

    // Look up normalized toolchain name (e.g., "msvc2022" -> "v143")
    auto it = toolchain_to_toolset_.find(lower_input);
    if (it != toolchain_to_toolset_.end()) {
        return it->second;
    }

    // Unknown toolset - return as-is for forward compatibility
    return input;
}

std::optional<ToolsetRegistry::ToolsetInfo> ToolsetRegistry::get_info(const std::string& toolset_id) const {
    auto it = toolsets_.find(toolset_id);
    if (it != toolsets_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool ToolsetRegistry::is_known(const std::string& toolset_id) const {
    return toolsets_.count(toolset_id) > 0;
}

std::string ToolsetRegistry::get_default() const {
    return default_toolset_;
}

void ToolsetRegistry::set_default(const std::string& toolset) {
    default_toolset_ = toolset;
}

int ToolsetRegistry::get_toolset_year(const std::string& toolset) const {
    auto it = toolsets_.find(toolset);
    return it != toolsets_.end() ? it->second.year : 0;
}

std::optional<std::string> ToolsetRegistry::toolset_for_year(int year) const {
    auto it = year_to_id_.find(year);
    if (it != year_to_id_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<ToolsetRegistry::ToolchainAlias> ToolsetRegistry::toolchain_aliases() const {
    std::vector<ToolchainAlias> aliases;
    for (const auto& def : kToolsetDefs) {
        if (def.alias) {
            aliases.push_back({def.alias, def.id, def.vs_version});
        }
    }
    return aliases;
}

} // namespace vcxproj
