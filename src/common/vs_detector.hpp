#pragma once
#include <string>
#include <optional>

namespace vcxproj {

struct VSInstallation {
    bool is_installed = false;
    int year = 0;                    // 2026, 2022, 2019, etc.
    std::string version;             // "17.11.5"
    std::string installation_path;   // Installation directory
    std::string platform_toolset;    // v145, v143, v142, etc.
};

class VSDetector {
public:
    // Detect latest VS installation
    static std::optional<VSInstallation> detect_latest_vs();

private:
    // Detection via vswhere.exe (VS 2017+)
    static std::optional<VSInstallation> detect_via_vswhere();

    // Detection via Windows Registry (VS 2015 and older)
    static std::optional<VSInstallation> detect_via_registry();

    // Helper: Execute command and get output
    static std::string execute_command(const std::string& command);

    // Helper: Parse vswhere version to year
    static int version_to_year(const std::string& version);

    // Helper: Year to toolset mapping
    static std::string year_to_toolset(int year);
};

} // namespace vcxproj
