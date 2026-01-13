#include "pch.h"
#include "vs_detector.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

// Helper: Execute command and get output
std::string VSDetector::execute_command(const std::string& command) {
#ifdef _WIN32
#ifndef NDEBUG
    std::cout << "[DEBUG] Executing command: " << command << "\n";
#endif

    std::array<char, 256> buffer;
    std::string result;

    // Use _popen to execute command and read output
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) {
#ifndef NDEBUG
        std::cout << "[DEBUG] Failed to execute command\n";
#endif
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    _pclose(pipe);

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

#ifndef NDEBUG
    std::cout << "[DEBUG] Command output: '" << result << "'\n";
#endif

    return result;
#else
    (void)command;
    return "";
#endif
}

// Helper: Parse vswhere version to year
int VSDetector::version_to_year(const std::string& version) {
    if (version.empty()) {
        return 0;
    }

    // Extract major version (first number before '.')
    size_t dot_pos = version.find('.');
    if (dot_pos == std::string::npos) {
        return 0;
    }

    try {
        int major = std::stoi(version.substr(0, dot_pos));

        // Map major version to year
        switch (major) {
            case 18: return 2026;  // VS 2026
            case 17: return 2022;  // VS 2022
            case 16: return 2019;  // VS 2019
            case 15: return 2017;  // VS 2017
            default: return 0;
        }
    } catch (...) {
        return 0;
    }
}

// Helper: Year to toolset mapping
std::string VSDetector::year_to_toolset(int year) {
    switch (year) {
        case 2026: return "v145";
        case 2022: return "v143";
        case 2019: return "v142";
        case 2017: return "v141";
        case 2015: return "v140";
        case 2013: return "v120";
        case 2012: return "v110";
        case 2010: return "v100";
        default: return "v143";  // Safe default
    }
}

// Detection via vswhere.exe (VS 2017+)
std::optional<VSInstallation> VSDetector::detect_via_vswhere() {
#ifdef _WIN32
#ifndef NDEBUG
    std::cout << "[DEBUG] Attempting VS detection via vswhere.exe\n";
#endif

    // Get ProgramFiles(x86) path
    const char* prog_files_x86 = std::getenv("ProgramFiles(x86)");
    if (!prog_files_x86) {
#ifndef NDEBUG
        std::cout << "[DEBUG] ProgramFiles(x86) environment variable not found\n";
#endif
        return std::nullopt;
    }

    // Build path to vswhere.exe
    fs::path vswhere_path = fs::path(prog_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";

#ifndef NDEBUG
    std::cout << "[DEBUG] Looking for vswhere at: " << vswhere_path << "\n";
#endif

    // Check if vswhere.exe exists
    if (!fs::exists(vswhere_path)) {
#ifndef NDEBUG
        std::cout << "[DEBUG] vswhere.exe not found\n";
#endif
        return std::nullopt;
    }

#ifndef NDEBUG
    std::cout << "[DEBUG] vswhere.exe found, querying version...\n";
#endif

    // Execute vswhere to get latest VS version
    std::string command = "\"" + vswhere_path.string() + "\" -latest -property installationVersion";
    std::string version_output = execute_command(command);

    if (version_output.empty()) {
        return std::nullopt;
    }

    // Parse version to year
    int year = version_to_year(version_output);
#ifndef NDEBUG
    std::cout << "[DEBUG] Parsed version: " << version_output << " -> year: " << year << "\n";
#endif

    if (year == 0) {
#ifndef NDEBUG
        std::cout << "[DEBUG] Could not parse year from version\n";
#endif
        return std::nullopt;
    }

    // Get installation path
    command = "\"" + vswhere_path.string() + "\" -latest -property installationPath";
    std::string install_path = execute_command(command);

    // Create VSInstallation struct
    VSInstallation vs_info;
    vs_info.is_installed = true;
    vs_info.year = year;
    vs_info.version = version_output;
    vs_info.installation_path = install_path;
    vs_info.platform_toolset = year_to_toolset(year);

#ifndef NDEBUG
    std::cout << "[DEBUG] Detected VS via vswhere: " << year << " (toolset " << vs_info.platform_toolset << ")\n";
#endif

    return vs_info;
#else
    return std::nullopt;
#endif
}

// Detection via Windows Registry (VS 2015 and older)
std::optional<VSInstallation> VSDetector::detect_via_registry() {
#ifdef _WIN32
#ifndef NDEBUG
    std::cout << "[DEBUG] Attempting VS detection via Windows Registry\n";
#endif

    // Registry versions to check (VS 2015 down to VS 2010)
    const std::vector<std::pair<std::string, int>> versions = {
        {"14.0", 2015},
        {"12.0", 2013},
        {"11.0", 2012},
        {"10.0", 2010}
    };

    for (const auto& [ver_key, year] : versions) {
        // Try 64-bit registry first
        std::string subkey = "SOFTWARE\\Microsoft\\VisualStudio\\" + ver_key + "\\Setup\\VS";
        HKEY hKey;

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            // Check if installed
            DWORD installed = 0;
            DWORD size = sizeof(DWORD);
            if (RegQueryValueExA(hKey, "Install", NULL, NULL, (LPBYTE)&installed, &size) == ERROR_SUCCESS && installed == 1) {
                // Get product directory
                char path[MAX_PATH];
                size = sizeof(path);
                if (RegQueryValueExA(hKey, "ProductDir", NULL, NULL, (LPBYTE)path, &size) == ERROR_SUCCESS) {
                    VSInstallation vs_info;
                    vs_info.is_installed = true;
                    vs_info.year = year;
                    vs_info.version = ver_key;
                    vs_info.installation_path = path;
                    vs_info.platform_toolset = year_to_toolset(year);

#ifndef NDEBUG
                    std::cout << "[DEBUG] Detected VS via registry: " << year << " (toolset " << vs_info.platform_toolset << ")\n";
#endif

                    RegCloseKey(hKey);
                    return vs_info;
                }
            }
            RegCloseKey(hKey);
        }

        // Try 32-bit registry (WOW6432Node)
        subkey = "SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\" + ver_key + "\\Setup\\VS";
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD installed = 0;
            DWORD size = sizeof(DWORD);
            if (RegQueryValueExA(hKey, "Install", NULL, NULL, (LPBYTE)&installed, &size) == ERROR_SUCCESS && installed == 1) {
                char path[MAX_PATH];
                size = sizeof(path);
                if (RegQueryValueExA(hKey, "ProductDir", NULL, NULL, (LPBYTE)path, &size) == ERROR_SUCCESS) {
                    VSInstallation vs_info;
                    vs_info.is_installed = true;
                    vs_info.year = year;
                    vs_info.version = ver_key;
                    vs_info.installation_path = path;
                    vs_info.platform_toolset = year_to_toolset(year);

#ifndef NDEBUG
                    std::cout << "[DEBUG] Detected VS via registry: " << year << " (toolset " << vs_info.platform_toolset << ")\n";
#endif

                    RegCloseKey(hKey);
                    return vs_info;
                }
            }
            RegCloseKey(hKey);
        }
    }

#ifndef NDEBUG
    std::cout << "[DEBUG] No VS found in registry\n";
#endif

    return std::nullopt;
#else
    return std::nullopt;
#endif
}

// Detect latest VS installation
std::optional<VSInstallation> VSDetector::detect_latest_vs() {
#ifndef NDEBUG
    std::cout << "[DEBUG] ========== VS Detection Start ==========\n";
#endif

    // Try vswhere first (VS 2017+)
    auto vs_info = detect_via_vswhere();
    if (vs_info.has_value()) {
#ifndef NDEBUG
        std::cout << "[DEBUG] VS detected successfully via vswhere\n";
        std::cout << "[DEBUG] ========== VS Detection End ==========\n";
#endif
        return vs_info;
    }

#ifndef NDEBUG
    std::cout << "[DEBUG] vswhere detection failed, trying registry...\n";
#endif

    // Fall back to registry (VS 2015 and older)
    auto registry_result = detect_via_registry();
#ifndef NDEBUG
    if (registry_result.has_value()) {
        std::cout << "[DEBUG] VS detected successfully via registry\n";
    } else {
        std::cout << "[DEBUG] No VS detected via any method\n";
    }
    std::cout << "[DEBUG] ========== VS Detection End ==========\n";
#endif

    return registry_result;
}

} // namespace vcxproj
