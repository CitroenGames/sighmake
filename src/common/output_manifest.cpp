#include "pch.h"
#include "output_manifest.hpp"
#include "target_os.hpp"
#include <cstdint>
#include <cstdio>

namespace fs = std::filesystem;

namespace vcxproj {

namespace {

std::vector<fs::path>& recorded_outputs() {
    static std::vector<fs::path> outputs;
    return outputs;
}

fs::path normalize_path(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

// Membership key of a manifest entry. Windows and macOS file systems are case-insensitive by
// default: a project renamed only in case must not delete the file that replaces it.
std::string entry_key(const std::string& relative) {
#if defined(_WIN32) || defined(__APPLE__)
    return to_lower(relative);
#else
    return relative;
#endif
}

// A manifest entry is a '/'-separated relative path that stays below the manifest directory.
bool is_safe_entry(const std::string& entry) {
    if (entry.empty()) return false;
    const fs::path path(entry);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

std::string relative_entry(const fs::path& directory, const fs::path& file) {
    const fs::path rel = file.lexically_relative(directory);
    const std::string text = rel.generic_string();
    if (text.empty() || text == "." || !is_safe_entry(text)) return {};
    return text;
}

using Sections = std::map<std::string, std::vector<std::string>>;

bool read_manifest(const fs::path& manifest_path, Sections& sections) {
    std::ifstream in(manifest_path, std::ios::binary);
    if (!in) return false;
    std::string line;
    std::string current;
    bool in_section = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            sections[current];
            in_section = true;
            continue;
        }
        if (in_section && is_safe_entry(line)) sections[current].push_back(line);
    }
    return true;
}

std::string format_manifest(const Sections& sections) {
    std::ostringstream text;
    text << "# sighmake generation manifest - auto-generated, do not edit.\n";
    text << "# Files written by the last generation, per generator and target OS. A listed file that a\n";
    text << "# later generation no longer writes is removed (sighmake --keep-stale keeps it).\n";
    for (const auto& [key, entries] : sections) {
        text << "[" << key << "]\n";
        for (const auto& entry : entries) text << entry << "\n";
    }
    return text.str();
}

bool write_if_changed(const fs::path& path, const std::string& content) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (existing == content) return true;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

std::string read_head(const fs::path& file, size_t size) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string head(size, '\0');
    in.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    return head;
}

bool ends_with_text(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// FNV-1a 64 over the file content, as 16 hex digits. Empty when the file cannot be read.
std::string content_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    uint64_t hash = 1469598103934665603ull;
    char buffer[8192];
    while (in) {
        in.read(buffer, sizeof buffer);
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    if (in.bad()) return {};
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Key of the section that holds the content hashes of a generation's files.
std::string hash_section_key(const std::string& scope_key) { return "hash " + scope_key; }

bool is_hash_section(const std::string& key) { return key.rfind("hash ", 0) == 0; }

// Hash lines are "<16 hex digits> <relative path>".
std::map<std::string, std::string> parse_hashes(const std::vector<std::string>& lines) {
    std::map<std::string, std::string> hashes;
    for (const auto& line : lines) {
        if (line.size() > 17 && line[16] == ' ') hashes[entry_key(line.substr(17))] = line.substr(0, 16);
    }
    return hashes;
}

// Fingerprint of the project and solution files the Visual Studio generator writes. They carry no
// header, so an unlisted file counts as generated only when its layout matches what sighmake writes:
// pugixml output (LF line endings, lower-case "utf-8" declaration for projects), and for .sln the
// bare layout without the SolutionGuid that Visual Studio adds when it saves a solution.
bool matches_headerless_fingerprint(const fs::path& file, const std::string& name) {
    std::string head = read_head(file, 4096);
    if (head.compare(0, 3, "\xEF\xBB\xBF") == 0) head.erase(0, 3);
    if (ends_with_text(name, ".sln")) {
        const size_t line_start = head.find_first_not_of("\r\n");
        if (line_start == std::string::npos ||
            head.compare(line_start, 37, "Microsoft Visual Studio Solution File") != 0) return false;
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return text.find("SolutionGuid") == std::string::npos &&
               text.find("HideSolutionNode = FALSE") != std::string::npos;
    }
    if (head.find('\r') != std::string::npos) return false;
    if (ends_with_text(name, ".slnx")) {
        return head.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Solution", 0) == 0;
    }
    if (ends_with_text(name, ".vcxproj") || ends_with_text(name, ".vcxproj.filters") ||
        ends_with_text(name, ".csproj")) {
        return head.rfind("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Project", 0) == 0;
    }
    return false;
}

bool is_headerless_project_file(const std::string& name) {
    return ends_with_text(name, ".sln") || ends_with_text(name, ".slnx") || ends_with_text(name, ".vcxproj") ||
           ends_with_text(name, ".vcxproj.filters") || ends_with_text(name, ".csproj");
}

void sort_unique(std::vector<std::string>& entries) {
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
}

} // namespace

void record_generated_output(const fs::path& path) {
    const fs::path normalized = normalize_path(path);
    auto& outputs = recorded_outputs();
    if (std::find(outputs.begin(), outputs.end(), normalized) == outputs.end()) {
        outputs.push_back(normalized);
    }
}

void reset_generated_outputs() {
    recorded_outputs().clear();
}

std::vector<fs::path> generated_outputs() {
    return recorded_outputs();
}

std::string target_os_manifest_name() {
    switch (target_os()) {
        case TargetOS::Windows: return "windows";
        case TargetOS::Linux: return "linux";
        case TargetOS::MacOS: return "macos";
        case TargetOS::IOS: return "ios";
        case TargetOS::Android: return "android";
    }
    return "unknown";
}

bool has_sighmake_header(const fs::path& file) {
    return read_head(file, 512).find("# Generated by sighmake") != std::string::npos;
}

bool looks_generated_by_sighmake(const fs::path& file, const std::string& relative) {
    const std::string name = fs::path(relative).filename().string();
    if (read_head(file, 512).find("Generated by sighmake") != std::string::npos) return true;
    if (is_headerless_project_file(name)) return matches_headerless_fingerprint(file, name);
    if (name == "CMakeLists.txt") return false;  // generated ones carry the header
    // Receipt, metadata and manifest copies are written without a header; Makefiles carry one.
    for (const char* extension : {".txt", ".xml", ".py", ".ps1"}) {
        if (ends_with_text(name, extension)) return true;
    }
    return false;
}

StaleOutputReport reconcile_generated_outputs(const OutputManifestScope& scope,
                                              const std::vector<fs::path>& produced,
                                              const StaleOutputOptions& options,
                                              std::ostream& out, std::ostream& err) {
    StaleOutputReport report;
    const fs::path directory = normalize_path(scope.directory);
    const fs::path manifest_path = directory / kOutputManifestFilename;
    auto display = [&](const std::string& entry) {
        return (scope.directory / fs::path(entry)).lexically_normal().generic_string();
    };

    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return report;

    Sections sections;
    report.had_previous_manifest = read_manifest(manifest_path, sections);
    const auto previous_it = sections.find(scope.key);
    const std::vector<std::string> previous =
        previous_it != sections.end() ? previous_it->second : std::vector<std::string>{};
    const bool had_own_section = previous_it != sections.end();
    const auto hash_it = sections.find(hash_section_key(scope.key));
    const std::map<std::string, std::string> previous_hashes =
        hash_it != sections.end() ? parse_hashes(hash_it->second) : std::map<std::string, std::string>{};

    // Files of this generation, relative to the manifest directory.
    std::vector<std::string> current;
    std::vector<fs::path> produced_paths;
    std::set<std::string> current_keys;
    for (const auto& file : produced) {
        const fs::path normalized = normalize_path(file);
        const std::string rel = relative_entry(directory, normalized);
        if (rel.empty() || rel == kOutputManifestFilename) continue;
        current.push_back(rel);
        current_keys.insert(entry_key(rel));
        produced_paths.push_back(normalized);
    }

    // Files another generator or target OS owns stay: they are that generation's to remove.
    std::set<std::string> other_keys;
    for (const auto& [key, entries] : sections) {
        if (key == scope.key || is_hash_section(key)) continue;
        for (const auto& entry : entries) other_keys.insert(entry_key(entry));
    }

    auto same_file_as_produced = [&](const fs::path& path) {
        for (const auto& p : produced_paths) {
            std::error_code eq_ec;
            if (fs::equivalent(path, p, eq_ec)) return true;
        }
        return false;
    };

    std::vector<std::string> carried;  // stale entries that stay listed (kept or not removable)
    for (const auto& entry : previous) {
        const std::string key = entry_key(entry);
        if (current_keys.count(key) || other_keys.count(key)) continue;
        const fs::path path = directory / fs::path(entry);
        std::error_code st_ec;
        const auto status = fs::symlink_status(path, st_ec);
        if (st_ec || !fs::exists(status)) continue;  // already gone
        if (!fs::is_regular_file(status) || same_file_as_produced(path)) continue;
        if (options.keep_stale) {
            report.kept.push_back(entry);
            carried.push_back(entry);
            continue;
        }
        // Delete only what sighmake wrote: a recorded content hash must still match; an entry of an
        // older manifest (no hash) must still look generated. Anything else is the user's file now.
        const auto recorded = previous_hashes.find(key);
        const bool still_generated = recorded != previous_hashes.end()
            ? content_hash(path) == recorded->second
            : looks_generated_by_sighmake(path, entry);
        if (!still_generated) {
            report.modified.push_back(entry);
            continue;
        }
        std::error_code rm_ec;
        if (fs::remove(path, rm_ec) && !rm_ec) {
            report.removed.push_back(entry);
        } else {
            report.failed.push_back(entry);
            carried.push_back(entry);
        }
    }

    // Probable outputs in no manifest: files that match this generator's naming patterns but were
    // not written now. Always counted; listed in full on the first generation of this generator and
    // target OS, deleted only with --prune-unlisted.
    if (scope.probable_output) {
        std::set<std::string> listed_keys = current_keys;
        listed_keys.insert(other_keys.begin(), other_keys.end());
        for (const auto& entry : previous) listed_keys.insert(entry_key(entry));
        listed_keys.insert(entry_key(kOutputManifestFilename));

        std::vector<std::pair<fs::path, std::string>> candidates;
        std::function<void(const fs::path&, int)> scan = [&](const fs::path& dir, int depth) {
            std::error_code it_ec;
            for (fs::directory_iterator it(dir, it_ec), end; !it_ec && it != end; it.increment(it_ec)) {
                std::error_code type_ec;
                const auto status = it->symlink_status(type_ec);
                if (type_ec) continue;
                if (fs::is_directory(status)) {
                    if (depth > 0) scan(it->path(), depth - 1);
                    continue;
                }
                if (!fs::is_regular_file(status)) continue;
                const std::string rel = relative_entry(directory, it->path().lexically_normal());
                if (rel.empty() || listed_keys.count(entry_key(rel))) continue;
                if (scope.probable_output(it->path(), rel)) candidates.emplace_back(it->path(), rel);
            }
        };
        scan(directory, scope.probable_search_depth);
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& [path, rel] : candidates) {
            if (same_file_as_produced(path)) continue;
            if (is_headerless_project_file(path.filename().string()) &&
                !looks_generated_by_sighmake(path, rel)) continue;
            if (options.prune_unlisted && !options.keep_stale) {
                std::error_code rm_ec;
                if (fs::remove(path, rm_ec) && !rm_ec) report.unlisted_removed.push_back(rel);
                else report.failed.push_back(rel);
            } else {
                report.unlisted.push_back(rel);
            }
        }
    }

    for (const auto& entry : report.removed) out << "Removed stale output: " << display(entry) << "\n";
    for (const auto& entry : report.unlisted_removed) out << "Removed unlisted stale output: " << display(entry) << "\n";
    for (const auto& entry : report.kept) out << "Kept stale output (--keep-stale): " << display(entry) << "\n";
    for (const auto& entry : report.modified) {
        err << "Warning: Kept stale output that is no longer a sighmake-generated file (modified or replaced): "
            << display(entry) << "\n";
    }
    for (const auto& entry : report.failed) err << "Warning: Could not remove stale output: " << display(entry) << "\n";
    const bool list_unlisted = !had_own_section || options.prune_unlisted;
    if (!report.unlisted.empty()) {
        if (list_unlisted) {
            err << "Warning: " << report.unlisted.size()
                << " probable stale output(s) are in no sighmake manifest and were left in place"
                << " (sighmake --prune-unlisted removes them):\n";
            for (const auto& entry : report.unlisted) err << "  " << display(entry) << "\n";
        } else {
            err << "Warning: " << report.unlisted.size()
                << " probable stale output(s) in no sighmake manifest remain in "
                << scope.directory.generic_string() << " (sighmake --prune-unlisted removes them)\n";
        }
    }
    const size_t removed_count = report.removed.size() + report.unlisted_removed.size();
    const size_t listed_count = report.kept.size() + report.unlisted.size() + report.modified.size() + report.failed.size();
    out << "Stale outputs: " << removed_count << " removed, " << listed_count << " listed\n";

    // New manifest: this generation's files, plus stale files that are still on disk because they
    // were kept or could not be removed (a later generation removes them).
    current.insert(current.end(), carried.begin(), carried.end());
    sort_unique(current);
    std::vector<std::string> hash_lines;
    {
        std::set<std::string> hashed;
        for (const auto& path : produced_paths) {
            const std::string rel = relative_entry(directory, path);
            if (rel.empty() || !hashed.insert(entry_key(rel)).second) continue;
            const std::string hash = content_hash(path);
            if (!hash.empty()) hash_lines.push_back(hash + " " + rel);
        }
        for (const auto& entry : carried) {  // kept files stay claimed with the hash they had
            const auto old = previous_hashes.find(entry_key(entry));
            if (old != previous_hashes.end() && hashed.insert(entry_key(entry)).second) {
                hash_lines.push_back(old->second + " " + entry);
            }
        }
    }
    sort_unique(hash_lines);
    sections[scope.key] = std::move(current);
    if (hash_lines.empty()) sections.erase(hash_section_key(scope.key));
    else sections[hash_section_key(scope.key)] = std::move(hash_lines);
    report.manifest_written = write_if_changed(manifest_path, format_manifest(sections));
    if (!report.manifest_written) {
        err << "Warning: Could not write " << manifest_path.generic_string() << "\n";
    }
    return report;
}

} // namespace vcxproj
