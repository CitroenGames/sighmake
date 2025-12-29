#pragma once
#include <string>
#include <map>
#include <optional>

namespace vcxproj {

// Registry for MSVC toolset version information
// Provides centralized mapping between user-friendly version names (years)
// and platform toolset identifiers (e.g., "v143")
class ToolsetRegistry {
public:
    struct ToolsetInfo {
        std::string id;           // Toolset ID (e.g., "v143")
        std::string vs_version;   // Visual Studio version (e.g., "Visual Studio 2022")
        int year;                 // Release year (e.g., 2022)
        bool is_legacy;           // True for older/unsupported versions
    };

    // Get singleton instance
    static ToolsetRegistry& instance();

    // Resolve user input (year or toolset ID) to canonical toolset ID
    // Returns std::nullopt if input is empty, otherwise returns best match
    // Examples:
    //   "2026" -> "v145"
    //   "v143" -> "v143"
    //   "v999" -> "v999" (unknown but allowed for forward compatibility)
    std::optional<std::string> resolve(const std::string& input) const;

    // Get detailed information about a toolset
    // Returns std::nullopt if toolset is unknown
    std::optional<ToolsetInfo> get_info(const std::string& toolset_id) const;

    // Check if a toolset is recognized
    bool is_known(const std::string& toolset_id) const;

    // Get current default toolset
    std::string get_default() const;

    // Set default toolset (used by CLI and environment variable)
    void set_default(const std::string& toolset);

private:
    ToolsetRegistry();
    ToolsetRegistry(const ToolsetRegistry&) = delete;
    ToolsetRegistry& operator=(const ToolsetRegistry&) = delete;

    std::map<std::string, ToolsetInfo> toolsets_;      // toolset_id -> info
    std::map<int, std::string> year_to_id_;           // year -> toolset_id
    std::string default_toolset_;
};

} // namespace vcxproj
