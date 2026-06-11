#include "pch.h"
#include "toolset_registry.hpp"

namespace vcxproj {

ToolsetRegistry& ToolsetRegistry::instance() {
    static ToolsetRegistry registry;
    return registry;
}

ToolsetRegistry::ToolsetRegistry() : default_toolset_("") {
    // Modern toolsets (officially released)
    toolsets_["v143"] = {"v143", "Visual Studio 2022", 2022, false};
    toolsets_["v142"] = {"v142", "Visual Studio 2019", 2019, false};
    toolsets_["v141"] = {"v141", "Visual Studio 2017", 2017, false};
    toolsets_["v140"] = {"v140", "Visual Studio 2015", 2015, false};

    // Future: Visual Studio 2026 (Microsoft hasn't announced the toolset number yet)
    // Supporting both v144 and v145 for flexibility
    toolsets_["v145"] = {"v145", "Visual Studio 2026", 2026, false};
    toolsets_["v144"] = {"v144", "Visual Studio 2026 (alternate)", 2026, false};

    // Legacy toolsets (older versions)
    toolsets_["v120"] = {"v120", "Visual Studio 2013", 2013, true};
    toolsets_["v110"] = {"v110", "Visual Studio 2012", 2012, true};
    toolsets_["v100"] = {"v100", "Visual Studio 2010", 2010, true};

    // Windows XP targeting toolsets
    toolsets_["v110_xp"] = {"v110_xp", "Visual Studio 2012 (XP)", 2012, true};

    // Year-to-toolset mappings for user convenience
    year_to_id_[2026] = "v145";  // Default to v145 for VS 2026
    year_to_id_[2022] = "v143";
    year_to_id_[2019] = "v142";
    year_to_id_[2017] = "v141";
    year_to_id_[2015] = "v140";
    year_to_id_[2013] = "v120";
    year_to_id_[2012] = "v110";
    year_to_id_[2010] = "v100";

    // Normalized toolchain name mappings
    toolchain_to_toolset_["msvc2026"] = "v145";
    toolchain_to_toolset_["msvc2022"] = "v143";
    toolchain_to_toolset_["msvc2019"] = "v142";
    toolchain_to_toolset_["msvc2017"] = "v141";
    toolchain_to_toolset_["msvc2015"] = "v140";
    toolchain_to_toolset_["msvc2013"] = "v120";
    toolchain_to_toolset_["msvc2012"] = "v110";
    toolchain_to_toolset_["msvc2010"] = "v100";

    // XP toolset mappings
    toolchain_to_toolset_["msvc2012xp"] = "v110_xp";

    // Future: other toolchains
    // toolchain_to_toolset_["gcc13"] = "gcc-13";
    // toolchain_to_toolset_["clang16"] = "clang-16";
}

std::optional<std::string> ToolsetRegistry::resolve(const std::string& input) const {
    if (input.empty()) {
        return default_toolset_;
    }

    // Convert to lowercase for case-insensitive matching
    std::string lower_input = input;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });

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

} // namespace vcxproj
