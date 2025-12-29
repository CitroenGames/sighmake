#include "toolset_registry.hpp"
#include <iostream>

namespace vcxproj {

ToolsetRegistry& ToolsetRegistry::instance() {
    static ToolsetRegistry registry;
    return registry;
}

ToolsetRegistry::ToolsetRegistry() : default_toolset_("v143") {
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

    // Year-to-toolset mappings for user convenience
    year_to_id_[2026] = "v145";  // Default to v145 for VS 2026
    year_to_id_[2022] = "v143";
    year_to_id_[2019] = "v142";
    year_to_id_[2017] = "v141";
    year_to_id_[2015] = "v140";
    year_to_id_[2013] = "v120";
    year_to_id_[2012] = "v110";
    year_to_id_[2010] = "v100";
}

std::optional<std::string> ToolsetRegistry::resolve(const std::string& input) const {
    if (input.empty()) {
        return default_toolset_;
    }

    // Try as direct toolset ID (e.g., "v143")
    if (toolsets_.count(input) > 0) {
        return input;
    }

    // Try as year (e.g., "2026")
    try {
        int year = std::stoi(input);
        auto it = year_to_id_.find(year);
        if (it != year_to_id_.end()) {
            return it->second;
        }
    } catch (const std::exception&) {
        // Not a valid integer, continue
    }

    // Unknown toolset - return as-is for forward compatibility
    // (allows users to use preview/unreleased toolsets)
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

} // namespace vcxproj
