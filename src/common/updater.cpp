#include "pch.h"
#include "updater.hpp"
#include "common/string_utils.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace vcxproj {
namespace updater {
namespace {

constexpr uint64_t kMaxManifestBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxArchiveBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxExtractedBinaryBytes = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxManifestLineBytes = 4096;
constexpr const char* kUpdateTempPrefix = "sighmake-update-";
constexpr const char* kFinishUpdateCommand = "--sighmake-finish-update";
constexpr const char* kCleanupUpdateCommand = "--sighmake-cleanup-update";

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

void set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

bool contains_control_character(const std::string& value) {
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
std::optional<std::wstring> utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0);
    if (size <= 0) {
        return std::nullopt;
    }

    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value.data(), static_cast<int>(value.size()),
                            wide.data(), size) != size) {
        return std::nullopt;
    }
    return wide;
}

std::optional<std::string> wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }

    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::nullopt;
    }

    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            value.data(), static_cast<int>(value.size()),
                            utf8.data(), size, nullptr, nullptr) != size) {
        return std::nullopt;
    }
    return utf8;
}
#endif

fs::path path_from_utf8(const std::string& value) {
#ifdef _WIN32
    return fs::u8path(value);
#else
    return fs::path(value);
#endif
}

std::string path_to_utf8(const fs::path& value) {
#ifdef _WIN32
    return value.u8string();
#else
    return value.string();
#endif
}

std::string random_suffix() {
    std::random_device rd;
    uint64_t random_bits = 0;
    for (int i = 0; i < 4; ++i) {
        random_bits = (random_bits << 16) ^ static_cast<uint64_t>(rd());
    }
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    uint64_t process_id = static_cast<uint64_t>(GetCurrentProcessId());
#else
    uint64_t process_id = static_cast<uint64_t>(getpid());
#endif
    std::ostringstream out;
    out << std::hex << now << '-' << process_id << '-' << random_bits;
    return out.str();
}

std::optional<fs::path> unique_temp_dir(std::string* error) {
    std::error_code ec;
    fs::path parent = fs::temp_directory_path(ec);
    if (ec || parent.empty()) {
        set_error(error, "Could not determine the temporary directory: " + ec.message());
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path candidate = parent / (std::string(kUpdateTempPrefix) + random_suffix());
#ifdef _WIN32
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            return candidate;
        }
        DWORD status = GetLastError();
        if (status != ERROR_ALREADY_EXISTS && status != ERROR_FILE_EXISTS) {
            set_error(error, "Could not create a private temporary directory (Windows error " +
                             std::to_string(status) + ")");
            return std::nullopt;
        }
#else
        if (::mkdir(candidate.c_str(), S_IRWXU) == 0) {
            return candidate;
        }
        if (errno != EEXIST) {
            set_error(error, "Could not create a private temporary directory: " +
                             std::error_code(errno, std::generic_category()).message());
            return std::nullopt;
        }
#endif
    }

    set_error(error, "Could not allocate a unique temporary directory");
    return std::nullopt;
}

struct TempDirGuard {
    fs::path dir;
    bool active = true;

    void keep() { active = false; }

    ~TempDirGuard() {
        if (active && !dir.empty()) {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    }
};

std::string normalize_numeric_component(std::string value) {
    size_t first_nonzero = value.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        return "0";
    }
    return value.substr(first_nonzero);
}

std::vector<std::string> parse_version_numbers(const std::string& version) {
    std::string normalized = normalize_version(version);
    std::vector<std::string> parts;
    std::string current;

    for (char c : normalized) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            current += c;
        } else if (c == '.') {
            parts.push_back(normalize_numeric_component(current.empty() ? "0" : current));
            current.clear();
        } else {
            break;
        }
    }

    if (!current.empty() || parts.empty()) {
        parts.push_back(normalize_numeric_component(current.empty() ? "0" : current));
    }

    while (parts.size() < 3) {
        parts.push_back("0");
    }

    return parts;
}

int compare_numeric_component(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return lhs.size() < rhs.size() ? -1 : 1;
    }
    if (lhs == rhs) {
        return 0;
    }
    return lhs < rhs ? -1 : 1;
}

bool is_valid_repository(const std::string& repository) {
    if (repository.empty() || repository.size() > 200 || contains_control_character(repository)) {
        return false;
    }

    size_t slash = repository.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= repository.size() ||
        repository.find('/', slash + 1) != std::string::npos) {
        return false;
    }

    for (char c : repository) {
        if (c == '/') {
            continue;
        }
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

bool is_valid_https_url(const std::string& url) {
    constexpr const char* prefix = "https://";
    if (!starts_with(url, prefix) || url.size() > 2048 || contains_control_character(url) ||
        url.find('\\') != std::string::npos || url.find('#') != std::string::npos) {
        return false;
    }

    size_t host_start = std::strlen(prefix);
    size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos || path_start == host_start) {
        return false;
    }

    std::string host = url.substr(host_start, path_start - host_start);
    if (host.front() == '.' || host.back() == '.' || host.find('@') != std::string::npos ||
        host.find(':') != std::string::npos) {
        return false;
    }
    for (char c : host) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '.') {
            return false;
        }
    }

    return url.find('?', path_start) == std::string::npos;
}

bool is_valid_release_version(const std::string& version) {
    if (version.empty() || version.size() > 64 || contains_control_character(version) ||
        !std::isdigit(static_cast<unsigned char>(version.front()))) {
        return false;
    }

    bool in_suffix = false;
    bool component_has_digit = false;
    size_t component_count = 1;
    for (size_t i = 0; i < version.size(); ++i) {
        char c = version[i];
        unsigned char uc = static_cast<unsigned char>(c);
        if (!in_suffix) {
            if (std::isdigit(uc)) {
                component_has_digit = true;
            } else if (c == '.') {
                if (!component_has_digit || ++component_count > 8) {
                    return false;
                }
                component_has_digit = false;
            } else if (c == '-') {
                if (!component_has_digit || i + 1 == version.size()) {
                    return false;
                }
                in_suffix = true;
            } else {
                return false;
            }
        } else if (!std::isalnum(uc) && c != '.' && c != '-') {
            return false;
        }
    }
    return component_has_digit || in_suffix;
}

bool is_valid_sha256(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool is_valid_manifest_key(const std::string& key) {
    if (key.empty() || key.size() > 64) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    });
}

bool is_safe_asset_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == ".." ||
        contains_control_character(name)) {
        return false;
    }

    fs::path path = path_from_utf8(name);
    if (path.is_absolute() || path.has_root_path() || path.has_parent_path() ||
        path.filename() != path) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    });
}

std::string expected_asset_name(const std::string& platform) {
    std::string asset_platform = platform;
    std::replace(asset_platform.begin(), asset_platform.end(), '_', '-');
#ifdef _WIN32
    return "sighmake-" + asset_platform + ".zip";
#else
    return "sighmake-" + asset_platform + ".tar.gz";
#endif
}

#ifdef _WIN32
std::wstring quote_windows_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(c);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool run_windows_process(const fs::path& application,
                         const std::vector<std::wstring>& arguments,
                         bool wait,
                         DWORD creation_flags,
                         DWORD* process_id,
                         std::string* error) {
    std::wstring command = quote_windows_argument(application.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_windows_argument(argument);
    }

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        creation_flags, nullptr, nullptr, &startup, &process)) {
        set_error(error, "Failed to start update helper (Windows error " +
                         std::to_string(GetLastError()) + ")");
        return false;
    }

    if (process_id) {
        *process_id = process.dwProcessId;
    }

    bool success = true;
    if (wait) {
        DWORD wait_status = WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exit_code = 1;
        if (wait_status != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exit_code) ||
            exit_code != 0) {
            set_error(error, "Update helper failed" +
                             (wait_status == WAIT_OBJECT_0
                                  ? " with status " + std::to_string(exit_code)
                                  : std::string(" while waiting for completion")));
            success = false;
        }
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return success;
}

std::optional<fs::path> powershell_executable(std::string* error) {
    std::vector<wchar_t> buffer(MAX_PATH + 1);
    UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        set_error(error, "Could not locate Windows PowerShell");
        return std::nullopt;
    }
    fs::path executable(std::wstring(buffer.data(), length));
    executable /= L"WindowsPowerShell\\v1.0\\powershell.exe";
    std::error_code ec;
    if (!fs::is_regular_file(executable, ec) || ec) {
        set_error(error, "Could not locate Windows PowerShell at " + path_to_utf8(executable));
        return std::nullopt;
    }
    return executable;
}

bool parse_https_url(const std::string& url, std::wstring& host, std::wstring& path) {
    if (!is_valid_https_url(url)) {
        return false;
    }

    constexpr size_t host_start = 8;
    size_t path_start = url.find('/', host_start);
    auto wide_host = utf8_to_wide(url.substr(host_start, path_start - host_start));
    auto wide_path = utf8_to_wide(url.substr(path_start));
    if (!wide_host || !wide_path) {
        return false;
    }
    host = std::move(*wide_host);
    path = std::move(*wide_path);
    return true;
}

bool download_url_to_file_windows(const std::string& url,
                                  const fs::path& output_path,
                                  uint64_t max_bytes,
                                  std::string* error) {
    std::wstring host;
    std::wstring path;
    if (!parse_https_url(url, host, path)) {
        set_error(error, "Only well-formed https:// URLs are supported");
        return false;
    }

    HINTERNET session = WinHttpOpen(L"sighmake updater/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        set_error(error, "WinHttpOpen failed (Windows error " +
                         std::to_string(GetLastError()) + ")");
        return false;
    }

    if (!WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000)) {
        DWORD status = GetLastError();
        WinHttpCloseHandle(session);
        set_error(error, "Failed to configure updater network timeouts (Windows error " +
                         std::to_string(status) + ")");
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        set_error(error, "WinHttpConnect failed (Windows error " +
                         std::to_string(GetLastError()) + ")");
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        set_error(error, "WinHttpOpenRequest failed (Windows error " +
                         std::to_string(GetLastError()) + ")");
        return false;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy, sizeof(redirect_policy))) {
        DWORD status = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        set_error(error, "Failed to enforce HTTPS redirects (Windows error " +
                         std::to_string(status) + ")");
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
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status_code,
                                 &status_size,
                                 WINHTTP_NO_HEADER_INDEX) ||
            status_code < 200 || status_code >= 300) {
            ok = FALSE;
        }
    }

    DWORD content_length = 0;
    DWORD content_length_size = sizeof(content_length);
    if (ok && WinHttpQueryHeaders(request,
                                  WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX,
                                  &content_length,
                                  &content_length_size,
                                  WINHTTP_NO_HEADER_INDEX) &&
        static_cast<uint64_t>(content_length) > max_bytes) {
        set_error(error, "Download exceeds the updater size limit");
        ok = FALSE;
    }

    HANDLE output = INVALID_HANDLE_VALUE;
    if (ok) {
        output = CreateFileW(output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            set_error(error, "Failed to create updater download file (Windows error " +
                             std::to_string(GetLastError()) + ")");
            ok = FALSE;
        }
    }

    uint64_t total_bytes = 0;
    std::array<char, 64 * 1024> buffer{};
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            set_error(error, "Failed while reading the HTTPS response (Windows error " +
                             std::to_string(GetLastError()) + ")");
            ok = FALSE;
            break;
        }
        if (available == 0) {
            break;
        }

        DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), requested, &read)) {
            set_error(error, "Failed while reading the HTTPS response (Windows error " +
                             std::to_string(GetLastError()) + ")");
            ok = FALSE;
            break;
        }
        if (read > max_bytes - total_bytes) {
            set_error(error, "Download exceeds the updater size limit");
            ok = FALSE;
            break;
        }

        DWORD written = 0;
        if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read) {
            set_error(error, "Failed while writing the updater download (Windows error " +
                             std::to_string(GetLastError()) + ")");
            ok = FALSE;
            break;
        }
        total_bytes += read;
    }

    if (output != INVALID_HANDLE_VALUE) {
        if (ok && !FlushFileBuffers(output)) {
            set_error(error, "Failed to flush the updater download (Windows error " +
                             std::to_string(GetLastError()) + ")");
            ok = FALSE;
        }
        CloseHandle(output);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok) {
        DeleteFileW(output_path.c_str());
        if (error && error->empty()) {
            *error = status_code ? "HTTPS download failed with status " + std::to_string(status_code)
                                 : "HTTPS download failed";
        }
    }

    return ok == TRUE;
}
#else
std::optional<fs::path> find_trusted_tool(std::initializer_list<const char*> candidates) {
    std::error_code ec;
    for (const char* candidate : candidates) {
        fs::path path(candidate);
        if (fs::is_regular_file(path, ec) && !ec) {
            return path;
        }
        ec.clear();
    }
    return std::nullopt;
}

bool run_unix_process(const std::vector<std::string>& arguments,
                      int stdout_fd,
                      uint64_t max_output_bytes,
                      std::string* error) {
    if (arguments.empty()) {
        set_error(error, "Updater helper command is empty");
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        set_error(error, "Could not start updater helper: " +
                         std::error_code(errno, std::generic_category()).message());
        return false;
    }

    if (child == 0) {
        if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) {
            _exit(126);
        }
        if (max_output_bytes > 0) {
            struct rlimit limit {};
            limit.rlim_cur = static_cast<rlim_t>(max_output_bytes);
            limit.rlim_max = static_cast<rlim_t>(max_output_bytes);
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
                _exit(126);
            }
        }

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            set_error(error, "Could not wait for updater helper: " +
                             std::error_code(errno, std::generic_category()).message());
            return false;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, "Updater helper failed" +
                         (WIFEXITED(status)
                              ? " with status " + std::to_string(WEXITSTATUS(status))
                              : std::string(" after receiving a signal")));
        return false;
    }
    return true;
}

bool download_url_to_file_unix(const std::string& url,
                               const fs::path& output_path,
                               uint64_t max_bytes,
                               std::string* error) {
    if (!is_valid_https_url(url)) {
        set_error(error, "Only well-formed https:// URLs are supported");
        return false;
    }

    std::optional<fs::path> tool = find_trusted_tool({"/usr/bin/curl", "/bin/curl"});
    std::vector<std::string> arguments;
    if (tool) {
        arguments = {
            tool->string(), "-q", "--fail", "--location", "--silent", "--show-error",
            "--retry", "3", "--proto", "=https", "--proto-redir", "=https",
            "--tlsv1.2", "--connect-timeout", "10", "--max-time", "120",
            "--max-filesize", std::to_string(max_bytes), "--output", output_path.string(),
            "--", url
        };
    } else {
        tool = find_trusted_tool({"/usr/bin/wget", "/bin/wget"});
        if (!tool) {
            set_error(error, "The updater requires /usr/bin/curl or /usr/bin/wget");
            return false;
        }
        arguments = {
            tool->string(), "--no-config", "--https-only", "--secure-protocol=TLSv1_2",
            "--timeout=30", "--tries=3", "--max-redirect=10",
            "--quota=" + std::to_string(max_bytes),
            "--output-document=" + output_path.string(), "--", url
        };
    }

    std::error_code ec;
    fs::remove(output_path, ec);
    if (!run_unix_process(arguments, -1, 0, error)) {
        fs::remove(output_path, ec);
        return false;
    }

    fs::file_status status = fs::symlink_status(output_path, ec);
    if (ec || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        fs::remove(output_path, ec);
        set_error(error, "HTTPS download did not produce a regular file");
        return false;
    }
    uint64_t size = fs::file_size(output_path, ec);
    if (ec || size > max_bytes) {
        fs::remove(output_path, ec);
        set_error(error, "Download exceeds the updater size limit");
        return false;
    }
    return true;
}
#endif

bool download_url_to_file(const std::string& url,
                          const fs::path& output_path,
                          uint64_t max_bytes,
                          std::string* error) {
#ifdef _WIN32
    return download_url_to_file_windows(url, output_path, max_bytes, error);
#else
    return download_url_to_file_unix(url, output_path, max_bytes, error);
#endif
}

std::optional<std::string> read_release_manifest_file_impl(const fs::path& path,
                                                           std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "Could not open the downloaded release manifest");
        return std::nullopt;
    }

    std::string text;
    std::array<char, 16 * 1024> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize bytes_read = in.gcount();
        if (bytes_read > 0) {
            uint64_t byte_count = static_cast<uint64_t>(bytes_read);
            if (byte_count > kMaxManifestBytes - static_cast<uint64_t>(text.size())) {
                set_error(error, "Release manifest exceeds the updater size limit");
                return std::nullopt;
            }
            text.append(buffer.data(), static_cast<size_t>(bytes_read));
        }
    }

    if (in.bad() || !in.eof()) {
        set_error(error, "Could not read the downloaded release manifest");
        return std::nullopt;
    }
    return text;
}

std::optional<std::string> download_url_to_string(const std::string& url, std::string* error) {
    auto temp_dir = unique_temp_dir(error);
    if (!temp_dir) {
        return std::nullopt;
    }
    TempDirGuard guard{*temp_dir};
    fs::path temp_file = *temp_dir / "download.txt";
    if (!download_url_to_file(url, temp_file, kMaxManifestBytes, error)) {
        return std::nullopt;
    }
    return read_release_manifest_file_impl(temp_file, error);
}

std::string release_download_base(const std::string& repository) {
    return "https://github.com/" + repository + "/releases/latest/download";
}

std::string release_download_base(const std::string& repository, const std::string& tag) {
    return "https://github.com/" + repository + "/releases/download/" + tag;
}

bool manifest_url_is_overridden() {
    const char* override_url = std::getenv("SIGHMAKE_UPDATE_MANIFEST_URL");
    return override_url && *override_url;
}

std::string manifest_url_for_repository(const std::string& repository) {
    if (const char* override_url = std::getenv("SIGHMAKE_UPDATE_MANIFEST_URL")) {
        if (*override_url) {
            return override_url;
        }
    }
    return release_download_base(repository) + "/sighmake-release.ini";
}

uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

class Sha256 {
public:
    void update(const uint8_t* data, size_t size) {
        total_bytes_ += static_cast<uint64_t>(size);
        while (size > 0) {
            size_t count = std::min(size, block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, data, count);
            block_size_ += count;
            data += count;
            size -= count;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        uint64_t bit_length = total_bytes_ * 8;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, 0);
        for (size_t i = 0; i < 8; ++i) {
            block_[63 - i] = static_cast<uint8_t>(bit_length >> (i * 8));
        }
        transform(block_.data());

        std::array<uint8_t, 32> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    void transform(const uint8_t* block) {
        static constexpr std::array<uint32_t, 64> constants = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i) {
            uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                          (words[i - 15] >> 3);
            uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                          (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (size_t i = 0; i < words.size(); ++i) {
            uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            uint32_t choice = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + sum1 + choice + constants[i] + words[i];
            uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_ = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    std::array<uint8_t, 64> block_{};
    size_t block_size_ = 0;
    uint64_t total_bytes_ = 0;
};

std::string sha256_file(const fs::path& path, std::string* error) {
    std::error_code ec;
    fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        set_error(error, "Cannot hash a non-regular file: " + path_to_utf8(path));
        return "";
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "Could not open file for SHA-256: " + path_to_utf8(path));
        return "";
    }

    Sha256 sha;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = input.gcount();
        if (count > 0) {
            sha.update(reinterpret_cast<const uint8_t*>(buffer.data()),
                       static_cast<size_t>(count));
        }
    }
    if (!input.eof()) {
        set_error(error, "Failed while reading file for SHA-256: " + path_to_utf8(path));
        return "";
    }

    auto digest = sha.finish();
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (uint8_t byte : digest) {
        encoded << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return encoded.str();
}

bool validate_extracted_binary(const fs::path& binary, std::string* error) {
    std::error_code ec;
    fs::file_status status = fs::symlink_status(binary, ec);
    if (ec || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        set_error(error, "Release archive did not produce a regular executable");
        return false;
    }

    uint64_t size = fs::file_size(binary, ec);
    if (ec || size == 0 || size > kMaxExtractedBinaryBytes) {
        set_error(error, "Extracted executable has an invalid size");
        return false;
    }

    std::ifstream input(binary, std::ios::binary);
    std::array<uint8_t, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    if (input.gcount() < 4) {
        set_error(error, "Extracted executable is truncated");
        return false;
    }

#ifdef _WIN32
    bool valid_magic = magic[0] == 'M' && magic[1] == 'Z';
#elif defined(__APPLE__)
    uint32_t value = (static_cast<uint32_t>(magic[0]) << 24) |
                     (static_cast<uint32_t>(magic[1]) << 16) |
                     (static_cast<uint32_t>(magic[2]) << 8) |
                     static_cast<uint32_t>(magic[3]);
    bool valid_magic = value == 0xfeedfaceu || value == 0xfeedfacfu ||
                       value == 0xcefaedfeu || value == 0xcffaedfeu ||
                       value == 0xcafebabeu || value == 0xbebafecau;
#else
    bool valid_magic = magic[0] == 0x7f && magic[1] == 'E' &&
                       magic[2] == 'L' && magic[3] == 'F';
#endif
    if (!valid_magic) {
        set_error(error, "Release archive contains the wrong executable format");
        return false;
    }
    return true;
}

#ifdef _WIN32
std::optional<fs::path> extract_release_binary(const fs::path& archive_path,
                                               const fs::path& output_dir,
                                               std::string* error) {
    fs::path script_path = output_dir.parent_path() / "extract-release.ps1";
    fs::path binary_path = output_dir / "sighmake.exe";
    std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
    if (!script) {
        set_error(error, "Failed to create the archive extraction helper");
        return std::nullopt;
    }
    script << R"PS1(param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][UInt64]$MaxBytes
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $entries = @($zip.Entries | Where-Object { $_.FullName -ceq 'sighmake.exe' })
    if ($entries.Count -ne 1) { throw 'archive must contain exactly one top-level sighmake.exe' }
    $entry = $entries[0]
    if ([UInt64]$entry.Length -eq 0 -or [UInt64]$entry.Length -gt $MaxBytes) {
        throw 'extracted executable exceeds the size limit'
    }
    $source = $entry.Open()
    try {
        $destination = [IO.File]::Open($Output, [IO.FileMode]::CreateNew,
                                      [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $source.CopyTo($destination)
            $destination.Flush($true)
        } finally {
            $destination.Dispose()
        }
    } finally {
        $source.Dispose()
    }
    if ([UInt64](Get-Item -LiteralPath $Output).Length -ne [UInt64]$entry.Length) {
        throw 'extracted executable length mismatch'
    }
} finally {
    $zip.Dispose()
}
)PS1";
    script.close();
    if (!script) {
        std::error_code ec;
        fs::remove(script_path, ec);
        set_error(error, "Failed to write the archive extraction helper");
        return std::nullopt;
    }

    auto powershell = powershell_executable(error);
    if (!powershell) {
        std::error_code ec;
        fs::remove(script_path, ec);
        return std::nullopt;
    }
    std::vector<std::wstring> arguments = {
        L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
        L"-File", script_path.wstring(), archive_path.wstring(), binary_path.wstring(),
        std::to_wstring(kMaxExtractedBinaryBytes)
    };
    bool extracted = run_windows_process(*powershell, arguments, true, CREATE_NO_WINDOW,
                                         nullptr, error);
    std::error_code ec;
    fs::remove(script_path, ec);
    if (!extracted || !validate_extracted_binary(binary_path, error)) {
        fs::remove(binary_path, ec);
        return std::nullopt;
    }
    return binary_path;
}
#else
std::optional<fs::path> extract_release_binary(const fs::path& archive_path,
                                               const fs::path& output_dir,
                                               std::string* error) {
    auto tar = find_trusted_tool({"/usr/bin/tar", "/bin/tar"});
    if (!tar) {
        set_error(error, "The updater requires /usr/bin/tar or /bin/tar");
        return std::nullopt;
    }

    fs::path binary_path = output_dir / "sighmake";
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int output = open(binary_path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (output < 0) {
        set_error(error, "Could not create the extracted executable: " +
                         std::error_code(errno, std::generic_category()).message());
        return std::nullopt;
    }

    std::vector<std::string> arguments = {
        tar->string(), "-xOzf", archive_path.string(), "sighmake"
    };
    bool extracted = run_unix_process(arguments, output, kMaxExtractedBinaryBytes, error);
    close(output);
    std::error_code ec;
    if (!extracted || !validate_extracted_binary(binary_path, error)) {
        fs::remove(binary_path, ec);
        return std::nullopt;
    }
    return binary_path;
}
#endif

bool replace_unix_binary_impl(const fs::path& source,
                              const fs::path& target,
                              std::string* error) {
    std::error_code ec;
    fs::file_status source_status = fs::symlink_status(source, ec);
    if (ec || !fs::is_regular_file(source_status) || fs::is_symlink(source_status)) {
        set_error(error, "New executable is not a regular file");
        return false;
    }
    fs::file_status target_status = fs::symlink_status(target, ec);
    if (ec || !fs::is_regular_file(target_status) || fs::is_symlink(target_status)) {
        set_error(error, "Current executable is not a regular file");
        return false;
    }

    fs::path parent = target.parent_path();
    if (parent.empty()) {
        set_error(error, "Current executable has no parent directory");
        return false;
    }

    fs::path staged;
    bool copied = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        staged = target;
        staged += ".new." + random_suffix();
        ec.clear();
        copied = fs::copy_file(source, staged, fs::copy_options::none, ec);
        if (copied) {
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            break;
        }
    }
    if (!copied) {
        set_error(error, "Failed to stage new executable: " +
                         (ec ? ec.message() : std::string("unique name unavailable")));
        return false;
    }

    fs::perms target_permissions = fs::status(target, ec).permissions();
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        set_error(error, "Failed to read current executable permissions: " + ec.message());
        return false;
    }
    fs::perms safe_permissions = fs::perms::owner_read | fs::perms::owner_write |
                                 fs::perms::owner_exec |
                                 (target_permissions & (fs::perms::group_read |
                                                        fs::perms::group_exec |
                                                        fs::perms::others_read |
                                                        fs::perms::others_exec));
    fs::permissions(staged, safe_permissions, fs::perm_options::replace, ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        set_error(error, "Failed to set new executable permissions: " + ec.message());
        return false;
    }

    std::string source_hash = sha256_file(source, error);
    std::string staged_hash = sha256_file(staged, error);
    if (source_hash.empty() || staged_hash.empty() || source_hash != staged_hash) {
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        if (source_hash != staged_hash) {
            set_error(error, "Staged executable failed its integrity check");
        }
        return false;
    }

#ifndef _WIN32
    int staged_fd = open(staged.c_str(), O_RDONLY);
    if (staged_fd < 0 || fsync(staged_fd) != 0) {
        int status = errno;
        if (staged_fd >= 0) close(staged_fd);
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        set_error(error, "Failed to flush staged executable: " +
                         std::error_code(status, std::generic_category()).message());
        return false;
    }
    close(staged_fd);
#endif

    fs::path backup = target;
    backup += ".old." + random_suffix();
    fs::rename(target, backup, ec);
    if (ec) {
        std::error_code rename_error = ec;
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        set_error(error, "Failed to move existing executable: " + rename_error.message());
        return false;
    }

    fs::rename(staged, target, ec);
    if (ec) {
        std::error_code install_error = ec;
        std::error_code restore_ec;
        fs::remove(target, restore_ec);
        restore_ec.clear();
        fs::rename(backup, target, restore_ec);
        std::error_code cleanup_ec;
        fs::remove(staged, cleanup_ec);
        set_error(error, "Failed to install staged executable: " + install_error.message() +
                         (restore_ec ? "; rollback also failed: " + restore_ec.message()
                                     : std::string()));
        return false;
    }

    fs::remove(backup, ec);
#ifndef _WIN32
    int parent_fd = open(parent.c_str(), O_RDONLY);
    if (parent_fd >= 0) {
        fsync(parent_fd);
        close(parent_fd);
    }
#endif
    return true;
}

#ifdef _WIN32
std::string hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char c : value) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 0x0f]);
    }
    return encoded;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::string> hex_decode(const std::string& value) {
    if (value.empty() || value.size() > 65536 || value.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        int high = hex_value(value[i]);
        int low = hex_value(value[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    if (decoded.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return decoded;
}

std::optional<DWORD> parse_process_id(const char* value) {
    if (!value || !*value) {
        return std::nullopt;
    }
    uint64_t parsed = 0;
    for (const char* cursor = value; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return std::nullopt;
        }
        parsed = parsed * 10 + static_cast<unsigned int>(*cursor - '0');
        if (parsed == 0 || parsed > std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<DWORD>(parsed);
}

bool wait_for_process_exit(DWORD process_id, std::string* error) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            return true;
        }
        set_error(error, "Could not wait for the previous updater process (Windows error " +
                         std::to_string(GetLastError()) + ")");
        return false;
    }
    DWORD status = WaitForSingleObject(process, 120000);
    CloseHandle(process);
    if (status != WAIT_OBJECT_0) {
        set_error(error, status == WAIT_TIMEOUT
                             ? "Timed out waiting for the previous updater process"
                             : "Failed while waiting for the previous updater process");
        return false;
    }
    return true;
}

bool validate_windows_target(const fs::path& target, std::string* error) {
    if (!target.is_absolute() || target.filename().empty()) {
        set_error(error, "Update target must be an absolute executable path");
        return false;
    }
    DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        set_error(error, "Update target is not a regular, non-reparse-point file");
        return false;
    }
    return true;
}

bool schedule_windows_replace(const fs::path& source,
                              const fs::path& target,
                              std::string* error) {
    if (!validate_windows_target(target, error)) {
        return false;
    }
    std::string source_hash = sha256_file(source, error);
    if (source_hash.empty()) {
        return false;
    }

    std::string encoded_target = hex_encode(path_to_utf8(target));
    std::vector<std::wstring> arguments = {
        std::wstring(kFinishUpdateCommand, kFinishUpdateCommand + std::strlen(kFinishUpdateCommand)),
        std::to_wstring(GetCurrentProcessId()),
        std::wstring(encoded_target.begin(), encoded_target.end()),
        std::wstring(source_hash.begin(), source_hash.end())
    };
    return run_windows_process(source, arguments, false,
                               CREATE_NO_WINDOW | DETACHED_PROCESS,
                               nullptr, error);
}

int finish_windows_update(int argc, char* argv[]) {
    if (argc != 5) {
        return 1;
    }
    auto parent_id = parse_process_id(argv[2]);
    auto decoded_target = hex_decode(argv[3]);
    std::string expected_hash = to_lower(argv[4] ? argv[4] : "");
    if (!parent_id || !decoded_target || !is_valid_sha256(expected_hash)) {
        return 1;
    }

    std::string error;
    fs::path source = path_from_utf8(current_executable_path(nullptr));
    fs::path target = path_from_utf8(*decoded_target);
    if (source.empty() || !validate_windows_target(target, &error) ||
        sha256_file(source, &error) != expected_hash ||
        !wait_for_process_exit(*parent_id, &error)) {
        return 1;
    }

    fs::path staged;
    bool copied = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        staged = target;
        staged += ".new." + random_suffix();
        if (CopyFileW(source.c_str(), staged.c_str(), TRUE)) {
            copied = true;
            break;
        }
        DWORD status = GetLastError();
        if (status != ERROR_FILE_EXISTS && status != ERROR_ALREADY_EXISTS) {
            return 1;
        }
    }
    if (!copied || sha256_file(staged, &error) != expected_hash) {
        std::error_code ec;
        fs::remove(staged, ec);
        return 1;
    }

    fs::path backup = target;
    backup += ".old." + random_suffix();
    if (!ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        std::error_code ec;
        fs::remove(staged, ec);
        return 1;
    }

    if (sha256_file(target, &error) != expected_hash) {
        ReplaceFileW(target.c_str(), backup.c_str(), nullptr,
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        return 1;
    }
    DeleteFileW(backup.c_str());

    std::string encoded_source = hex_encode(path_to_utf8(source));
    std::vector<std::wstring> cleanup_arguments = {
        std::wstring(kCleanupUpdateCommand,
                     kCleanupUpdateCommand + std::strlen(kCleanupUpdateCommand)),
        std::to_wstring(GetCurrentProcessId()),
        std::wstring(encoded_source.begin(), encoded_source.end())
    };
    run_windows_process(target, cleanup_arguments, false,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr);
    return 0;
}

bool validate_cleanup_source(const fs::path& source) {
    std::error_code ec;
    fs::path extract_dir = source.parent_path();
    fs::path root = extract_dir.parent_path();
    fs::path temp = fs::temp_directory_path(ec);
    if (ec || source.filename() != L"sighmake.exe" || extract_dir.filename() != L"extract" ||
        !starts_with(path_to_utf8(root.filename()), kUpdateTempPrefix) ||
        root.parent_path().lexically_normal() != temp.lexically_normal()) {
        return false;
    }
    fs::file_status root_status = fs::symlink_status(root, ec);
    if (ec || !fs::is_directory(root_status) || fs::is_symlink(root_status)) {
        return false;
    }
    fs::file_status extract_status = fs::symlink_status(extract_dir, ec);
    return !ec && fs::is_directory(extract_status) && !fs::is_symlink(extract_status);
}

int cleanup_windows_update(int argc, char* argv[]) {
    if (argc != 4) {
        return 1;
    }
    auto parent_id = parse_process_id(argv[2]);
    auto decoded_source = hex_decode(argv[3]);
    if (!parent_id || !decoded_source) {
        return 1;
    }
    fs::path source = path_from_utf8(*decoded_source);
    if (!validate_cleanup_source(source)) {
        return 1;
    }

    std::string error;
    if (!wait_for_process_exit(*parent_id, &error)) {
        return 1;
    }
    std::error_code ec;
    fs::path extract_dir = source.parent_path();
    fs::path root = extract_dir.parent_path();
    fs::remove(source, ec);
    ec.clear();
    fs::remove(extract_dir, ec);
    ec.clear();
    fs::remove(root, ec);
    return 0;
}
#else
bool replace_unix_binary(const fs::path& source,
                         const fs::path& target,
                         std::string* error) {
    return replace_unix_binary_impl(source, target, error);
}
#endif

} // namespace

std::optional<std::string> read_release_manifest_file(const fs::path& path,
                                                      std::string* error) {
    if (error) error->clear();
    return read_release_manifest_file_impl(path, error);
}

bool is_internal_update_command(const char* command) {
    return command && (std::strcmp(command, kFinishUpdateCommand) == 0 ||
                       std::strcmp(command, kCleanupUpdateCommand) == 0);
}

int run_internal_update_command(int argc, char* argv[]) {
#ifdef _WIN32
    if (argc >= 2 && std::strcmp(argv[1], kFinishUpdateCommand) == 0) {
        return finish_windows_update(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], kCleanupUpdateCommand) == 0) {
        return cleanup_windows_update(argc, argv);
    }
#else
    (void)argc;
    (void)argv;
#endif
    return 1;
}

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
    left.resize(count, "0");
    right.resize(count, "0");

    for (size_t i = 0; i < count; ++i) {
        int comparison = compare_numeric_component(left[i], right[i]);
        if (comparison != 0) return comparison;
    }
    return 0;
}

std::optional<ReleaseManifest> parse_release_manifest(const std::string& text,
                                                       std::string* error) {
    if (error) error->clear();
    if (text.size() > kMaxManifestBytes) {
        set_error(error, "Release manifest exceeds the updater size limit");
        return std::nullopt;
    }

    ReleaseManifest manifest;
    std::map<std::string, std::string> values;
    std::istringstream input(text);
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > kMaxManifestLineBytes) {
            set_error(error, "Manifest line " + std::to_string(line_number) + " is too long");
            return std::nullopt;
        }
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            set_error(error, "Invalid manifest line " + std::to_string(line_number));
            return std::nullopt;
        }

        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!is_valid_manifest_key(key) || contains_control_character(value)) {
            set_error(error, "Invalid manifest field on line " + std::to_string(line_number));
            return std::nullopt;
        }
        if (!values.emplace(key, value).second) {
            set_error(error, "Duplicate manifest field: " + key);
            return std::nullopt;
        }
    }

    auto version = values.find("version");
    auto tag = values.find("tag");
    if (version == values.end() || version->second.empty()) {
        set_error(error, "Manifest is missing version");
        return std::nullopt;
    }
    if (tag == values.end() || tag->second.empty()) {
        set_error(error, "Manifest is missing tag");
        return std::nullopt;
    }
    manifest.version = version->second;
    manifest.tag = tag->second;
    auto asset_base_url = values.find("asset_base_url");
    if (asset_base_url != values.end()) {
        manifest.asset_base_url = asset_base_url->second;
    }

    if (!is_valid_release_version(manifest.version)) {
        set_error(error, "Manifest has an invalid release version");
        return std::nullopt;
    }
    if (manifest.tag != "v" + manifest.version) {
        set_error(error, "Manifest tag does not match its version");
        return std::nullopt;
    }
    if (!manifest.asset_base_url.empty() && !is_valid_https_url(manifest.asset_base_url)) {
        set_error(error, "Manifest asset_base_url must be a well-formed HTTPS URL");
        return std::nullopt;
    }

    for (const auto& [key, value] : values) {
        if (ends_with(key, "_sha256")) {
            std::string asset_key = key.substr(0, key.size() - 7);
            if (asset_key.empty() || values.find(asset_key) == values.end()) {
                set_error(error, "Manifest contains an orphan checksum: " + key);
                return std::nullopt;
            }
            continue;
        }
        if (key == "version" || key == "tag" || key == "asset_base_url") {
            continue;
        }
        if (value.empty() || !is_safe_asset_name(value)) {
            set_error(error, "Manifest has an unsafe asset name for " + key);
            return std::nullopt;
        }

        ReleaseAsset asset;
        asset.platform_key = key;
        asset.name = value;
        auto sha_it = values.find(key + "_sha256");
        if (sha_it == values.end()) {
            set_error(error, "Manifest is missing SHA-256 for " + key);
            return std::nullopt;
        }
        asset.sha256 = to_lower(sha_it->second);
        if (!is_valid_sha256(asset.sha256)) {
            set_error(error, "Manifest has an invalid SHA-256 for " + key);
            return std::nullopt;
        }
        manifest.assets.push_back(asset);
        if (manifest.assets.size() > 64) {
            set_error(error, "Manifest lists too many release assets");
            return std::nullopt;
        }
    }

    if (manifest.assets.empty()) {
        set_error(error, "Manifest does not list any release assets");
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
    std::vector<wchar_t> buffer(512);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size > 0 && size < buffer.size()) {
            fs::path executable(std::wstring(buffer.data(), size));
            std::error_code ec;
            executable = fs::absolute(executable, ec);
            if (!ec) {
                return path_to_utf8(executable);
            }
            break;
        }
        if (size == 0 || buffer.size() >= 32768) {
            break;
        }
        buffer.resize(buffer.size() * 2);
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
        std::error_code ec;
        fs::path path = fs::absolute(path_from_utf8(argv0), ec);
        if (!ec) {
            return path_to_utf8(path);
        }
    }
    return "";
}

bool verify_file_sha256(const std::string& path,
                        const std::string& expected_sha256,
                        std::string* error) {
    if (error) error->clear();
    std::string expected = to_lower(trim(expected_sha256));
    if (!is_valid_sha256(expected)) {
        set_error(error, "Expected SHA-256 must contain exactly 64 hexadecimal characters");
        return false;
    }

    std::string actual = sha256_file(path_from_utf8(path), error);
    if (actual.empty()) {
        return false;
    }

    unsigned int difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned int>(actual[i] ^ expected[i]);
    }
    if (difference != 0) {
        set_error(error, "SHA-256 mismatch for " + path + ": expected " + expected +
                         ", got " + actual);
        return false;
    }

    return true;
}

int run_update(const UpdateOptions& options) {
    std::ostream& out = options.out ? *options.out : std::cout;
    std::ostream& err = options.err ? *options.err : std::cerr;

    try {
        if (!is_valid_repository(options.repository)) {
            err << "Error: release repository must be a valid GitHub owner/repository name\n";
            return 1;
        }

        std::string manifest_url = manifest_url_for_repository(options.repository);
        if (!is_valid_https_url(manifest_url)) {
            err << "Error: updater manifest URL must be a well-formed HTTPS URL\n";
            return 1;
        }
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

        std::string canonical_latest_base = release_download_base(options.repository);
        std::string canonical_tag_base = release_download_base(options.repository, manifest->tag);
        bool custom_manifest = manifest_url_is_overridden();
        if (!custom_manifest && !manifest->asset_base_url.empty() &&
            manifest->asset_base_url != canonical_latest_base &&
            manifest->asset_base_url != canonical_tag_base) {
            err << "Error: release manifest points outside the configured GitHub repository\n";
            return 1;
        }

        std::string current = normalize_version(options.current_version);
        std::string latest = normalize_version(manifest->version);
        int cmp = compare_versions(current, latest);

        if (cmp >= 0 && !options.force) {
            out << "sighmake is up to date (" << options.current_version << ").\n";
            return 0;
        }

        out << "Latest release: " << manifest->tag << " (current "
            << options.current_version << ")\n";
        if (options.check_only) {
            return cmp < 0 ? 2 : 0;
        }

        std::string platform = current_platform_key();
        const ReleaseAsset* asset = find_asset(*manifest, platform);
        if (!asset) {
            err << "Error: release " << manifest->tag << " has no asset for "
                << platform << "\n";
            return 1;
        }
        std::string required_asset_name = expected_asset_name(platform);
        if (asset->name != required_asset_name) {
            err << "Error: release manifest selected unexpected asset " << asset->name
                << " for " << platform << "\n";
            return 1;
        }

        std::string base;
        if (custom_manifest) {
            if (!manifest->asset_base_url.empty()) {
                base = manifest->asset_base_url;
            } else {
                size_t slash = manifest_url.rfind('/');
                base = slash == std::string::npos ? std::string() : manifest_url.substr(0, slash);
            }
        } else {
            // Pin the asset request to the validated tag. This avoids a latest-release race
            // between fetching the manifest and fetching the archive.
            base = canonical_tag_base;
        }
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        std::string asset_url = base + "/" + asset->name;
        if (!is_valid_https_url(asset_url)) {
            err << "Error: release asset URL must be a well-formed HTTPS URL\n";
            return 1;
        }

        fs::path executable = path_from_utf8(options.executable_path.empty()
            ? current_executable_path(nullptr)
            : options.executable_path);
        if (executable.empty()) {
            err << "Error: could not determine current executable path\n";
            return 1;
        }
        std::error_code ec;
        executable = fs::absolute(executable, ec);
        fs::file_status executable_status = fs::symlink_status(executable, ec);
        if (ec || !fs::is_regular_file(executable_status) || fs::is_symlink(executable_status)) {
            err << "Error: current executable is not a regular, non-symlink file\n";
            return 1;
        }

        auto temp_dir = unique_temp_dir(&error);
        if (!temp_dir) {
            err << "Error: " << error << "\n";
            return 1;
        }
        TempDirGuard temp_guard{*temp_dir};
        fs::path archive_path = *temp_dir / asset->name;
        fs::path extract_dir = *temp_dir / "extract";
        if (!fs::create_directory(extract_dir, ec) || ec) {
            err << "Error: failed to create updater extraction directory: " << ec.message() << "\n";
            return 1;
        }

        out << "Downloading " << asset_url << "\n";
        if (!download_url_to_file(asset_url, archive_path, kMaxArchiveBytes, &error)) {
            err << "Error: " << error << "\n";
            return 1;
        }

        out << "Verifying SHA-256\n";
        if (!verify_file_sha256(path_to_utf8(archive_path), asset->sha256, &error)) {
            err << "Error: " << error << "\n";
            return 1;
        }

        auto binary = extract_release_binary(archive_path, extract_dir, &error);
        if (!binary) {
            err << "Error: " << error << "\n";
            return 1;
        }

#ifdef _WIN32
        if (!fs::remove(archive_path, ec) || ec) {
            err << "Error: failed to remove verified update archive before handoff: "
                << ec.message() << "\n";
            return 1;
        }
        if (!schedule_windows_replace(*binary, executable, &error)) {
            err << "Error: " << error << "\n";
            return 1;
        }
        temp_guard.keep();
        out << "Update scheduled. Restart your terminal after this process exits.\n";
#else
        if (!replace_unix_binary(*binary, executable, &error)) {
            err << "Error: " << error << "\n";
            return 1;
        }
        out << "Updated sighmake to " << manifest->tag << ".\n";
#endif

        return 0;
    } catch (const std::exception& exception) {
        err << "Error: updater failed safely: " << exception.what() << "\n";
        return 1;
    }
}

} // namespace updater
} // namespace vcxproj
