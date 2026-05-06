#include "pch.h"
#include "updater.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace vcxproj {
namespace updater {
namespace {

std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string popen_read(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        return "";
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') {
            result += "\\\"";
        } else {
            result += c;
        }
    }
    result += "\"";
    return result;
#else
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
#endif
}

std::string powershell_quote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

std::vector<int> parse_version_numbers(const std::string& version) {
    std::string normalized = normalize_version(version);
    std::vector<int> parts;
    std::string current;

    for (char c : normalized) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            current += c;
        } else if (c == '.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else {
            break;
        }
    }

    if (!current.empty() || parts.empty()) {
        parts.push_back(current.empty() ? 0 : std::stoi(current));
    }

    while (parts.size() < 3) {
        parts.push_back(0);
    }

    return parts;
}

fs::path unique_temp_dir() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() /
        ("sighmake-update-" + std::to_string(now) + "-" + std::to_string(rd()));
    fs::create_directories(dir);
    return dir;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wide[0], size);
    return wide;
}

bool parse_https_url(const std::string& url, std::wstring& host, std::wstring& path) {
    const std::string prefix = "https://";
    if (!starts_with(url, prefix)) {
        return false;
    }

    size_t host_start = prefix.size();
    size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos || path_start == host_start) {
        return false;
    }

    host = utf8_to_wide(url.substr(host_start, path_start - host_start));
    path = utf8_to_wide(url.substr(path_start));
    return true;
}

bool download_url_to_file_windows(const std::string& url,
                                  const std::string& output_path,
                                  std::string* error) {
    std::wstring host;
    std::wstring path;
    if (!parse_https_url(url, host, path)) {
        if (error) *error = "Only https:// URLs are supported";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"sighmake updater/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = "WinHttpOpen failed";
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        if (error) *error = "WinHttpConnect failed";
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if (error) *error = "WinHttpOpenRequest failed";
        return false;
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (ok) {
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status_code,
                            &status_size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status_code < 200 || status_code >= 300) {
            ok = FALSE;
        }
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        ok = FALSE;
        if (error) *error = "Failed to open output file: " + output_path;
    }

    if (ok) {
        DWORD available = 0;
        do {
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                ok = FALSE;
                break;
            }
            if (available == 0) {
                break;
            }

            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read)) {
                ok = FALSE;
                break;
            }
            out.write(buffer.data(), read);
            if (!out) {
                ok = FALSE;
                if (error) *error = "Failed while writing output file";
                break;
            }
        } while (available > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok && error && error->empty()) {
        *error = status_code ? "HTTP download failed with status " + std::to_string(status_code)
                             : "HTTP download failed";
    }

    return ok == TRUE;
}
#endif

bool download_url_to_file(const std::string& url,
                          const std::string& output_path,
                          std::string* error) {
#ifdef _WIN32
    return download_url_to_file_windows(url, output_path, error);
#else
    std::string curl_cmd = "curl -fL --retry 3 -o " + shell_quote(output_path) + " " + shell_quote(url);
    int curl_status = std::system(curl_cmd.c_str());
    if (curl_status == 0) {
        return true;
    }

    std::string wget_cmd = "wget -O " + shell_quote(output_path) + " " + shell_quote(url);
    int wget_status = std::system(wget_cmd.c_str());
    if (wget_status == 0) {
        return true;
    }

    if (error) {
        *error = "curl and wget failed to download " + url;
    }
    return false;
#endif
}

std::optional<std::string> download_url_to_string(const std::string& url, std::string* error) {
    fs::path temp_dir = unique_temp_dir();
    fs::path temp_file = temp_dir / "download.txt";
    if (!download_url_to_file(url, temp_file.string(), error)) {
        return std::nullopt;
    }

    std::ifstream in(temp_file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    return ss.str();
}

std::string release_download_base(const std::string& repository) {
    return "https://github.com/" + repository + "/releases/latest/download";
}

std::string manifest_url_for_repository(const std::string& repository) {
    if (const char* override_url = std::getenv("SIGHMAKE_UPDATE_MANIFEST_URL")) {
        if (*override_url) {
            return override_url;
        }
    }
    return release_download_base(repository) + "/sighmake-release.ini";
}

std::string command_sha256(const std::string& path, std::string* error) {
#ifdef _WIN32
    std::string output = popen_read("certutil -hashfile " + shell_quote(path) + " SHA256 2>nul");
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::string candidate;
        for (char c : line) {
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                candidate += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (candidate.size() == 64) {
            return candidate;
        }
    }
#else
    std::string output = popen_read("sha256sum " + shell_quote(path) + " 2>/dev/null");
    if (output.empty()) {
        output = popen_read("shasum -a 256 " + shell_quote(path) + " 2>/dev/null");
    }

    std::istringstream iss(output);
    std::string first;
    iss >> first;
    first = to_lower(first);
    if (first.size() == 64) {
        return first;
    }
#endif

    if (error) {
        *error = "Could not compute SHA-256 for " + path;
    }
    return "";
}

bool extract_archive(const std::string& archive_path,
                     const std::string& output_dir,
                     std::string* error) {
#ifdef _WIN32
    std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -Command "
        "\"Expand-Archive -LiteralPath " + powershell_quote(archive_path) +
        " -DestinationPath " + powershell_quote(output_dir) + " -Force\"";
#else
    std::string command = "tar -xzf " + shell_quote(archive_path) + " -C " + shell_quote(output_dir);
#endif

    int result = std::system(command.c_str());
    if (result != 0 && error) {
        *error = "Failed to extract release archive";
    }
    return result == 0;
}

std::optional<fs::path> find_released_binary(const fs::path& dir) {
#ifdef _WIN32
    const std::string expected = "sighmake.exe";
#else
    const std::string expected = "sighmake";
#endif

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (to_lower(entry.path().filename().string()) == expected) {
            return entry.path();
        }
    }

    return std::nullopt;
}

#ifdef _WIN32
bool schedule_windows_replace(const fs::path& source,
                              const fs::path& target,
                              std::string* error) {
    fs::path script = source.parent_path() / "finish-update.cmd";
    fs::path backup = target;
    backup += ".old";

    std::ofstream out(script);
    if (!out) {
        if (error) *error = "Failed to write update handoff script";
        return false;
    }

    out << "@echo off\n";
    out << "set \"SRC=" << source.string() << "\"\n";
    out << "set \"DST=" << target.string() << "\"\n";
    out << "set \"BAK=" << backup.string() << "\"\n";
    out << "ping 127.0.0.1 -n 3 >nul\n";
    out << "if exist \"%BAK%\" del /q \"%BAK%\" >nul 2>nul\n";
    out << "if exist \"%DST%\" move /y \"%DST%\" \"%BAK%\" >nul\n";
    out << "move /y \"%SRC%\" \"%DST%\" >nul\n";
    out << "if errorlevel 1 (\n";
    out << "  if exist \"%BAK%\" move /y \"%BAK%\" \"%DST%\" >nul\n";
    out << "  exit /b 1\n";
    out << ")\n";
    out << "if exist \"%BAK%\" del /q \"%BAK%\" >nul 2>nul\n";
    out << "del /q \"%~f0\" >nul 2>nul\n";
    out.close();

    std::string command = "cmd.exe /c \"" + script.string() + "\"";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW | DETACHED_PROCESS,
                                  nullptr, nullptr, &startup, &process);
    if (!created) {
        if (error) *error = "Failed to start update handoff process";
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool replace_unix_binary(const fs::path& source,
                         const fs::path& target,
                         std::string* error) {
    std::error_code ec;
    fs::permissions(source,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);

    fs::path backup = target;
    backup += ".old";
    fs::remove(backup, ec);
    fs::rename(target, backup, ec);
    if (ec) {
        if (error) *error = "Failed to move existing executable: " + ec.message();
        return false;
    }

    fs::rename(source, target, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(backup, target, restore_ec);
        if (error) *error = "Failed to install new executable: " + ec.message();
        return false;
    }

    fs::remove(backup, ec);
    return true;
}
#endif

} // namespace

std::string normalize_version(std::string version) {
    version = trim(version);
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        version.erase(version.begin());
    }
    return version;
}

int compare_versions(const std::string& lhs, const std::string& rhs) {
    auto left = parse_version_numbers(lhs);
    auto right = parse_version_numbers(rhs);
    size_t count = left.size() > right.size() ? left.size() : right.size();
    left.resize(count, 0);
    right.resize(count, 0);

    for (size_t i = 0; i < count; ++i) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
    }
    return 0;
}

std::optional<ReleaseManifest> parse_release_manifest(const std::string& text,
                                                       std::string* error) {
    ReleaseManifest manifest;
    std::map<std::string, std::string> values;
    std::istringstream input(text);
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "Invalid manifest line " + std::to_string(line_number);
            return std::nullopt;
        }

        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    manifest.version = values["version"];
    manifest.tag = values["tag"];
    manifest.asset_base_url = values["asset_base_url"];

    if (manifest.version.empty()) {
        if (error) *error = "Manifest is missing version";
        return std::nullopt;
    }
    if (manifest.tag.empty()) {
        manifest.tag = "v" + normalize_version(manifest.version);
    }

    for (const auto& [key, value] : values) {
        if (key.size() > 7 && key.substr(key.size() - 7) == "_sha256") {
            continue;
        }
        if (key == "version" || key == "tag" || key == "asset_base_url") {
            continue;
        }
        if (value.empty()) {
            continue;
        }

        ReleaseAsset asset;
        asset.platform_key = key;
        asset.name = value;
        auto sha_it = values.find(key + "_sha256");
        if (sha_it != values.end()) {
            asset.sha256 = to_lower(sha_it->second);
        }
        manifest.assets.push_back(asset);
    }

    if (manifest.assets.empty()) {
        if (error) *error = "Manifest does not list any release assets";
        return std::nullopt;
    }

    return manifest;
}

const ReleaseAsset* find_asset(const ReleaseManifest& manifest,
                               const std::string& platform_key) {
    for (const auto& asset : manifest.assets) {
        if (asset.platform_key == platform_key) {
            return &asset;
        }
    }
    return nullptr;
}

std::string current_platform_key() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "windows_x64";
#elif defined(_M_ARM64)
    return "windows_arm64";
#else
    return "windows_x86";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos_arm64";
#else
    return "macos_x64";
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    return "linux_x64";
#elif defined(__aarch64__)
    return "linux_arm64";
#else
    return "linux_unknown";
#endif
#else
    return "unknown";
#endif
}

std::string current_executable_path(const char* argv0) {
#ifdef _WIN32
    std::vector<char> buffer(MAX_PATH);
    DWORD size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size > 0) {
        return fs::absolute(std::string(buffer.data(), size)).string();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return fs::weakly_canonical(buffer.data()).string();
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size > 0) {
        buffer[static_cast<size_t>(size)] = '\0';
        return fs::weakly_canonical(buffer.data()).string();
    }
#endif
    if (argv0 && *argv0) {
        return fs::absolute(argv0).string();
    }
    return "";
}

bool verify_file_sha256(const std::string& path,
                        const std::string& expected_sha256,
                        std::string* error) {
    std::string expected = to_lower(trim(expected_sha256));
    if (expected.empty()) {
        if (error) *error = "Missing expected SHA-256";
        return false;
    }

    std::string actual = command_sha256(path, error);
    if (actual.empty()) {
        return false;
    }

    if (actual != expected) {
        if (error) {
            *error = "SHA-256 mismatch for " + path + ": expected " + expected + ", got " + actual;
        }
        return false;
    }

    return true;
}

int run_update(const UpdateOptions& options) {
    std::ostream& out = options.out ? *options.out : std::cout;
    std::ostream& err = options.err ? *options.err : std::cerr;

    if (options.repository.empty()) {
        err << "Error: release repository is not configured\n";
        return 1;
    }

    std::string manifest_url = manifest_url_for_repository(options.repository);
    out << "Checking " << manifest_url << "\n";

    std::string error;
    auto manifest_text = download_url_to_string(manifest_url, &error);
    if (!manifest_text) {
        err << "Error: " << error << "\n";
        return 1;
    }

    auto manifest = parse_release_manifest(*manifest_text, &error);
    if (!manifest) {
        err << "Error: " << error << "\n";
        return 1;
    }

    std::string current = normalize_version(options.current_version);
    std::string latest = normalize_version(manifest->version);
    int cmp = compare_versions(current, latest);

    if (cmp >= 0 && !options.force) {
        out << "sighmake is up to date (" << options.current_version << ").\n";
        return 0;
    }

    out << "Latest release: " << manifest->tag << " (current " << options.current_version << ")\n";
    if (options.check_only) {
        return cmp < 0 ? 2 : 0;
    }

    std::string platform = current_platform_key();
    const ReleaseAsset* asset = find_asset(*manifest, platform);
    if (!asset) {
        err << "Error: release " << manifest->tag << " has no asset for " << platform << "\n";
        return 1;
    }

    std::string base = manifest->asset_base_url.empty()
        ? release_download_base(options.repository)
        : manifest->asset_base_url;
    std::string asset_url = base + "/" + asset->name;

    fs::path executable = options.executable_path.empty()
        ? fs::path(current_executable_path(nullptr))
        : fs::path(options.executable_path);
    if (executable.empty()) {
        err << "Error: could not determine current executable path\n";
        return 1;
    }

    fs::path temp_dir = unique_temp_dir();
    fs::path archive_path = temp_dir / asset->name;
    fs::path extract_dir = temp_dir / "extract";
    fs::create_directories(extract_dir);

    out << "Downloading " << asset_url << "\n";
    if (!download_url_to_file(asset_url, archive_path.string(), &error)) {
        err << "Error: " << error << "\n";
        return 1;
    }

    if (!asset->sha256.empty()) {
        out << "Verifying SHA-256\n";
        if (!verify_file_sha256(archive_path.string(), asset->sha256, &error)) {
            err << "Error: " << error << "\n";
            return 1;
        }
    } else {
        out << "Warning: release manifest did not provide a SHA-256 for " << asset->name << "\n";
    }

    if (!extract_archive(archive_path.string(), extract_dir.string(), &error)) {
        err << "Error: " << error << "\n";
        return 1;
    }

    auto binary = find_released_binary(extract_dir);
    if (!binary) {
        err << "Error: archive did not contain a sighmake executable\n";
        return 1;
    }

#ifdef _WIN32
    if (!schedule_windows_replace(*binary, executable, &error)) {
        err << "Error: " << error << "\n";
        return 1;
    }
    out << "Update scheduled. Restart your terminal after this process exits.\n";
#else
    if (!replace_unix_binary(*binary, executable, &error)) {
        err << "Error: " << error << "\n";
        return 1;
    }
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    out << "Updated sighmake to " << manifest->tag << ".\n";
#endif

    return 0;
}

} // namespace updater
} // namespace vcxproj
