#include "pch.h"
#include "vcxproj_reader.hpp"
#define PUGIXML_HEADER_ONLY
#include "pugixml.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

std::string VcxprojReader::parse_condition(const std::string& condition) {
    // Parse condition like "'$(Configuration)|$(Platform)'=='Debug|Win32'"
    std::regex re(R"('==\s*'([^']+)')");
    std::smatch match;
    if (std::regex_search(condition, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

std::pair<std::string, std::string> VcxprojReader::parse_config_platform(const std::string& label) {
    size_t pos = label.find('|');
    if (pos != std::string::npos) {
        return {label.substr(0, pos), label.substr(pos + 1)};
    }
    return {label, "Win32"};
}

// Helper function to filter out VPC-related commands from build events
static std::string filter_vpc_commands(const std::string& command) {
    // Return command unchanged to preserve VPC commands
    return command;
}

// Helper function to normalize paths using std::filesystem
// Canonical form for comparing two dir-path strings: forward slashes, a single
// trailing slash. Only used to decide whether an OutDir/IntDir we just read
// back from a .vcxproj matches the default the generator would have emitted,
// so we don't round-trip a redundant outdir/intdir line into the buildscript.
static std::string path_key(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    while (p.size() >= 2 && p.back() == '/' && p[p.size() - 2] == '/') p.pop_back();
    if (!p.empty() && p.back() != '/') p.push_back('/');
    return p;
}

static bool is_default_out_dir(const std::string& value,
                               const std::string& config_key) {
    if (value.empty()) return false;
    auto [config_name, platform_name] = parse_config_key(config_key);
    return path_key(value) == path_key(default_vcxproj_out_dir(platform_name, config_name));
}

static bool is_default_int_dir(const std::string& value,
                               const std::string& config_key,
                               const std::string& project_name) {
    if (value.empty()) return false;
    auto [config_name, platform_name] = parse_config_key(config_key);
    return path_key(value) == path_key(default_vcxproj_int_dir(platform_name, config_name, project_name));
}

static std::string normalize_path(const std::string& path) {
    if (path.empty()) return path;

    try {
        namespace fs = std::filesystem;
        // Use filesystem to normalize the path
        fs::path p(path);
        std::string normalized = p.lexically_normal().string();

        // Preserve trailing slash/backslash if original had one
        if (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
            if (!normalized.empty() && normalized.back() != '/' && normalized.back() != '\\') {
                normalized += path.back();
            }
        }

        return normalized;
    } catch (...) {
        // If filesystem operation fails, return original path
        return path;
    }
}

static std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::string linked_library_key(const std::string& dependency) {
    std::string dep = trim_copy(dependency);
    if (dep.empty() || dep.find("$(") != std::string::npos || dep.find("%(") != std::string::npos) {
        return "";
    }

    fs::path dep_path(dep);
    std::string ext = to_lower(dep_path.extension().string());
    if (ext != ".lib") {
        return "";
    }

    return to_lower(dep_path.stem().string());
}

static void add_library_project_key(std::map<std::string, std::string>& library_to_project,
                                    const std::string& key,
                                    const std::string& project_name) {
    if (key.empty()) return;

    auto [it, inserted] = library_to_project.emplace(to_lower(key), project_name);
    if (!inserted && it->second != project_name) {
        it->second.clear(); // Ambiguous library name.
    }
}

static bool has_project_reference(const Project& project, const std::string& dependency_name) {
    return std::any_of(project.project_references.begin(),
                       project.project_references.end(),
                       [&](const ProjectDependency& dep) { return dep.name == dependency_name; });
}

static std::string normalized_abs_path_key(const fs::path& path) {
    auto value = fs::absolute(path).lexically_normal().string();
    std::replace(value.begin(), value.end(), '/', '\\');
    return to_lower(value);
}

static bool path_contains_build_macro(const std::string& path) {
    return path.find("$(") != std::string::npos ||
           path.find("%(") != std::string::npos ||
           path.find("%") != std::string::npos;
}

static fs::path resolve_from_dir(const fs::path& base_dir, const std::string& path) {
    fs::path resolved(path);
    if (!resolved.is_absolute()) {
        resolved = base_dir / resolved;
    }
    return resolved.lexically_normal();
}

static std::string relative_path_string(const fs::path& target, const fs::path& from) {
    std::error_code ec;
    fs::path relative = fs::relative(target, from, ec);
    if (ec) {
        relative = target;
    }
    auto value = relative.lexically_normal().string();
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}

static std::vector<std::string> read_include_directives(const fs::path& file_path) {
    std::vector<std::string> includes;
    std::ifstream file(file_path);
    if (!file.is_open()) return includes;

    static const std::regex include_re(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
    std::string line;
    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_search(line, match, include_re) && match.size() > 1) {
            std::string include = trim_copy(match[1].str());
            if (!include.empty() && include.find('$') == std::string::npos) {
                includes.push_back(include);
            }
        }
    }

    return includes;
}

static bool include_exists_under(const fs::path& root, const std::string& include_path) {
    std::error_code ec;
    return fs::exists((root / fs::path(include_path)).lexically_normal(), ec);
}

static bool config_has_include_root(const Configuration& cfg,
                                    const fs::path& project_dir,
                                    const fs::path& include_root) {
    const auto target_key = normalized_abs_path_key(include_root);
    for (const auto& dir : cfg.cl_compile.additional_include_directories) {
        if (dir.empty() || path_contains_build_macro(dir)) continue;
        if (normalized_abs_path_key(resolve_from_dir(project_dir, dir)) == target_key) {
            return true;
        }
    }
    return false;
}

static bool include_resolves_for_config(const Configuration& cfg,
                                        const fs::path& project_dir,
                                        const fs::path& source_dir,
                                        const std::string& include_path) {
    if (include_exists_under(source_dir, include_path)) {
        return true;
    }

    for (const auto& dir : cfg.cl_compile.additional_include_directories) {
        if (dir.empty() || path_contains_build_macro(dir)) continue;
        if (include_exists_under(resolve_from_dir(project_dir, dir), include_path)) {
            return true;
        }
    }

    return false;
}

static std::vector<fs::path> common_solution_include_roots(const fs::path& solution_dir) {
    std::vector<fs::path> roots = {
        solution_dir / "deps" / "include",
        solution_dir / "dependencies" / "include",
        solution_dir / "external" / "include",
        solution_dir / "extern" / "include",
        solution_dir / "third_party" / "include",
        solution_dir / "thirdparty" / "include",
        solution_dir / "vendor" / "include",
        solution_dir / "include",
    };

    std::vector<fs::path> existing;
    for (auto& root : roots) {
        std::error_code ec;
        if (fs::is_directory(root, ec)) {
            existing.push_back(root.lexically_normal());
        }
    }
    return existing;
}

static void infer_solution_local_include_dirs(Solution& solution, const fs::path& solution_dir) {
    auto include_roots = common_solution_include_roots(solution_dir);
    if (include_roots.empty()) return;

    for (auto& project : solution.projects) {
        if (project.vcxproj_path.empty()) continue;

        fs::path vcxproj_path = fs::path(project.vcxproj_path);
        if (!vcxproj_path.is_absolute()) {
            vcxproj_path = solution_dir / vcxproj_path;
        }
        const fs::path project_dir = vcxproj_path.lexically_normal().parent_path();

        struct IncludeUse {
            fs::path source_dir;
            std::string include_path;
        };

        std::vector<IncludeUse> include_uses;
        for (const auto& source : project.sources) {
            if (source.type != FileType::ClCompile && source.type != FileType::ClInclude) {
                continue;
            }
            if (source.path.empty() || path_contains_build_macro(source.path)) {
                continue;
            }

            fs::path source_path = resolve_from_dir(project_dir, source.path);
            std::error_code ec;
            if (!fs::is_regular_file(source_path, ec)) {
                continue;
            }

            auto includes = read_include_directives(source_path);
            for (const auto& include : includes) {
                include_uses.push_back({source_path.parent_path(), include});
            }
        }

        if (include_uses.empty()) continue;

        for (auto& [config_key, cfg] : project.configurations) {
            std::vector<std::string> added_dirs;

            for (const auto& use : include_uses) {
                if (include_resolves_for_config(cfg, project_dir, use.source_dir, use.include_path)) {
                    continue;
                }

                for (const auto& include_root : include_roots) {
                    if (!include_exists_under(include_root, use.include_path)) {
                        continue;
                    }
                    if (config_has_include_root(cfg, project_dir, include_root)) {
                        continue;
                    }

                    std::string relative = relative_path_string(include_root, project_dir);
                    cfg.cl_compile.additional_include_directories.push_back(relative);
                    added_dirs.push_back(relative);
                    break;
                }
            }

            for (const auto& dir : added_dirs) {
                std::cout << "  " << project.name << " inferred include dir for "
                          << config_key << ": " << dir << "\n";
            }
        }
    }
}

static void infer_project_references_from_link_libraries(Solution& solution) {
    std::map<std::string, std::string> library_to_project;

    for (const auto& project : solution.projects) {
        bool produces_library = false;
        for (const auto& [_, cfg] : project.configurations) {
            if (cfg.config_type == "StaticLibrary" || cfg.config_type == "DynamicLibrary") {
                produces_library = true;
                add_library_project_key(library_to_project, cfg.target_name.empty() ? project.name : cfg.target_name, project.name);
            }
        }

        if (produces_library) {
            add_library_project_key(library_to_project, project.name, project.name);
            add_library_project_key(library_to_project, project.project_name, project.name);
        }
    }

    for (auto& project : solution.projects) {
        for (const auto& [_, cfg] : project.configurations) {
            for (const auto& dependency : cfg.link.additional_dependencies) {
                auto it = library_to_project.find(linked_library_key(dependency));
                if (it == library_to_project.end() || it->second.empty() || it->second == project.name) {
                    continue;
                }
                if (!has_project_reference(project, it->second)) {
                    project.project_references.push_back(ProjectDependency(it->second));
                    std::cout << "  " << project.name << " inferred dependency from "
                              << dependency << " -> " << it->second << "\n";
                }
            }
        }
    }
}

// Helper function to normalize paths within build event commands
// For 100% accuracy, we preserve paths exactly as they appear
static std::string normalize_command_paths(const std::string& command) {
    // Return command unchanged to preserve exact path formatting
    return command;
}

// Helper to check if a path looks like a file path (not a macro or argument)
static bool looks_like_file_path(const std::string& token) {
    // Skip MSBuild macros
    if (token.find("$(") != std::string::npos || token.find("%(") != std::string::npos)
        return false;
    // Skip shell operators
    if (token == ">" || token == "<" || token == "|" || token == "||" || token == "&&")
        return false;
    // Check if it has path separators or file extension
    return (token.find('\\') != std::string::npos ||
            token.find('/') != std::string::npos ||
            (token.find('.') != std::string::npos && token.find(".exe") != std::string::npos) ||
            (token.find('.') != std::string::npos && token.find(".pl") != std::string::npos) ||
            (token.find('.') != std::string::npos && token.find(".py") != std::string::npos));
}

// Adjust relative file paths in a custom build command
// Currently unused but kept for potential future use
[[maybe_unused]] static std::string adjust_command_paths(const std::string& command,
                                       const std::string& from_dir,
                                       const std::string& to_dir) {
    namespace fs = std::filesystem;

    if (command.empty()) return command;

    // Split command by spaces (simple tokenization)
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;

    for (char c : command) {
        if (c == '"') {
            in_quotes = !in_quotes;
            current_token += c;
        } else if (c == ' ' && !in_quotes) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token += c;
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    // Process each token
    std::string result;
    for (const auto& token : tokens) {
        if (!result.empty()) result += " ";

        // Check if this looks like a relative file path
        if (looks_like_file_path(token)) {
            try {
                // Remove quotes if present
                std::string clean_token = token;
                bool had_quotes = false;
                if (clean_token.size() >= 2 && clean_token.front() == '"' && clean_token.back() == '"') {
                    clean_token = clean_token.substr(1, clean_token.size() - 2);
                    had_quotes = true;
                }

                fs::path p(clean_token);
                // Only process relative paths
                if (!p.is_absolute()) {
                    // Make absolute relative to from_dir
                    fs::path abs_path = fs::absolute(fs::path(from_dir) / p);
                    // Make relative to to_dir
                    fs::path rel_path = fs::relative(abs_path, to_dir);

                    std::string new_path = rel_path.string();
                    std::replace(new_path.begin(), new_path.end(), '/', '\\');

                    if (had_quotes) {
                        result += "\"" + new_path + "\"";
                    } else {
                        result += new_path;
                    }
                } else {
                    result += token;
                }
            } catch (...) {
                // If path processing fails, keep original
                result += token;
            }
        } else {
            result += token;
        }
    }

    return result;
}

// Structure to hold property sheet settings
struct PropSheetSettings {
    std::vector<std::string> preprocessor_definitions;
    std::vector<std::string> additional_include_directories;
};

// Read a .props file and extract ItemDefinitionGroup settings
static PropSheetSettings read_props_file(const std::string& filepath) {
    PropSheetSettings settings;
    pugi::xml_document doc;

    if (!doc.load_file(filepath.c_str())) {
        std::cerr << "Warning: Failed to load property sheet: " << filepath << std::endl;
        return settings;
    }

    auto root = doc.child("Project");
    if (!root) return settings;

    // Parse ItemDefinitionGroup
    for (auto item_def_group : root.children("ItemDefinitionGroup")) {
        if (auto cl = item_def_group.child("ClCompile")) {
            // Parse PreprocessorDefinitions
            if (auto n = cl.child("PreprocessorDefinitions")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty() && item.find("%(") != 0) {
                        settings.preprocessor_definitions.push_back(item);
                    }
                }
            }

            // Parse AdditionalIncludeDirectories
            if (auto n = cl.child("AdditionalIncludeDirectories")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty() && item.find("%(") != 0) {
                        settings.additional_include_directories.push_back(item);
                    }
                }
            }
        }
    }

    return settings;
}

// Evaluate an MSBuild-style exists() condition on an Import element.
// Returns true if the import should proceed, false if it should be skipped.
static bool should_import(const std::string& condition, const fs::path& base_dir) {
    if (condition.empty()) return true;

    std::regex exists_re(R"(exists\(\s*'([^']+)'\s*\))");
    std::smatch match;
    if (std::regex_search(condition, match, exists_re) && match.size() > 1) {
        std::string path_str = match[1].str();
        if (path_str.find("$(") != std::string::npos) return true;
        fs::path resolved = (base_dir / path_str).lexically_normal();
        return fs::exists(resolved);
    }
    return true;
}

Project VcxprojReader::read_vcxproj(const std::string& filepath) {
    Project project;
    pugi::xml_document doc;

    pugi::xml_parse_result result = doc.load_file(filepath.c_str());
    if (!result) {
        throw std::runtime_error("Failed to parse vcxproj: " + std::string(result.description()));
    }

    auto root = doc.child("Project");
    if (!root) {
        throw std::runtime_error("Invalid vcxproj file: no Project root element");
    }

    // Parse Globals
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string label = prop_group.attribute("Label").as_string();
        if (label == "Globals") {
            project.project_name = prop_group.child_value("ProjectName");
            std::string guid = prop_group.child_value("ProjectGuid");
            // Remove braces from GUID
            if (!guid.empty() && guid.front() == '{') {
                guid = guid.substr(1, guid.length() - 2);
            }
            project.uuid = guid;
            project.root_namespace = prop_group.child_value("RootNamespace");
            project.ignore_warn_compile_duplicated_filename =
                prop_group.child("IgnoreWarnCompileDuplicatedFilename").text().as_bool();
            break;
        }
    }

    // Parse configurations
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string label = prop_group.attribute("Label").as_string();
        std::string condition = prop_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (label == "Configuration" && !config_key.empty()) {
            auto& cfg = project.configurations[config_key];
            cfg.config_type = prop_group.child_value("ConfigurationType");
            cfg.platform_toolset = prop_group.child_value("PlatformToolset");
            cfg.windows_target_platform_version = prop_group.child_value("WindowsTargetPlatformVersion");
            cfg.character_set = prop_group.child_value("CharacterSet");
            cfg.use_debug_libraries = prop_group.child("UseDebugLibraries").text().as_bool();
            cfg.whole_program_optimization = prop_group.child("WholeProgramOptimization").text().as_bool();
            cfg.use_of_mfc = prop_group.child_value("UseOfMfc");
            cfg.use_of_atl = prop_group.child_value("UseOfAtl");
            // Read TargetName from Configuration PropertyGroup
            if (auto node = prop_group.child("TargetName"))
                cfg.target_name = node.text().as_string();
        }
    }

    // Parse PropertyGroup settings (OutDir, IntDir, LinkIncremental, etc.)
    for (auto prop_group : root.children("PropertyGroup")) {
        std::string condition = prop_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (!config_key.empty() && project.configurations.count(config_key)) {
            auto& cfg = project.configurations[config_key];

            if (auto node = prop_group.child("OutDir")) {
                std::string v = normalize_path(node.text().as_string());
                if (!is_default_out_dir(v, config_key)) cfg.out_dir = v;
            }
            if (auto node = prop_group.child("IntDir")) {
                std::string v = normalize_path(node.text().as_string());
                {
                    const std::string& pname = project.name.empty() ? project.project_name : project.name;
                    if (!is_default_int_dir(v, config_key, pname)) cfg.int_dir = v;
                }
            }
            if (auto node = prop_group.child("TargetName"))
                cfg.target_name = node.text().as_string();
            if (auto node = prop_group.child("TargetExt"))
                cfg.target_ext = node.text().as_string();
            if (auto node = prop_group.child("LinkIncremental"))
                cfg.link_incremental = node.text().as_bool();
            if (auto node = prop_group.child("ExecutablePath"))
                cfg.executable_path = node.text().as_string();
            if (auto node = prop_group.child("GenerateManifest"))
                cfg.generate_manifest = node.text().as_bool();
            if (auto node = prop_group.child("IgnoreImportLibrary"))
                cfg.ignore_import_library = node.text().as_bool();
            if (auto node = prop_group.child("ImportLibrary"))
                cfg.import_library = node.text().as_string();
            if (auto node = prop_group.child("PreBuildEventUseInBuild"))
                cfg.pre_build_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PreLinkEventUseInBuild"))
                cfg.pre_link_event.use_in_build = node.text().as_bool();
            if (auto node = prop_group.child("PostBuildEventUseInBuild"))
                cfg.post_build_event.use_in_build = node.text().as_bool();
        }

        // Also handle PropertyGroup elements without a Condition, where individual
        // child elements have their own Condition attributes
        if (config_key.empty()) {
            for (auto node : prop_group.children()) {
                std::string node_condition = node.attribute("Condition").as_string();
                std::string node_config_key = parse_condition(node_condition);

                if (!node_config_key.empty() && project.configurations.count(node_config_key)) {
                    auto& cfg = project.configurations[node_config_key];
                    std::string node_name = node.name();

                    if (node_name == "OutDir") {
                        std::string v = normalize_path(node.text().as_string());
                        if (!is_default_out_dir(v, node_config_key)) cfg.out_dir = v;
                    }
                    else if (node_name == "IntDir") {
                        std::string v = normalize_path(node.text().as_string());
                        const std::string& pname = project.name.empty() ? project.project_name : project.name;
                        if (!is_default_int_dir(v, node_config_key, pname)) cfg.int_dir = v;
                    }
                    else if (node_name == "TargetName")
                        cfg.target_name = node.text().as_string();
                    else if (node_name == "TargetExt")
                        cfg.target_ext = node.text().as_string();
                    else if (node_name == "LinkIncremental")
                        cfg.link_incremental = node.text().as_bool();
                    else if (node_name == "ExecutablePath")
                        cfg.executable_path = node.text().as_string();
                    else if (node_name == "GenerateManifest")
                        cfg.generate_manifest = node.text().as_bool();
                    else if (node_name == "PreBuildEventUseInBuild")
                        cfg.pre_build_event.use_in_build = node.text().as_bool();
                    else if (node_name == "PreLinkEventUseInBuild")
                        cfg.pre_link_event.use_in_build = node.text().as_bool();
                    else if (node_name == "PostBuildEventUseInBuild")
                        cfg.post_build_event.use_in_build = node.text().as_bool();
                }
            }
        }
    }

    // Parse ImportGroup sections to find property sheets and accumulate their settings
    std::map<std::string, PropSheetSettings> prop_sheet_settings;
    fs::path vcxproj_dir = fs::path(filepath).parent_path();

    for (auto import_group : root.children("ImportGroup")) {
        std::string label = import_group.attribute("Label").as_string();
        if (label != "PropertySheets") continue;

        // Get configuration condition
        std::string condition = import_group.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        // Read each imported property sheet
        PropSheetSettings& settings = prop_sheet_settings[config_key];

        for (auto import_node : import_group.children("Import")) {
            std::string props_path = import_node.attribute("Project").as_string();

            // Skip MSBuild system property sheets (they contain MSBuild variables we can't resolve)
            if (props_path.find("$(") != std::string::npos) continue;

            // Check Condition attribute (e.g., exists('game.props'))
            std::string import_condition = import_node.attribute("Condition").as_string();
            if (!should_import(import_condition, vcxproj_dir)) continue;

            // Resolve relative path
            fs::path abs_props_path = vcxproj_dir / props_path;
            abs_props_path = abs_props_path.lexically_normal();

            // Read the property sheet
            PropSheetSettings props_settings = read_props_file(abs_props_path.string());

            // Accumulate settings
            settings.preprocessor_definitions.insert(
                settings.preprocessor_definitions.end(),
                props_settings.preprocessor_definitions.begin(),
                props_settings.preprocessor_definitions.end()
            );
            settings.additional_include_directories.insert(
                settings.additional_include_directories.end(),
                props_settings.additional_include_directories.begin(),
                props_settings.additional_include_directories.end()
            );
        }
    }

    // Parse ItemDefinitionGroup (compiler, linker, etc. settings)
    for (auto item_def : root.children("ItemDefinitionGroup")) {
        std::string condition = item_def.attribute("Condition").as_string();
        std::string config_key = parse_condition(condition);

        if (config_key.empty() || !project.configurations.count(config_key)) continue;

        auto& cfg = project.configurations[config_key];

        // ClCompile settings
        if (auto cl = item_def.child("ClCompile")) {
            auto& settings = cfg.cl_compile;

            #define READ_TEXT(name, field) if (auto n = cl.child(name)) settings.field = n.text().as_string()
            #define READ_BOOL(name, field) if (auto n = cl.child(name)) settings.field = n.text().as_bool()
            #define READ_VECTOR(name, field) if (auto n = cl.child(name)) { \
                std::string val = n.text().as_string(); \
                std::istringstream ss(val); std::string item; \
                while (std::getline(ss, item, ';')) { \
                    if (!item.empty() && item.find("%(") != 0) { \
                        settings.field.push_back(item); \
                    } \
                } \
            }

            READ_TEXT("Optimization", optimization);
            READ_TEXT("InlineFunctionExpansion", inline_function_expansion);
            READ_BOOL("IntrinsicFunctions", intrinsic_functions);
            READ_TEXT("FavorSizeOrSpeed", favor_size_or_speed);

            // Parse AdditionalIncludeDirectories with %(AdditionalIncludeDirectories) expansion
            if (auto n = cl.child("AdditionalIncludeDirectories")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) {
                        if (item == "%(AdditionalIncludeDirectories)") {
                            // Expand to property sheet includes for this config
                            if (prop_sheet_settings.count(config_key)) {
                                for (const auto& inc : prop_sheet_settings[config_key].additional_include_directories) {
                                    settings.additional_include_directories.push_back(inc);
                                }
                            }
                        } else {
                            settings.additional_include_directories.push_back(item);
                        }
                    }
                }
            }

            // Parse PreprocessorDefinitions with %(PreprocessorDefinitions) expansion
            if (auto n = cl.child("PreprocessorDefinitions")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) {
                        if (item == "%(PreprocessorDefinitions)") {
                            // Expand to property sheet defines for this config
                            if (prop_sheet_settings.count(config_key)) {
                                for (const auto& def : prop_sheet_settings[config_key].preprocessor_definitions) {
                                    settings.preprocessor_definitions.push_back(def);
                                }
                            }
                        } else {
                            settings.preprocessor_definitions.push_back(item);
                        }
                    }
                }
            }

            READ_VECTOR("ForcedIncludeFiles", forced_include_files);
            READ_BOOL("StringPooling", string_pooling);
            READ_BOOL("MinimalRebuild", minimal_rebuild);
            READ_TEXT("ExceptionHandling", exception_handling);
            READ_TEXT("BasicRuntimeChecks", basic_runtime_checks);
            READ_TEXT("RuntimeLibrary", runtime_library);
            READ_BOOL("BufferSecurityCheck", buffer_security_check);
            READ_BOOL("FunctionLevelLinking", function_level_linking);
            READ_TEXT("EnableEnhancedInstructionSet", enhanced_instruction_set);
            READ_TEXT("FloatingPointModel", floating_point_model);
            READ_BOOL("ForceConformanceInForLoopScope", force_conformance_in_for_loop_scope);
            READ_BOOL("RuntimeTypeInfo", runtime_type_info);
            READ_TEXT("PrecompiledHeader", pch.mode);
            READ_TEXT("PrecompiledHeaderFile", pch.header);
            READ_TEXT("PrecompiledHeaderOutputFile", pch.output);
            READ_TEXT("AssemblerListingLocation", assembler_listing_location);
            READ_TEXT("ObjectFileName", object_file_name);
            READ_TEXT("ProgramDataBaseFileName", program_database_file_name);
            READ_BOOL("GenerateXMLDocumentationFiles", generate_xml_documentation_files);
            READ_BOOL("BrowseInformation", browse_information);
            READ_TEXT("BrowseInformationFile", browse_information_file);
            READ_TEXT("WarningLevel", warning_level);
            READ_TEXT("DebugInformationFormat", debug_information_format);
            READ_TEXT("CompileAs", compile_as);
            READ_VECTOR("DisableSpecificWarnings", disable_specific_warnings);
            READ_BOOL("MultiProcessorCompilation", multi_processor_compilation);
            READ_TEXT("ErrorReporting", error_reporting);
            READ_TEXT("AdditionalOptions", additional_options);
            READ_TEXT("LanguageStandard", language_standard);

            // Read C standard (LanguageStandard_C) and convert to sighmake format
            if (auto c_std_node = cl.child("LanguageStandard_C")) {
                std::string c_std = c_std_node.text().as_string();
                // Convert MSVC format to sighmake format
                if (c_std == "stdc89" || c_std == "stdc90") project.c_standard = "89";
                else if (c_std == "stdc11") project.c_standard = "11";
                else if (c_std == "stdc17") project.c_standard = "17";
            }

            READ_BOOL("TreatWChar_tAsBuiltInType", treat_wchar_t_as_built_in_type);
            READ_TEXT("AssemblerOutput", assembler_output);
            READ_BOOL("ExpandAttributedSource", expand_attributed_source);
            READ_BOOL("OpenMPSupport", openmp_support);
            READ_BOOL("TreatWarningAsError", treat_warning_as_error);

            // Check if AdditionalOptions contains /utf-8 flag
            if (settings.additional_options.find("/utf-8") != std::string::npos) {
                settings.utf8_source = true;
            }

            #undef READ_TEXT
            #undef READ_BOOL
            #undef READ_VECTOR
        }

        // Link settings
        if (auto link = item_def.child("Link")) {
            auto& settings = cfg.link;

            #define READ_TEXT(name, field) if (auto n = link.child(name)) settings.field = n.text().as_string()
            #define READ_BOOL(name, field) if (auto n = link.child(name)) settings.field = n.text().as_bool()
            #define READ_VECTOR(name, field) if (auto n = link.child(name)) { \
                std::string val = n.text().as_string(); \
                std::istringstream ss(val); std::string item; \
                while (std::getline(ss, item, ';')) { \
                    if (!item.empty() && item.find("%(") != 0) { \
                        settings.field.push_back(item); \
                    } \
                } \
            }

            READ_TEXT("ShowProgress", show_progress);
            READ_TEXT("OutputFile", output_file);
            // Normalize the output file path to remove redundant \.\ segments
            if (!settings.output_file.empty()) {
                settings.output_file = normalize_path(settings.output_file);
            }
            READ_BOOL("SuppressStartupBanner", suppress_startup_banner);
            READ_VECTOR("AdditionalDependencies", additional_dependencies);
            READ_VECTOR("AdditionalLibraryDirectories", additional_library_directories);
            READ_VECTOR("IgnoreSpecificDefaultLibraries", ignore_specific_default_libraries);
            READ_BOOL("IgnoreAllDefaultLibraries", ignore_all_default_libraries);
            READ_TEXT("ModuleDefinitionFile", module_definition_file);
            READ_BOOL("GenerateDebugInformation", generate_debug_info);
            READ_TEXT("ProgramDatabaseFile", program_database_file);
            READ_TEXT("SubSystem", sub_system);
            READ_BOOL("OptimizeReferences", optimize_references);
            READ_BOOL("EnableCOMDATFolding", enable_comdat_folding);
            READ_BOOL("GenerateMapFile", generate_map_file);
            READ_TEXT("MapFileName", map_file_name);
            READ_TEXT("BaseAddress", base_address);
            READ_TEXT("TargetMachine", target_machine);
            READ_TEXT("LinkErrorReporting", error_reporting);
            READ_BOOL("ImageHasSafeExceptionHandlers", image_has_safe_exception_handlers);
            READ_TEXT("AdditionalOptions", additional_options);
            READ_TEXT("EntryPointSymbol", entry_point_symbol);
            READ_TEXT("Version", version);
            READ_BOOL("FixedBaseAddress", fixed_base_address);
            READ_BOOL("LargeAddressAware", large_address_aware);

            // For RandomizedBaseAddress, need manual handling since it's std::optional<bool>
            if (auto rba_node = link.child("RandomizedBaseAddress")) {
                std::string val = rba_node.text().as_string();
                cfg.link.randomized_base_address = (val == "true");
            }

            #undef READ_TEXT
            #undef READ_BOOL
            #undef READ_VECTOR
        }

        // Lib settings (for static libraries)
        if (auto lib = item_def.child("Lib")) {
            auto& settings = cfg.lib;

            if (auto n = lib.child("OutputFile"))
                settings.output_file = normalize_path(n.text().as_string());
            if (auto n = lib.child("SuppressStartupBanner"))
                settings.suppress_startup_banner = n.text().as_bool();
            if (auto n = lib.child("UseUnicodeResponseFiles"))
                settings.use_unicode_response_files = n.text().as_bool();
            if (auto n = lib.child("AdditionalOptions"))
                settings.additional_options = n.text().as_string();
            if (auto n = lib.child("AdditionalDependencies")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) settings.additional_dependencies.push_back(item);
                }
            }
        }

        // ResourceCompile settings
        if (auto rc = item_def.child("ResourceCompile")) {
            auto& settings = cfg.resource_compile;

            if (auto n = rc.child("PreprocessorDefinitions")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) {
                        // Resolve %(PreprocessorDefinitions) macro by substituting ClCompile defines
                        if (item == "%(PreprocessorDefinitions)") {
                            // Use the ClCompile preprocessor definitions from this same config
                            for (const auto& def : cfg.cl_compile.preprocessor_definitions) {
                                settings.preprocessor_definitions.push_back(def);
                            }
                        } else {
                            settings.preprocessor_definitions.push_back(item);
                        }
                    }
                }
            }
            if (auto n = rc.child("Culture"))
                settings.culture = n.text().as_string();
            if (auto n = rc.child("AdditionalIncludeDirectories")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) settings.additional_include_directories.push_back(item);
                }
            }
        }

        // MessageCompile settings
        if (auto mc = item_def.child("MessageCompile")) {
            if (auto n = mc.child("HeaderFilePath"))
                cfg.mc.header_file_path = n.text().as_string();
            if (auto n = mc.child("RCFilePath"))
                cfg.mc.rc_file_path = n.text().as_string();
            if (auto n = mc.child("AdditionalOptions"))
                cfg.mc.additional_options = n.text().as_string();
        }

        // Midl settings
        if (auto midl = item_def.child("Midl")) {
            if (auto n = midl.child("OutputDirectory"))
                cfg.midl.output_directory = n.text().as_string();
            if (auto n = midl.child("HeaderFileName"))
                cfg.midl.header_file_name = n.text().as_string();
            if (auto n = midl.child("TypeLibraryName"))
                cfg.midl.type_library_name = n.text().as_string();
            if (auto n = midl.child("DllDataFileName"))
                cfg.midl.dlldata_file_name = n.text().as_string();
            if (auto n = midl.child("InterfaceIdentifierFileName"))
                cfg.midl.interface_identifier_file_name = n.text().as_string();
            if (auto n = midl.child("ProxyFileName"))
                cfg.midl.proxy_file_name = n.text().as_string();
            if (auto n = midl.child("PreprocessorDefinitions")) {
                std::string val = n.text().as_string();
                std::istringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) cfg.midl.preprocessor_definitions.push_back(item);
                }
            }
            if (auto n = midl.child("AdditionalOptions"))
                cfg.midl.additional_options = n.text().as_string();
            if (auto n = midl.child("DefaultCharType"))
                cfg.midl.default_char_type = n.text().as_string();
            if (auto n = midl.child("TargetEnvironment"))
                cfg.midl.target_environment = n.text().as_string();
        }

        // Manifest settings
        if (auto manifest = item_def.child("Manifest")) {
            if (auto n = manifest.child("SuppressStartupBanner"))
                cfg.manifest.suppress_startup_banner = n.text().as_bool();
            if (auto n = manifest.child("AdditionalManifestFiles"))
                cfg.manifest.additional_manifest_files = n.text().as_string();
        }

        // Xdcmake settings
        if (auto xdcmake = item_def.child("Xdcmake")) {
            if (auto n = xdcmake.child("SuppressStartupBanner"))
                cfg.xdcmake.suppress_startup_banner = n.text().as_bool();
        }

        // Bscmake settings
        if (auto bscmake = item_def.child("Bscmake")) {
            if (auto n = bscmake.child("SuppressStartupBanner"))
                cfg.bscmake.suppress_startup_banner = n.text().as_bool();
            if (auto n = bscmake.child("OutputFile"))
                cfg.bscmake.output_file = normalize_path(n.text().as_string());
        }

        // Build events (filter out VPC-related commands and normalize paths)
        if (auto pre_build = item_def.child("PreBuildEvent")) {
            if (auto n = pre_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.pre_build_event.command = normalize_command_paths(cmd);
            }
            if (auto n = pre_build.child("Message"))
                cfg.pre_build_event.message = n.text().as_string();
        }
        if (auto pre_link = item_def.child("PreLinkEvent")) {
            if (auto n = pre_link.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.pre_link_event.command = normalize_command_paths(cmd);
            }
            if (auto n = pre_link.child("Message"))
                cfg.pre_link_event.message = n.text().as_string();
        }
        if (auto post_build = item_def.child("PostBuildEvent")) {
            if (auto n = post_build.child("Command")) {
                std::string cmd = n.text().as_string();
                cmd = filter_vpc_commands(cmd);
                cfg.post_build_event.command = normalize_command_paths(cmd);
            }
            if (auto n = post_build.child("Message"))
                cfg.post_build_event.message = n.text().as_string();
        }
    }

    // Parse source files
    for (auto item_group : root.children("ItemGroup")) {
        for (auto file_elem : item_group.children()) {
            std::string elem_name = file_elem.name();
            if (elem_name != "ClCompile" && elem_name != "ClInclude" &&
                elem_name != "ResourceCompile" && elem_name != "CustomBuild" &&
                elem_name != "MessageCompile" && elem_name != "Midl" &&
                elem_name != "None") {
                continue;
            }

            SourceFile src;
            src.path = file_elem.attribute("Include").as_string();

            if (elem_name == "ClCompile") src.type = FileType::ClCompile;
            else if (elem_name == "ClInclude") src.type = FileType::ClInclude;
            else if (elem_name == "ResourceCompile") src.type = FileType::ResourceCompile;
            else if (elem_name == "CustomBuild") src.type = FileType::CustomBuild;
            else if (elem_name == "MessageCompile") {
                src.type = FileType::MessageCompile;
                project.has_mc_files = true;
            }
            else if (elem_name == "Midl") {
                src.type = FileType::Midl;
                project.has_idl_files = true;
            }
            else src.type = FileType::None;

            // Parse per-file settings
            for (auto child : file_elem.children()) {
                std::string name = child.name();
                std::string condition = child.attribute("Condition").as_string();
                std::string config_key = condition.empty() ? ALL_CONFIGS : parse_condition(condition);

                if (name == "ExcludedFromBuild") {
                    src.settings.excluded[config_key] = child.text().as_bool();
                } else if (name == "ObjectFileName") {
                    src.settings.object_file[config_key] = child.text().as_string();
                } else if (name == "AdditionalIncludeDirectories") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        if (!item.empty()) {
                            // Resolve %(AdditionalIncludeDirectories) macro by substituting project-level includes
                            if (item == "%(AdditionalIncludeDirectories)") {
                                // Get project-level includes for this config
                                if (project.configurations.count(config_key)) {
                                    const auto& project_includes = project.configurations.at(config_key).cl_compile.additional_include_directories;
                                    for (const auto& inc : project_includes) {
                                        src.settings.additional_includes[config_key].push_back(inc);
                                    }
                                }
                            } else {
                                src.settings.additional_includes[config_key].push_back(item);
                            }
                        }
                    }
                } else if (name == "PreprocessorDefinitions") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        if (!item.empty()) {
                            // Resolve %(PreprocessorDefinitions) macro by substituting project-level defines
                            if (item == "%(PreprocessorDefinitions)") {
                                // Get project-level defines for this config
                                if (project.configurations.count(config_key)) {
                                    const auto& project_defines = project.configurations.at(config_key).cl_compile.preprocessor_definitions;
                                    for (const auto& def : project_defines) {
                                        src.settings.preprocessor_defines[config_key].push_back(def);
                                    }
                                }
                            } else {
                                src.settings.preprocessor_defines[config_key].push_back(item);
                            }
                        }
                    }
                } else if (name == "AdditionalOptions") {
                    std::string val = child.text().as_string();
                    std::istringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ' ')) {
                        if (!item.empty()) src.settings.additional_options[config_key].push_back(item);
                    }
                } else if (name == "PrecompiledHeader") {
                    src.settings.pch[config_key].mode = child.text().as_string();
                } else if (name == "PrecompiledHeaderFile") {
                    src.settings.pch[config_key].header = child.text().as_string();
                } else if (name == "PrecompiledHeaderOutputFile") {
                    src.settings.pch[config_key].output = child.text().as_string();
                } else if (name == "CompileAs") {
                    src.settings.compile_as[config_key] = child.text().as_string();
                } else if (name == "Optimization") {
                    src.settings.optimization[config_key] = child.text().as_string();
                } else if (name == "Command") {
                    src.custom_command[config_key] = child.text().as_string();
                } else if (name == "Message") {
                    src.custom_message[config_key] = child.text().as_string();
                } else if (name == "Outputs") {
                    src.custom_outputs[config_key] = child.text().as_string();
                } else if (name == "AdditionalInputs") {
                    src.custom_inputs[config_key] = child.text().as_string();
                }
            }

            project.sources.push_back(src);
        }

        // Parse library references
        for (auto lib : item_group.children("Library")) {
            std::string include = lib.attribute("Include").as_string();
            if (!include.empty()) {
                LibraryFile lf;
                lf.path = include;

                // Parse per-library settings (like ExcludedFromBuild)
                for (auto child : lib.children()) {
                    std::string name = child.name();
                    if (name == "ExcludedFromBuild") {
                        std::string condition = child.attribute("Condition").as_string();
                        std::string config_key = condition.empty() ? ALL_CONFIGS : parse_condition(condition);
                        lf.excluded[config_key] = child.text().as_bool();
                    }
                }

                project.libraries.push_back(lf);
            }
        }

        // Parse project references
        for (auto ref : item_group.children("ProjectReference")) {
            std::string include = ref.attribute("Include").as_string();
            // Extract project name from path (remove .vcxproj extension)
            fs::path p(include);
            std::string proj_name = p.stem().string();
            project.project_references.push_back(ProjectDependency(proj_name));
        }
    }

    // Detect project language from CompileAs settings and file extensions if not explicitly set
    if (project.language.empty()) {
        int c_count = 0;
        int cpp_count = 0;

        for (const auto& src : project.sources) {
            // Check file-specific CompileAs settings
            for (const auto& [config, compile_as] : src.settings.compile_as) {
                if (compile_as == "CompileAsC") {
                    c_count++;
                } else if (compile_as == "CompileAsCpp") {
                    cpp_count++;
                }
            }

            // Check file extension
            fs::path p(src.path);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext == ".c") {
                c_count++;
            } else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                cpp_count++;
            }
        }

        // Set language if there's a clear majority
        if (c_count > 0 && cpp_count == 0) {
            project.language = "C";
        } else if (cpp_count > 0) {
            project.language = "C++";
        }
        // Leave empty if can't determine (will auto-detect later)
    }

    return project;
}

// Solution parser implementation
static std::string strip_guid_braces(std::string guid) {
    if (guid.size() >= 2 && guid.front() == '{' && guid.back() == '}') {
        guid = guid.substr(1, guid.size() - 2);
    }
    return guid;
}

static std::string solution_id_key(const std::string& id) {
    std::string key = strip_guid_braces(id);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

static std::string solution_path_key(std::string path) {
    path = fs::path(path).lexically_normal().string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

static std::string slnx_folder_path(std::string name, const std::string& parent) {
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    while (!name.empty() && name.back() == '/') name.pop_back();

    if (name.empty()) return parent;
    if (name.find('/') != std::string::npos || parent.empty()) return name;
    return parent + "/" + name;
}

static void add_unique_string(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static bool is_vcxproj_path(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".vcxproj";
}

static std::string project_name_from_vcxproj_path(const std::string& path) {
    return fs::path(path).stem().string();
}

static fs::path resolve_slnx_project_path(const fs::path& sln_dir, const std::string& project_path) {
    fs::path raw_path(project_path);
    fs::path direct = raw_path.is_absolute()
        ? raw_path
        : (sln_dir / raw_path).lexically_normal();

    if (fs::exists(direct)) {
        return direct.lexically_normal();
    }

    std::vector<fs::path> parts;
    for (const auto& component : raw_path) {
        if (component == raw_path.root_name() || component == raw_path.root_directory()) {
            continue;
        }
        parts.push_back(component);
    }

    for (size_t skip = 0; skip < parts.size(); ++skip) {
        fs::path tail;
        for (size_t i = skip; i < parts.size(); ++i) {
            tail /= parts[i];
        }
        if (tail.empty()) {
            continue;
        }

        fs::path rebased = (sln_dir / tail).lexically_normal();
        if (fs::exists(rebased)) {
            return rebased;
        }
    }

    std::error_code ec;
    if (fs::exists(sln_dir, ec)) {
        std::string target_name = raw_path.filename().string();
        std::string target_lower = target_name;
        std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (fs::recursive_directory_iterator it(sln_dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }

            std::string candidate_name = it->path().filename().string();
            std::transform(candidate_name.begin(), candidate_name.end(), candidate_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (candidate_name == target_lower) {
                return it->path().lexically_normal();
            }
        }
    }

    return direct.lexically_normal();
}

static std::string path_relative_to_solution(const fs::path& path, const fs::path& sln_dir) {
    std::error_code ec;
    fs::path rel = fs::relative(path, sln_dir, ec);
    if (!ec && !rel.empty()) {
        return rel.string();
    }
    return path.string();
}

static std::optional<std::string> resolve_slnx_reference(
    const std::string& ref,
    const std::map<std::string, std::string>& key_to_name)
{
    auto id_it = key_to_name.find(solution_id_key(ref));
    if (id_it != key_to_name.end()) {
        return id_it->second;
    }

    auto path_it = key_to_name.find(solution_path_key(ref));
    if (path_it != key_to_name.end()) {
        return path_it->second;
    }

    return std::nullopt;
}

Solution SlnReader::read_sln(const std::string& filepath) {
    Solution solution;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open solution file: " + filepath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Parse VisualStudioVersion from header to determine source VS version
    {
        std::regex vs_ver_re(R"(VisualStudioVersion\s*=\s*(\d+)\.)");
        std::smatch vs_match;
        if (std::regex_search(content, vs_match, vs_ver_re)) {
            int major = std::stoi(vs_match[1].str());
            switch (major) {
                case 11: solution.target_toolset = "v110"; break;
                case 12: solution.target_toolset = "v120"; break;
                case 14: solution.target_toolset = "v140"; break;
                case 15: solution.target_toolset = "v141"; break;
                case 16: solution.target_toolset = "v142"; break;
                case 17: solution.target_toolset = "v143"; break;
                case 18: solution.target_toolset = "v145"; break;
                default: break;
            }
        }
    }

    // Parse solution configurations
    std::regex config_re(R"((\w+)\|(\w+)\s*=\s*(\w+)\|(\w+))");
    std::smatch match;
    std::set<std::string> configs, platforms;

    auto it = content.cbegin();
    while (std::regex_search(it, content.cend(), match, config_re)) {
        configs.insert(match[1].str());
        platforms.insert(match[2].str());
        it = match.suffix().first;
    }

    solution.configurations = std::vector<std::string>(configs.begin(), configs.end());
    solution.platforms = std::vector<std::string>(platforms.begin(), platforms.end());

    if (solution.configurations.empty()) {
        solution.configurations = {"Debug", "Release"};
    }
    if (solution.platforms.empty()) {
        solution.platforms = {"Win32", "x64"};
    }

    // Parse projects
    auto projects = parse_projects(content);

    std::cout << "Found " << projects.size() << " project(s) in solution\n";

    // Parse project dependencies (UUID -> list of dependent UUIDs)
    auto dependencies = parse_project_dependencies(content);

    fs::path sln_dir = fs::path(filepath).parent_path();

    for (const auto& proj_info : projects) {
        fs::path proj_path = sln_dir / proj_info.path;

        if (fs::exists(proj_path)) {
            try {
                VcxprojReader reader;
                Project proj = reader.read_vcxproj(proj_path.string());
                proj.name = proj_info.name;
                proj.uuid = proj_info.uuid;
                proj.vcxproj_path = proj_info.path;  // Store relative path from .sln
                solution.projects.push_back(proj);
            } catch (const std::exception& e) {
                std::cerr << "Warning: Failed to read project " << proj_info.name
                          << ": " << e.what() << "\n";
            }
        } else {
            std::cerr << "Warning: Project file not found: " << proj_path.string() << "\n";
        }
    }

    // Build UUID -> project name mapping
    std::map<std::string, std::string> uuid_to_name;
    for (const auto& proj : solution.projects) {
        uuid_to_name[proj.uuid] = proj.name;
    }

    // Convert dependencies from UUIDs to project names
    std::cout << "\nResolving dependencies:\n";
    for (auto& proj : solution.projects) {
        if (dependencies.count(proj.uuid)) {
            std::cout << "  " << proj.name << " (UUID: " << proj.uuid << ") has "
                      << dependencies[proj.uuid].size() << " dependencies\n";
            for (const auto& dep_uuid : dependencies[proj.uuid]) {
                if (uuid_to_name.count(dep_uuid)) {
                    std::string dep_name = uuid_to_name[dep_uuid];
                    proj.project_references.push_back(ProjectDependency(dep_name));
                    std::cout << "    -> " << dep_name << "\n";
                } else {
                    std::cout << "    -> (unknown UUID: " << dep_uuid << ")\n";
                }
            }
        }
    }

    infer_solution_local_include_dirs(solution, sln_dir);
    infer_project_references_from_link_libraries(solution);

    // Extract solution name from filename
    solution.name = fs::path(filepath).stem().string();
    solution.uuid = generate_uuid();

    return solution;
}

Solution SlnReader::read_slnx(const std::string& filepath) {
    Solution solution;

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());
    if (!result) {
        throw std::runtime_error("Failed to parse slnx: " + std::string(result.description()));
    }

    auto root = doc.child("Solution");
    if (!root) {
        throw std::runtime_error("Invalid slnx file: no Solution root element");
    }

    std::set<std::string> configs, platforms;
    if (auto config_root = root.child("Configurations")) {
        for (auto node : config_root.children()) {
            std::string node_name = node.name();
            std::string name = node.attribute("Name").as_string();
            if (name.empty()) continue;

            if (node_name == "BuildType" || node_name == "Configuration") {
                configs.insert(name);
            } else if (node_name == "Platform") {
                platforms.insert(normalize_platform(name));
            }
        }
    }

    solution.configurations = std::vector<std::string>(configs.begin(), configs.end());
    solution.platforms = std::vector<std::string>(platforms.begin(), platforms.end());

    std::vector<SlnProject> projects;
    std::set<std::string> folder_paths;

    std::function<void(pugi::xml_node, const std::string&)> collect_projects =
        [&](pugi::xml_node node, const std::string& folder_path) {
            for (auto child : node.children()) {
                std::string node_name = child.name();

                if (node_name == "Folder") {
                    std::string child_folder = slnx_folder_path(child.attribute("Name").as_string(), folder_path);
                    if (!child_folder.empty()) {
                        folder_paths.insert(child_folder);
                    }
                    collect_projects(child, child_folder);
                    continue;
                }

                if (node_name == "Project") {
                    SlnProject proj;
                    proj.path = child.attribute("Path").as_string();
                    if (proj.path.empty() || !is_vcxproj_path(proj.path)) {
                        continue;
                    }

                    proj.name = child.attribute("Name").as_string();
                    if (proj.name.empty()) {
                        proj.name = child.attribute("DisplayName").as_string();
                    }
                    proj.uuid = strip_guid_braces(child.attribute("Id").as_string());
                    proj.solution_folder = folder_path;

                    for (auto dep : child.children("BuildDependency")) {
                        std::string dep_key = dep.attribute("Project").as_string();
                        if (dep_key.empty()) dep_key = dep.attribute("Id").as_string();
                        if (dep_key.empty()) dep_key = dep.attribute("Path").as_string();
                        add_unique_string(proj.dependency_keys, dep_key);
                    }

                    projects.push_back(proj);
                    continue;
                }

                collect_projects(child, folder_path);
            }
        };

    collect_projects(root, "");

    std::cout << "Found " << projects.size() << " project(s) in solution\n";

    fs::path sln_dir = fs::path(filepath).parent_path();
    std::vector<std::pair<size_t, SlnProject>> loaded_projects;

    for (const auto& proj_info : projects) {
        fs::path proj_path = resolve_slnx_project_path(sln_dir, proj_info.path);

        if (fs::exists(proj_path)) {
            try {
                VcxprojReader reader;
                Project proj = reader.read_vcxproj(proj_path.string());
                SlnProject loaded_info = proj_info;
                loaded_info.resolved_path = proj_path.string();

                if (!proj_info.name.empty()) {
                    proj.name = proj_info.name;
                } else if (!proj.project_name.empty()) {
                    proj.name = proj.project_name;
                } else {
                    proj.name = project_name_from_vcxproj_path(proj_info.path);
                }

                if (!proj_info.uuid.empty()) {
                    proj.uuid = proj_info.uuid;
                }

                proj.vcxproj_path = path_relative_to_solution(proj_path, sln_dir);
                proj.solution_folder = proj_info.solution_folder;

                size_t project_index = solution.projects.size();
                solution.projects.push_back(proj);
                loaded_projects.push_back({project_index, loaded_info});
            } catch (const std::exception& e) {
                std::cerr << "Warning: Failed to read project " << proj_info.path
                          << ": " << e.what() << "\n";
            }
        } else {
            std::cerr << "Warning: Project file not found: " << proj_path.string() << "\n";
        }
    }

    if (solution.configurations.empty() || solution.platforms.empty()) {
        std::set<std::string> inferred_configs(solution.configurations.begin(), solution.configurations.end());
        std::set<std::string> inferred_platforms(solution.platforms.begin(), solution.platforms.end());

        for (const auto& project : solution.projects) {
            for (const auto& [config_key, cfg] : project.configurations) {
                (void)cfg;
                auto [config_name, platform_name] = parse_config_key(config_key);
                if (!config_name.empty()) {
                    inferred_configs.insert(config_name);
                }
                if (!platform_name.empty()) {
                    inferred_platforms.insert(normalize_platform(platform_name));
                }
            }
        }

        if (solution.configurations.empty()) {
            solution.configurations = std::vector<std::string>(inferred_configs.begin(), inferred_configs.end());
        }
        if (solution.platforms.empty()) {
            solution.platforms = std::vector<std::string>(inferred_platforms.begin(), inferred_platforms.end());
        }
    }

    if (solution.configurations.empty()) {
        solution.configurations = {"Debug", "Release"};
    }
    if (solution.platforms.empty()) {
        solution.platforms = {"Win32", "x64"};
    }

    std::map<std::string, std::string> key_to_name;
    for (const auto& loaded : loaded_projects) {
        const auto& proj_info = loaded.second;
        const auto& proj = solution.projects[loaded.first];

        key_to_name[solution_path_key(proj_info.path)] = proj.name;
        if (!proj_info.resolved_path.empty()) {
            key_to_name[solution_path_key(proj_info.resolved_path)] = proj.name;
        }
        if (!proj.vcxproj_path.empty()) {
            key_to_name[solution_path_key(proj.vcxproj_path)] = proj.name;
        }
        if (!proj_info.uuid.empty()) {
            key_to_name[solution_id_key(proj_info.uuid)] = proj.name;
        }
        if (!proj.uuid.empty()) {
            key_to_name[solution_id_key(proj.uuid)] = proj.name;
        }
    }

    std::cout << "\nResolving dependencies:\n";
    for (const auto& loaded : loaded_projects) {
        auto& proj = solution.projects[loaded.first];
        const auto& proj_info = loaded.second;

        if (proj_info.dependency_keys.empty()) {
            continue;
        }

        std::cout << "  " << proj.name << " has "
                  << proj_info.dependency_keys.size() << " dependencies\n";

        for (const auto& dep_key : proj_info.dependency_keys) {
            auto dep_name = resolve_slnx_reference(dep_key, key_to_name);
            if (dep_name && *dep_name != proj.name) {
                auto already_added = std::find_if(
                    proj.project_references.begin(),
                    proj.project_references.end(),
                    [&](const ProjectDependency& dep) { return dep.name == *dep_name; });

                if (already_added == proj.project_references.end()) {
                    proj.project_references.push_back(ProjectDependency(*dep_name));
                    std::cout << "    -> " << *dep_name << "\n";
                }
            } else {
                std::cout << "    -> (unknown reference: " << dep_key << ")\n";
            }
        }
    }

    infer_solution_local_include_dirs(solution, sln_dir);
    infer_project_references_from_link_libraries(solution);

    for (const auto& path : folder_paths) {
        SolutionFolder folder;
        size_t last_slash = path.rfind('/');
        folder.name = (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);
        folder.path = path;
        folder.parent = (last_slash == std::string::npos) ? std::string() : path.substr(0, last_slash);
        folder.uuid = generate_uuid();
        solution.folders.push_back(folder);
    }

    solution.name = fs::path(filepath).stem().string();
    solution.uuid = generate_uuid();

    return solution;
}

std::vector<SlnReader::SlnProject> SlnReader::parse_projects(const std::string& content) {
    std::vector<SlnProject> projects;
    int total_matches = 0;
    int vcxproj_count = 0;
    int skipped_count = 0;

    // Parse project lines: Project("{...}") = "ProjectName", "path\to\project.vcxproj", "{GUID}"
    std::regex proj_re(R"xxx(Project\s*\("[^"]+"\)\s*=\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"\{([^}]+)\}")xxx");
    std::smatch match;

    auto it = content.cbegin();
    while (std::regex_search(it, content.cend(), match, proj_re)) {
        total_matches++;
        SlnProject proj;
        proj.name = match[1].str();
        proj.path = match[2].str();
        proj.uuid = match[3].str();

        // Only include vcxproj files (filter out solution folders and other project types)
        if (proj.path.find(".vcxproj") != std::string::npos) {
            projects.push_back(proj);
            vcxproj_count++;
        } else {
            std::cout << "  Skipping: " << proj.name << " (" << proj.path << ")\n";
            skipped_count++;
        }

        it = match.suffix().first;
    }

    std::cout << "Total entries parsed: " << total_matches << " (vcxproj: " << vcxproj_count << ", skipped: " << skipped_count << ")\n";

    return projects;
}

std::map<std::string, std::vector<std::string>> SlnReader::parse_project_dependencies(const std::string& content) {
    std::map<std::string, std::vector<std::string>> dependencies;

    // Use string searching instead of complex regex to avoid stack overflow
    // Parse by finding Project lines, then looking for ProjectSection(ProjectDependencies) blocks

    size_t pos = 0;
    std::regex proj_line_re(R"(Project\s*\("[^"]+"\)\s*=\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"\{([A-Fa-f0-9-]+)\}")");

    std::cout << "Parsing ProjectDependencies from .sln content...\n";
    while (pos < content.length()) {
        // Find next "Project(" line
        size_t proj_start = content.find("Project(", pos);
        if (proj_start == std::string::npos) break;

        // Find the matching EndProject
        size_t end_project = content.find("EndProject", proj_start);
        if (end_project == std::string::npos) break;

        // Extract the project block - include "EndProject" line
        size_t end_project_line = content.find('\n', end_project);
        if (end_project_line == std::string::npos) end_project_line = content.length();
        std::string block = content.substr(proj_start, end_project_line - proj_start);

        // Extract UUID from Project line using regex
        std::smatch match;
        if (std::regex_search(block, match, proj_line_re)) {
            std::string project_uuid = match[1].str();

            // Check if this block has ProjectSection(ProjectDependencies)
            size_t dep_section = block.find("ProjectSection(ProjectDependencies)");
            if (dep_section != std::string::npos) {
                // Find EndProjectSection
                size_t end_section = block.find("EndProjectSection", dep_section);
                if (end_section != std::string::npos) {
                    std::string dep_section_content = block.substr(dep_section, end_section - dep_section);

                    // Extract dependency UUIDs
                    std::regex dep_re(R"(\{([A-Fa-f0-9-]+)\}\s*=\s*\{[A-Fa-f0-9-]+\})");
                    std::smatch dep_match;

                    auto it = dep_section_content.cbegin();
                    while (std::regex_search(it, dep_section_content.cend(), dep_match, dep_re)) {
                        std::string dep_uuid = dep_match[1].str();
                        dependencies[project_uuid].push_back(dep_uuid);
                        it = dep_match.suffix().first;
                    }
                }
            }
        }

        pos = end_project + 1;
    }

    return dependencies;
}

// Buildscript writer implementation
std::string BuildscriptWriter::join_vector(const std::vector<std::string>& vec, const std::string& sep) {
    if (vec.empty()) return "";
    std::string result = vec[0];
    for (size_t i = 1; i < vec.size(); i++) {
        result += sep + vec[i];
    }
    return result;
}

std::string BuildscriptWriter::format_value(const std::string& value) {
    // Check if value contains newlines - if so, use multiline format
    if (value.find('\n') != std::string::npos) {
        std::string result = "\"\"\"\n";
        for (char c : value) {
            if (c == '\r') {
                // Skip carriage returns
            } else {
                result += c;
            }
        }
        if (!result.empty() && result.back() != '\n') {
            result += '\n';
        }
        result += "\"\"\"";
        return result;
    }
    // Single-line value, return as-is
    return value;
}

// Helper to check if all configs have the same value for a string setting
template<typename MapType>
bool all_configs_have_same_value(const MapType& map, const std::vector<std::string>& config_keys, std::string& out_value) {
    if (map.empty()) return false;

    std::string first_value;
    bool has_first = false;

    for (const auto& config_key : config_keys) {
        if (map.count(config_key)) {
            const auto& value = map.at(config_key);
            if (!has_first) {
                first_value = value;
                has_first = true;
            } else if (value != first_value) {
                return false; // Values differ
            }
        } else {
            return false; // Config missing
        }
    }

    if (has_first) {
        out_value = first_value;
        return true;
    }
    return false;
}

// Helper for vector<string> maps
bool all_configs_have_same_vector(const std::map<std::string, std::vector<std::string>>& map,
                                   const std::vector<std::string>& config_keys,
                                   std::vector<std::string>& out_value) {
    if (map.empty()) return false;

    std::vector<std::string> first_value;
    bool has_first = false;

    for (const auto& config_key : config_keys) {
        if (map.count(config_key)) {
            const auto& value = map.at(config_key);
            if (!has_first) {
                first_value = value;
                has_first = true;
            } else if (value != first_value) {
                return false; // Values differ
            }
        } else {
            return false; // Config missing
        }
    }

    if (has_first) {
        out_value = first_value;
        return true;
    }
    return false;
}

// Helper for bool maps
bool all_configs_have_same_bool(const std::map<std::string, bool>& map,
                                const std::vector<std::string>& config_keys,
                                bool& out_value) {
    if (map.empty()) return false;

    bool first_value = false;
    bool has_first = false;

    for (const auto& config_key : config_keys) {
        if (map.count(config_key)) {
            const auto& value = map.at(config_key);
            if (!has_first) {
                first_value = value;
                has_first = true;
            } else if (value != first_value) {
                return false; // Values differ
            }
        } else {
            return false; // Config missing
        }
    }

    if (has_first) {
        out_value = first_value;
        return true;
    }
    return false;
}

// Helper function to create a signature for file settings for grouping purposes
static std::string create_file_settings_signature(const SourceFile* src,
                                                   const std::vector<std::string>&,
                                                   const std::map<std::string, std::string>& default_pch_mode,
                                                   const std::map<std::string, std::string>& default_pch_header,
                                                   bool has_project_common_pch_mode,
                                                   bool has_project_common_pch_header,
                                                   bool has_project_common_pch_output,
                                                   const std::string& project_common_pch_mode,
                                                   const std::string& project_common_pch_header,
                                                   const std::string& project_common_pch_output) {
    (void)has_project_common_pch_mode;
    (void)has_project_common_pch_header;
    (void)has_project_common_pch_output;
    (void)project_common_pch_mode;
    (void)project_common_pch_header;
    (void)project_common_pch_output;

    std::ostringstream sig;

    // Add all settings to signature
    for (const auto& kv : src->settings.additional_includes) {
        sig << "inc[" << kv.first << "]:";
        for (const auto& inc : kv.second) sig << inc << ";";
        sig << "|";
    }
    for (const auto& kv : src->settings.preprocessor_defines) {
        sig << "def[" << kv.first << "]:";
        for (const auto& def : kv.second) sig << def << ";";
        sig << "|";
    }
    for (const auto& kv : src->settings.additional_options) {
        sig << "opt[" << kv.first << "]:";
        for (const auto& opt : kv.second) sig << opt << ";";
        sig << "|";
    }
    for (const auto& kv : src->settings.excluded) {
        sig << "exc[" << kv.first << "]:" << (kv.second ? "1" : "0") << "|";
    }
    for (const auto& kv : src->settings.compile_as) {
        sig << "cas[" << kv.first << "]:" << kv.second << "|";
    }
    for (const auto& kv : src->settings.object_file) {
        sig << "obj[" << kv.first << "]:" << kv.second << "|";
    }
    for (const auto& kv : src->custom_command) {
        sig << "cmd[" << kv.first << "]:" << kv.second << "|";
    }
    for (const auto& kv : src->custom_outputs) {
        sig << "out[" << kv.first << "]:" << kv.second << "|";
    }
    for (const auto& kv : src->custom_message) {
        sig << "msg[" << kv.first << "]:" << kv.second << "|";
    }

    // Add PCH settings that differ from defaults
    for (const auto& pch_kv : src->settings.pch) {
        const std::string& config_key = pch_kv.first;
        const PrecompiledHeader& pch = pch_kv.second;
        bool mode_differs = (!pch.mode.empty() &&
                            (!default_pch_mode.count(config_key) ||
                             pch.mode != default_pch_mode.at(config_key)));
        bool header_differs = (!pch.header.empty() &&
                              (!default_pch_header.count(config_key) ||
                               pch.header != default_pch_header.at(config_key)));

        if (mode_differs || pch.mode == "NotUsing")
            sig << "pch_mode[" << config_key << "]:" << pch.mode << "|";
        if (header_differs)
            sig << "pch_header[" << config_key << "]:" << pch.header << "|";
        if (!pch.output.empty())
            sig << "pch_output[" << config_key << "]:" << pch.output << "|";
    }

    return sig.str();
}

static std::string config_type_to_buildscript_type(const std::string& config_type, bool kernel_mode) {
    if (config_type == "Application") return "exe";
    if (config_type == "StaticLibrary" && kernel_mode) return "sys_lib";
    if (config_type == "StaticLibrary") return "lib";
    if (config_type == "DynamicLibrary") return "dll";
    if (config_type == "Driver") return "sys";
    if (config_type == "Utility") return "interface";
    return config_type;
}

static std::string cpp_standard_to_buildscript_value(std::string value) {
    if (value.find("stdcpp") == 0) {
        value = value.substr(6);
    }
    return value;
}

void BuildscriptWriter::write_project_content(std::ostream& out, const Project& project,
                                             const std::string& filepath,
                                             const std::vector<std::string>& configurations,
                                             const std::vector<std::string>& platforms) {
    // Suppress unused parameter warnings (these may be used in future enhancements)
    (void)configurations;
    (void)platforms;

    out << "[project:" << project.name << "]\n";

    // Write global project properties
    if (!project.project_name.empty())
        out << "project_name = " << project.project_name << "\n";
    if (!project.uuid.empty())
        out << "uuid = " << project.uuid << "\n";
    if (!project.root_namespace.empty())
        out << "root_namespace = " << project.root_namespace << "\n";
    if (project.ignore_warn_compile_duplicated_filename)
        out << "ignore_warn_duplicated_filename = true\n";

    // Determine project type from first configuration
    if (!project.configurations.empty()) {
        auto& first_cfg = project.configurations.begin()->second;
        out << "type = " << config_type_to_buildscript_type(first_cfg.config_type, project.is_kernel_mode) << "\n";
    }

    // Source files
    std::vector<std::string> cpp_files, h_files, rc_files, masm_files, nasm_files, mc_files, idl_files;
    for (const auto& src : project.sources) {
        if (src.type == FileType::ClCompile) {
            cpp_files.push_back(src.path);
        } else if (src.type == FileType::ClInclude) {
            h_files.push_back(src.path);
        } else if (src.type == FileType::ResourceCompile) {
            rc_files.push_back(src.path);
        } else if (src.type == FileType::MASM) {
            masm_files.push_back(src.path);
        } else if (src.type == FileType::NASM) {
            nasm_files.push_back(src.path);
        } else if (src.type == FileType::MessageCompile) {
            mc_files.push_back(src.path);
        } else if (src.type == FileType::Midl) {
            idl_files.push_back(src.path);
        }
    }

    if (!cpp_files.empty()) {
        out << "sources = " << join_vector(cpp_files, ", ") << "\n";
    }
    if (!h_files.empty()) {
        out << "headers = " << join_vector(h_files, ", ") << "\n";
    }
    if (!rc_files.empty()) {
        out << "resources = " << join_vector(rc_files, ", ") << "\n";
    }
    if (!masm_files.empty()) {
        out << "masm = " << join_vector(masm_files, ", ") << "\n";
    }
    if (!nasm_files.empty()) {
        out << "nasm = " << join_vector(nasm_files, ", ") << "\n";
    }
    if (!mc_files.empty()) {
        out << "mc = " << join_vector(mc_files, ", ") << "\n";
    }
    if (!idl_files.empty()) {
        out << "idl = " << join_vector(idl_files, ", ") << "\n";
    }

    // Project references - write as target_link_libraries for clarity
    if (!project.project_references.empty()) {
        std::cout << "    Writing dependencies for " << project.project_name << ": "
                  << project.project_references.size() << " deps\n";
        // Extract dependency names for join_vector
        std::vector<std::string> dep_names;
        for (const auto& dep : project.project_references) {
            dep_names.push_back(dep.name);
        }
        out << "target_link_libraries(" << join_vector(dep_names, ", ") << ")\n";
    }

    // Analyze libraries to separate those with exclusions from those without
    std::vector<const LibraryFile*> excluded_libs;
    for (const auto& lib : project.libraries) {
        if (!lib.excluded.empty()) {
            excluded_libs.push_back(&lib);
        }
    }

    // Write common settings (from first config as default)
    if (!project.configurations.empty()) {
        auto& first_cfg = project.configurations.begin()->second;
        auto& cl = first_cfg.cl_compile;
        auto& link = first_cfg.link;
        auto& libsettings = first_cfg.lib;

        if (!first_cfg.platform_toolset.empty())
            out << "toolset = " << first_cfg.platform_toolset << "\n";
        if (!first_cfg.windows_target_platform_version.empty())
            out << "windows_sdk = " << first_cfg.windows_target_platform_version << "\n";
        if (!first_cfg.character_set.empty())
            out << "charset = " << first_cfg.character_set << "\n";
        if (!first_cfg.use_of_mfc.empty() && first_cfg.use_of_mfc != "false")
            out << "use_of_mfc = " << first_cfg.use_of_mfc << "\n";
        if (!first_cfg.use_of_atl.empty() && first_cfg.use_of_atl != "false")
            out << "use_of_atl = " << first_cfg.use_of_atl << "\n";

        // Compiler settings
        if (!cl.additional_include_directories.empty())
            out << "includes = " << join_vector(cl.additional_include_directories, ", ") << "\n";
        if (!cl.forced_include_files.empty())
            out << "forced_includes = " << join_vector(cl.forced_include_files, ", ") << "\n";

        // Find common preprocessor definitions across all configurations
        // Configuration-specific defines like _DEBUG, DEBUG, NDEBUG will be written per-config
        if (!project.configurations.empty()) {
            std::set<std::string> common_defines;
            bool first = true;

            for (const auto& [config_key, config] : project.configurations) {
                std::set<std::string> config_defines(
                    config.cl_compile.preprocessor_definitions.begin(),
                    config.cl_compile.preprocessor_definitions.end()
                );

                if (first) {
                    common_defines = config_defines;
                    first = false;
                } else {
                    // Keep only defines that exist in all configurations
                    std::set<std::string> intersection;
                    std::set_intersection(
                        common_defines.begin(), common_defines.end(),
                        config_defines.begin(), config_defines.end(),
                        std::inserter(intersection, intersection.begin())
                    );
                    common_defines = intersection;
                }
            }

            if (!common_defines.empty()) {
                std::vector<std::string> common_defines_vec(common_defines.begin(), common_defines.end());
                out << "defines = " << join_vector(common_defines_vec, ", ") << "\n";
            }
        }
        if (!cl.language_standard.empty()) {
            out << "std = " << cpp_standard_to_buildscript_value(cl.language_standard) << "\n";
        }
        if (!cl.warning_level.empty()) {
            std::string wl = cl.warning_level;
            if (wl == "TurnOffAllWarnings") wl = "Level0";
            out << "warning_level = " << wl << "\n";
        }
        if (!cl.exception_handling.empty())
            out << "exceptions = " << cl.exception_handling << "\n";
        if (!cl.runtime_type_info)
            out << "rtti = false\n";
        if (cl.multi_processor_compilation)
            out << "multiprocessor = true\n";
        if (!cl.enhanced_instruction_set.empty())
            out << "simd = " << cl.enhanced_instruction_set << "\n";
        if (!cl.floating_point_model.empty())
            out << "floating_point = " << cl.floating_point_model << "\n";
        if (cl.string_pooling)
            out << "string_pooling = true\n";
        if (!cl.favor_size_or_speed.empty())
            out << "favor = " << cl.favor_size_or_speed << "\n";
        if (!cl.inline_function_expansion.empty())
            out << "inline_expansion = " << cl.inline_function_expansion << "\n";
        if (!cl.buffer_security_check)
            out << "buffer_security = false\n";
        if (!cl.compile_as.empty())
            out << "compile_as = " << cl.compile_as << "\n";
        if (!cl.disable_specific_warnings.empty())
            out << "disable_warnings = " << join_vector(cl.disable_specific_warnings, ", ") << "\n";
        if (!cl.additional_options.empty())
            out << "cflags = " << cl.additional_options << "\n";
        if (!cl.error_reporting.empty())
            out << "error_reporting = " << cl.error_reporting << "\n";
        if (!cl.treat_wchar_t_as_built_in_type)
            out << "treat_wchar_t_as_builtin = false\n";
        if (!cl.assembler_output.empty())
            out << "assembler_output = " << cl.assembler_output << "\n";
        if (cl.expand_attributed_source)
            out << "expand_attributed_source = true\n";
        if (cl.openmp_support)
            out << "openmp = true\n";
        if (cl.treat_warning_as_error)
            out << "treat_warning_as_error = true\n";
        if (!cl.assembler_listing_location.empty())
            out << "assembler_listing = " << cl.assembler_listing_location << "\n";
        if (!cl.object_file_name.empty())
            out << "object_file_name = " << cl.object_file_name << "\n";
        if (!cl.program_database_file_name.empty())
            out << "program_database_file = " << cl.program_database_file_name << "\n";
        if (!cl.browse_information_file.empty())
            out << "browse_information_file = " << cl.browse_information_file << "\n";
        if (!cl.basic_runtime_checks.empty())
            out << "basic_runtime_checks = " << cl.basic_runtime_checks << "\n";

        // Linker settings
        if (!link.sub_system.empty())
            out << "subsystem = " << link.sub_system << "\n";
        if (!link.image_has_safe_exception_handlers)
            out << "safe_seh = false\n";
        if (!link.additional_options.empty())
            out << "ldflags = " << link.additional_options << "\n";
        if (!link.error_reporting.empty())
            out << "link_error_reporting = " << link.error_reporting << "\n";
        if (!link.target_machine.empty())
            out << "target_machine = " << link.target_machine << "\n";
        if (link.suppress_startup_banner)
            out << "suppress_startup_banner = true\n";
        if (!link.show_progress.empty())
            out << "show_progress = " << link.show_progress << "\n";
        if (!link.entry_point_symbol.empty())
            out << "entry_point = " << link.entry_point_symbol << "\n";
        if (!link.version.empty())
            out << "link_version = " << link.version << "\n";
        if (!link.output_file.empty())
            out << "link_output_file = " << link.output_file << "\n";
        if (!link.program_database_file.empty())
            out << "link_program_database_file = " << link.program_database_file << "\n";
        if (link.generate_map_file)
            out << "generate_map_file = true\n";
        if (!link.map_file_name.empty())
            out << "map_file_name = " << link.map_file_name << "\n";
        if (link.fixed_base_address)
            out << "fixed_base_address = true\n";
        if (link.randomized_base_address.has_value() && !link.randomized_base_address.value())
            out << "randomized_base_address = false\n";
        if (link.large_address_aware)
            out << "large_address_aware = true\n";
        if (link.ignore_all_default_libraries)
            out << "ignore_all_default_libraries = true\n";
        if (!link.module_definition_file.empty())
            out << "module_def = " << link.module_definition_file << "\n";

        // Librarian settings (for static libraries)
        if (!libsettings.output_file.empty())
            out << "lib_output_file = " << libsettings.output_file << "\n";
        if (libsettings.suppress_startup_banner)
            out << "lib_suppress_startup_banner = true\n";
        if (libsettings.use_unicode_response_files)
            out << "lib_use_unicode_response_files = true\n";
        if (!libsettings.additional_options.empty())
            out << "libflags = " << libsettings.additional_options << "\n";
        if (!libsettings.additional_dependencies.empty())
            out << "lib_additional_dependencies = " << join_vector(libsettings.additional_dependencies, ", ") << "\n";

        // Combine Library elements and additional dependencies
        // Only include libraries without config-specific exclusions at project level
        std::vector<std::string> all_libs;

        for (const auto& lib : project.libraries) {
            // Check if library has any config-specific exclusions
            if (lib.excluded.empty()) {
                all_libs.push_back(lib.path);
            }
        }
        // Don't add config-specific additional_dependencies here
        // They will be written per-config
        if (!all_libs.empty())
            out << "libs = " << join_vector(all_libs, ", ") << "\n";

        if (!link.additional_library_directories.empty())
            out << "libdirs = " << join_vector(link.additional_library_directories, ", ") << "\n";

        // Write Library elements with ExcludedFromBuild as excluded_library entries
        for (const auto* lib : excluded_libs) {
            // For each configuration, write the library if NOT excluded
            for (const auto& [config_key, cfg] : project.configurations) {
                bool is_excluded = false;
                for (const auto& [excl_config_key, excluded] : lib->excluded) {
                    if ((excl_config_key == ALL_CONFIGS || excl_config_key == config_key) && excluded) {
                        is_excluded = true;
                        break;
                    }
                }
                if (!is_excluded) {
                    out << "excluded_library[" << config_key << "] = " << lib->path << "\n";
                }
            }
        }
    }

    // Write config-specific settings
    out << "\n# Configuration-specific settings\n";

    // First, calculate common defines across all configurations
    std::set<std::string> common_defines;
    if (!project.configurations.empty()) {
        bool first = true;
        for (const auto& [config_key, config] : project.configurations) {
            std::set<std::string> config_defines(
                config.cl_compile.preprocessor_definitions.begin(),
                config.cl_compile.preprocessor_definitions.end()
            );

            if (first) {
                common_defines = config_defines;
                first = false;
            } else {
                std::set<std::string> intersection;
                std::set_intersection(
                    common_defines.begin(), common_defines.end(),
                    config_defines.begin(), config_defines.end(),
                    std::inserter(intersection, intersection.begin())
                );
                common_defines = intersection;
            }
        }
    }

    for (const auto& [config_key, cfg] : project.configurations) {
        out << "\n[config:" << config_key << "]\n";
        const auto& first_cfg = project.configurations.begin()->second;

        if (!cfg.config_type.empty() && cfg.config_type != first_cfg.config_type) {
            out << "type = " << config_type_to_buildscript_type(cfg.config_type, project.is_kernel_mode) << "\n";
        }
        if (!cfg.platform_toolset.empty() && cfg.platform_toolset != first_cfg.platform_toolset) {
            out << "toolset = " << cfg.platform_toolset << "\n";
        }
        if (!cfg.windows_target_platform_version.empty() &&
            cfg.windows_target_platform_version != first_cfg.windows_target_platform_version) {
            out << "windows_sdk = " << cfg.windows_target_platform_version << "\n";
        }

        // Write config-specific preprocessor definitions (those not common to all configs)
        std::vector<std::string> config_specific_defines;
        for (const auto& def : cfg.cl_compile.preprocessor_definitions) {
            if (common_defines.find(def) == common_defines.end()) {
                config_specific_defines.push_back(def);
            }
        }
        if (!config_specific_defines.empty()) {
            out << "defines = " << join_vector(config_specific_defines, ", ") << "\n";
        }

        // Resolve out_dir and int_dir to actual paths relative to buildscript location
        // This ensures the buildscript is location-independent
        if (!cfg.out_dir.empty()) {
            std::string converted_out_dir = cfg.out_dir;
            // Skip path conversion for values containing MSBuild variables - they must be preserved as-is
            if (cfg.out_dir.find("$(") == std::string::npos && !project.vcxproj_path.empty()) {
                namespace fs = std::filesystem;
                try {
                    fs::path buildscript_dir = fs::path(filepath).parent_path();
                    fs::path vcxproj_path(project.vcxproj_path);
                    fs::path vcxproj_dir = vcxproj_path.is_absolute()
                        ? vcxproj_path.parent_path()
                        : buildscript_dir;

                    // Resolve out_dir to absolute path based on vcxproj location
                    fs::path abs_out_dir = fs::absolute(vcxproj_dir / cfg.out_dir);

                    // Normalize the path (removes .\ and \.\ etc)
                    abs_out_dir = abs_out_dir.lexically_normal();

                    // Make it relative to buildscript location
                    fs::path relative_out = fs::relative(abs_out_dir, buildscript_dir);

                    converted_out_dir = relative_out.string();
                    std::replace(converted_out_dir.begin(), converted_out_dir.end(), '/', '\\');

                    // Add trailing backslash if original had one
                    if (!cfg.out_dir.empty() && (cfg.out_dir.back() == '\\' || cfg.out_dir.back() == '/')) {
                        if (!converted_out_dir.empty() && converted_out_dir.back() != '\\') {
                            converted_out_dir += '\\';
                        }
                    }
                } catch (...) {
                    // If conversion fails, use original path
                }
            }
            out << "outdir = " << converted_out_dir << "\n";
        }
        if (!cfg.int_dir.empty()) {
            std::string converted_int_dir = cfg.int_dir;
            // Skip path conversion for values containing MSBuild variables - they must be preserved as-is
            if (cfg.int_dir.find("$(") == std::string::npos && !project.vcxproj_path.empty()) {
                namespace fs = std::filesystem;
                try {
                    fs::path buildscript_dir = fs::path(filepath).parent_path();
                    fs::path vcxproj_path(project.vcxproj_path);
                    fs::path vcxproj_dir = vcxproj_path.is_absolute()
                        ? vcxproj_path.parent_path()
                        : buildscript_dir;

                    // Resolve int_dir to absolute path based on vcxproj location
                    fs::path abs_int_dir = fs::absolute(vcxproj_dir / cfg.int_dir);

                    // Normalize the path (removes .\ and \.\ etc)
                    abs_int_dir = abs_int_dir.lexically_normal();

                    // Make it relative to buildscript location
                    fs::path relative_int = fs::relative(abs_int_dir, buildscript_dir);

                    converted_int_dir = relative_int.string();
                    std::replace(converted_int_dir.begin(), converted_int_dir.end(), '/', '\\');

                    // Add trailing backslash if original had one
                    if (!cfg.int_dir.empty() && (cfg.int_dir.back() == '\\' || cfg.int_dir.back() == '/')) {
                        if (!converted_int_dir.empty() && converted_int_dir.back() != '\\') {
                            converted_int_dir += '\\';
                        }
                    }
                } catch (...) {
                    // If conversion fails, use original path
                }
            }
            out << "intdir = " << converted_int_dir << "\n";
        }
        if (!cfg.target_name.empty())
            out << "target_name = " << cfg.target_name << "\n";
        if (!cfg.target_ext.empty())
            out << "target_ext = " << cfg.target_ext << "\n";
        if (!cfg.character_set.empty() &&
            cfg.character_set != project.configurations.begin()->second.character_set)
            out << "charset = " << cfg.character_set << "\n";
        if (!cfg.cl_compile.language_standard.empty() &&
            cfg.cl_compile.language_standard != first_cfg.cl_compile.language_standard) {
            out << "std = " << cpp_standard_to_buildscript_value(cfg.cl_compile.language_standard) << "\n";
        }
        if (!cfg.cl_compile.additional_include_directories.empty() &&
            cfg.cl_compile.additional_include_directories != first_cfg.cl_compile.additional_include_directories) {
            out << "includes = " << join_vector(cfg.cl_compile.additional_include_directories, ", ") << "\n";
        }
        if (!cfg.cl_compile.forced_include_files.empty() &&
            cfg.cl_compile.forced_include_files != first_cfg.cl_compile.forced_include_files) {
            out << "forced_includes = " << join_vector(cfg.cl_compile.forced_include_files, ", ") << "\n";
        }
        if (!cfg.executable_path.empty())
            out << "executable_path = " << cfg.executable_path << "\n";
        if (!cfg.generate_manifest)
            out << "generate_manifest = false\n";
        if (cfg.ignore_import_library)
            out << "ignore_import_library = true\n";
        if (!cfg.import_library.empty())
            out << "import_library = " << cfg.import_library << "\n";
        if (!cfg.cl_compile.optimization.empty())
            out << "optimization = " << cfg.cl_compile.optimization << "\n";
        if (!cfg.cl_compile.runtime_library.empty())
            out << "runtime_library = " << cfg.cl_compile.runtime_library << "\n";
        if (!cfg.cl_compile.debug_information_format.empty())
            out << "debug_info = " << cfg.cl_compile.debug_information_format << "\n";
        if (cfg.link.generate_debug_info)
            out << "generate_debug_info = true\n";
        if (cfg.link_incremental)
            out << "link_incremental = true\n";
        if (cfg.whole_program_optimization)
            out << "whole_program_optimization = true\n";
        // Only write per-config cflags if different from global (first config)
        if (!cfg.cl_compile.additional_options.empty() &&
            cfg.cl_compile.additional_options != project.configurations.begin()->second.cl_compile.additional_options)
            out << "cflags = " << cfg.cl_compile.additional_options << "\n";
        if (cfg.cl_compile.multi_processor_compilation != first_cfg.cl_compile.multi_processor_compilation)
            out << "multiprocessor = " << (cfg.cl_compile.multi_processor_compilation ? "true" : "false") << "\n";
        if (!cfg.cl_compile.favor_size_or_speed.empty())
            out << "favor = " << cfg.cl_compile.favor_size_or_speed << "\n";
        if (!cfg.cl_compile.inline_function_expansion.empty())
            out << "inline_expansion = " << cfg.cl_compile.inline_function_expansion << "\n";
        if (cfg.cl_compile.intrinsic_functions.value_or(false))
            out << "intrinsic_functions = true\n";
        if (cfg.cl_compile.function_level_linking.value_or(false))
            out << "function_level_linking = true\n";

        // Write config-specific libraries (only additional dependencies, not excluded libs)
        // Excluded libs are now written as excluded_library entries at project level
        std::vector<std::string> config_libs;

        // Add config-specific additional dependencies only
        for (const auto& dep : cfg.link.additional_dependencies) {
            config_libs.push_back(dep);
        }

        if (!config_libs.empty()) {
            out << "libs = " << join_vector(config_libs, ", ") << "\n";
        }
        if (!cfg.link.additional_library_directories.empty() &&
            cfg.link.additional_library_directories != first_cfg.link.additional_library_directories) {
            out << "libdirs = " << join_vector(cfg.link.additional_library_directories, ", ") << "\n";
        }
        if (!cfg.link.sub_system.empty() && cfg.link.sub_system != first_cfg.link.sub_system) {
            out << "subsystem = " << cfg.link.sub_system << "\n";
        }
        if (!cfg.link.additional_options.empty() &&
            cfg.link.additional_options != first_cfg.link.additional_options) {
            out << "ldflags = " << cfg.link.additional_options << "\n";
        }

        // Linker ignore settings
        // Note: Common runtime libraries (libc, libcmt, libcmtd, etc.) are automatically
        // added based on the runtime_library setting, so you typically don't need to
        // specify them manually unless you have custom requirements
        if (!cfg.link.ignore_specific_default_libraries.empty()) {
            out << "ignore_libs = " << join_vector(cfg.link.ignore_specific_default_libraries, ", ") << "\n";
        }

        // Xdcmake settings
        if (cfg.xdcmake.suppress_startup_banner)
            out << "xdcmake_suppress_startup_banner = true\n";

        // Bscmake settings
        if (cfg.bscmake.suppress_startup_banner)
            out << "bscmake_suppress_startup_banner = true\n";
        if (!cfg.bscmake.output_file.empty())
            out << "bscmake_output_file = " << cfg.bscmake.output_file << "\n";

        // ResourceCompile settings
        if (!cfg.resource_compile.culture.empty())
            out << "rc_culture = " << cfg.resource_compile.culture << "\n";
        if (!cfg.resource_compile.preprocessor_definitions.empty())
            out << "rc_defines = " << join_vector(cfg.resource_compile.preprocessor_definitions, ", ") << "\n";
        if (!cfg.resource_compile.additional_include_directories.empty())
            out << "rc_includes = " << join_vector(cfg.resource_compile.additional_include_directories, ", ") << "\n";

        // Message Compiler settings
        if (!cfg.mc.header_file_path.empty())
            out << "mc_header_dir = " << cfg.mc.header_file_path << "\n";
        if (!cfg.mc.rc_file_path.empty())
            out << "mc_rc_dir = " << cfg.mc.rc_file_path << "\n";
        if (!cfg.mc.additional_options.empty())
            out << "mc_flags = " << cfg.mc.additional_options << "\n";

        // MIDL compiler settings
        if (!cfg.midl.output_directory.empty())
            out << "midl_output_dir = " << cfg.midl.output_directory << "\n";
        if (!cfg.midl.header_file_name.empty())
            out << "midl_header = " << cfg.midl.header_file_name << "\n";
        if (!cfg.midl.type_library_name.empty())
            out << "midl_type_library = " << cfg.midl.type_library_name << "\n";
        if (!cfg.midl.dlldata_file_name.empty())
            out << "midl_dlldata = " << cfg.midl.dlldata_file_name << "\n";
        if (!cfg.midl.interface_identifier_file_name.empty())
            out << "midl_iid = " << cfg.midl.interface_identifier_file_name << "\n";
        if (!cfg.midl.proxy_file_name.empty())
            out << "midl_proxy = " << cfg.midl.proxy_file_name << "\n";
        if (!cfg.midl.preprocessor_definitions.empty())
            out << "midl_defines = " << join_vector(cfg.midl.preprocessor_definitions, ", ") << "\n";
        if (!cfg.midl.additional_options.empty())
            out << "midl_flags = " << cfg.midl.additional_options << "\n";
        if (!cfg.midl.default_char_type.empty())
            out << "midl_default_char_type = " << cfg.midl.default_char_type << "\n";
        if (!cfg.midl.target_environment.empty())
            out << "midl_target_environment = " << cfg.midl.target_environment << "\n";

        // Manifest settings
        if (cfg.manifest.suppress_startup_banner)
            out << "manifest_suppress_startup_banner = true\n";
        if (!cfg.manifest.additional_manifest_files.empty())
            out << "manifest_additional_files = " << cfg.manifest.additional_manifest_files << "\n";

        // Build events
        if (!cfg.pre_build_event.command.empty()) {
            out << "prebuild = " << format_value(cfg.pre_build_event.command) << "\n";
            if (!cfg.pre_build_event.message.empty())
                out << "prebuild_message = " << format_value(cfg.pre_build_event.message) << "\n";
            if (!cfg.pre_build_event.use_in_build)
                out << "prebuild_use_in_build = false\n";
        }
        if (!cfg.pre_link_event.command.empty()) {
            out << "prelink = " << format_value(cfg.pre_link_event.command) << "\n";
            if (!cfg.pre_link_event.message.empty())
                out << "prelink_message = " << format_value(cfg.pre_link_event.message) << "\n";
            if (!cfg.pre_link_event.use_in_build)
                out << "prelink_use_in_build = false\n";
        }
        if (!cfg.post_build_event.command.empty()) {
            out << "postbuild = " << format_value(cfg.post_build_event.command) << "\n";
            if (!cfg.post_build_event.message.empty())
                out << "postbuild_message = " << format_value(cfg.post_build_event.message) << "\n";
            if (!cfg.post_build_event.use_in_build)
                out << "postbuild_use_in_build = false\n";
        }
    }

    // Use project-level PCH settings from ItemDefinitionGroup as defaults
    std::map<std::string, std::string> default_pch_mode;
    std::map<std::string, std::string> default_pch_header;
    std::map<std::string, std::string> default_pch_output;

    // Collect all configuration keys for consolidation checks
    std::vector<std::string> config_keys;
    for (const auto& [config_key, cfg] : project.configurations) {
        config_keys.push_back(config_key);
    }

    // Extract project-level PCH settings from configurations
    for (const auto& [config_key, cfg] : project.configurations) {
        if (!cfg.cl_compile.pch.mode.empty()) {
            default_pch_mode[config_key] = cfg.cl_compile.pch.mode;
        }
        if (!cfg.cl_compile.pch.header.empty()) {
            default_pch_header[config_key] = cfg.cl_compile.pch.header;
        }
        if (!cfg.cl_compile.pch.output.empty()) {
            default_pch_output[config_key] = cfg.cl_compile.pch.output;
        }
    }

    // Write project-level PCH defaults if they exist
    bool wrote_pch_defaults = false;
    for (const auto& [config_key, mode] : default_pch_mode) {
        // Write pch mode if not "NotUsing" (NotUsing is the implicit default)
        if (mode != "NotUsing") {
            if (!wrote_pch_defaults) {
                out << "\n# Precompiled header defaults\n";
                wrote_pch_defaults = true;
            }
            out << "pch[" << config_key << "] = " << mode << "\n";
            if (default_pch_header.count(config_key)) {
                out << "pch_header[" << config_key << "] = " << default_pch_header[config_key] << "\n";
            }
            if (default_pch_output.count(config_key)) {
                out << "pch_output[" << config_key << "] = " << default_pch_output[config_key] << "\n";
            }
        }
    }

    // Write pch_header even when mode is "NotUsing" if header is specified
    // This is needed for files that have Create mode - they inherit this header
    for (const auto& [config_key, header] : default_pch_header) {
        // Only write if we haven't already written it above
        if (!default_pch_mode.count(config_key) || default_pch_mode[config_key] == "NotUsing") {
            if (!wrote_pch_defaults) {
                out << "\n# Precompiled header defaults\n";
                wrote_pch_defaults = true;
            }
            out << "pch_header[" << config_key << "] = " << header << "\n";
        }
    }

    // Check if project-level PCH settings are consistent across all configs
    std::string project_common_pch_mode, project_common_pch_header, project_common_pch_output;
    bool has_project_common_pch_mode = all_configs_have_same_value(default_pch_mode, config_keys, project_common_pch_mode);
    bool has_project_common_pch_header = all_configs_have_same_value(default_pch_header, config_keys, project_common_pch_header);
    bool has_project_common_pch_output = all_configs_have_same_value(default_pch_output, config_keys, project_common_pch_output);

    // Group files by their PCH settings for uses_pch() function
    struct PCHGroup {
        std::string mode;
        std::string header;
        std::string output;
        std::vector<std::string> files;

        bool operator<(const PCHGroup& other) const {
            if (mode != other.mode) return mode < other.mode;
            if (header != other.header) return header < other.header;
            return output < other.output;
        }
    };

    std::map<std::string, PCHGroup> pch_groups; // key is "mode|header|output"
    std::vector<const SourceFile*> files_with_other_settings;

    // First pass: categorize files
    for (const auto& src : project.sources) {
        bool has_other_settings = !src.settings.additional_includes.empty() ||
                                 !src.settings.preprocessor_defines.empty() ||
                                 !src.settings.additional_options.empty() ||
                                 !src.settings.excluded.empty() ||
                                 !src.settings.compile_as.empty() ||
                                 !src.settings.object_file.empty() ||
                                 !src.custom_command.empty();

        // Collect the PCH settings that differ from defaults per-config
        std::map<std::string, std::string> pch_modes_to_write;
        std::map<std::string, std::string> pch_headers_to_write;
        std::map<std::string, std::string> pch_outputs_to_write;

        for (const auto& [config_key, pch] : src.settings.pch) {
            bool mode_differs = (!pch.mode.empty() &&
                                (!default_pch_mode.count(config_key) ||
                                 pch.mode != default_pch_mode[config_key]));
            bool header_differs = (!pch.header.empty() &&
                                  (!default_pch_header.count(config_key) ||
                                   pch.header != default_pch_header[config_key]));

            if (mode_differs || pch.mode == "NotUsing")
                pch_modes_to_write[config_key] = pch.mode;
            if (header_differs)
                pch_headers_to_write[config_key] = pch.header;
            if (!pch.output.empty() && (!default_pch_output.count(config_key) || pch.output != default_pch_output[config_key]))
                pch_outputs_to_write[config_key] = pch.output;
        }

        // Check if the consolidated file settings match the consolidated project defaults
        std::string file_common_pch_mode, file_common_pch_header, file_common_pch_output;
        bool file_has_common_mode = all_configs_have_same_value(pch_modes_to_write, config_keys, file_common_pch_mode);
        bool file_has_common_header = all_configs_have_same_value(pch_headers_to_write, config_keys, file_common_pch_header);
        bool file_has_common_output = all_configs_have_same_value(pch_outputs_to_write, config_keys, file_common_pch_output);

        // If file settings consolidated and match project defaults consolidated, skip
        // Note: These variables are computed but not currently used - reserved for future validation
        (void)file_has_common_mode;
        (void)file_has_common_header;
        (void)file_has_common_output;
        (void)has_project_common_pch_mode;
        (void)has_project_common_pch_header;
        (void)has_project_common_pch_output;
        (void)project_common_pch_mode;
        (void)project_common_pch_header;
        (void)project_common_pch_output;

        // If a file has ANY explicit PCH settings in the original vcxproj, we need to preserve them
        // even if they match the project defaults, because they were explicitly set
        bool has_pch_exception = !pch_modes_to_write.empty() ||
                                !pch_headers_to_write.empty() ||
                                !pch_outputs_to_write.empty();

        // Categorize this file
        // For grouping, we need consistent PCH settings across configs
        // But only group if mode and header are consistent (output can be empty/consistent)
        bool can_be_grouped = has_pch_exception && !has_other_settings &&
                             file_has_common_mode &&
                             (file_has_common_header || pch_headers_to_write.empty()) &&
                             (file_has_common_output || pch_outputs_to_write.empty());

        if (can_be_grouped) {
            // This file only has PCH settings that are consistent across configs - add to group
            std::string key = file_common_pch_mode + "|" + file_common_pch_header + "|" + file_common_pch_output;
            if (pch_groups.find(key) == pch_groups.end()) {
                pch_groups[key].mode = file_common_pch_mode;
                pch_groups[key].header = file_common_pch_header;
                pch_groups[key].output = file_common_pch_output;
            }
            pch_groups[key].files.push_back(src.path);
        } else if (has_pch_exception || has_other_settings) {
            // This file has other settings or non-consolidated PCH - write individually
            files_with_other_settings.push_back(&src);
        }
    }

    // Write uses_pch() groups
    for (const auto& kv : pch_groups) {
        const auto& group = kv.second;
        // Use uses_pch() function if header is not empty (i.e., not for NotUsing files)
        bool use_uses_pch_function = group.files.size() >= 2 && !group.header.empty();

        if (use_uses_pch_function) {
            out << "\nuses_pch(\"" << group.mode << "\", \"" << group.header << "\"";
            if (!group.output.empty()) {
                out << ", \"" << group.output << "\"";
            }
            out << ", [\n";
            for (size_t i = 0; i < group.files.size(); i++) {
                out << "    \"" << group.files[i] << "\"";
                if (i < group.files.size() - 1) out << ",";
                out << "\n";
            }
            out << "])\n";
        } else {
            // For NotUsing or single files, use file_properties() or set_file_properties()
            if (group.files.size() >= 2) {
                // Multiple files with same PCH settings (e.g., NotUsing)
                out << "\nfile_properties(";
                for (size_t i = 0; i < group.files.size(); i++) {
                    out << group.files[i];
                    if (i < group.files.size() - 1) out << ", ";
                }
                out << ") {\n";
                out << "    pch[*] = " << group.mode << "\n";
                if (!group.header.empty()) {
                    out << "    pch_header[*] = " << group.header << "\n";
                }
                if (!group.output.empty()) {
                    out << "    pch_output[*] = " << group.output << "\n";
                }
                out << "}\n";
            } else {
                // Single file
                for (const auto& file_path : group.files) {
                    out << "\nset_file_properties(" << file_path << ",\n";
                    out << "    pch[*] = " << group.mode << "\n";
                    if (!group.header.empty()) {
                        out << "    pch_header[*] = " << group.header << "\n";
                    }
                    if (!group.output.empty()) {
                        out << "    pch_output[*] = " << group.output << "\n";
                    }
                    out << ")\n";
                }
            }
        }
    }

    // Group files with other settings by their settings signature
    std::map<std::string, std::vector<const SourceFile*>> file_groups;

    for (const auto* src : files_with_other_settings) {
        std::string signature = create_file_settings_signature(src, config_keys, default_pch_mode, default_pch_header,
                                                               has_project_common_pch_mode, has_project_common_pch_header,
                                                               has_project_common_pch_output, project_common_pch_mode,
                                                               project_common_pch_header, project_common_pch_output);
        file_groups[signature].push_back(src);
    }

    // Write grouped files using file_properties() or individual [file:...] sections
    for (const auto& group : file_groups) {
        const auto& files = group.second;
        if (files.empty()) continue;

        const SourceFile* first_src = files[0];

        // Check if this file/group has settings that need to be written
        bool has_pch_exception = false;
        bool has_other_settings = !first_src->settings.additional_includes.empty() ||
                                 !first_src->settings.preprocessor_defines.empty() ||
                                 !first_src->settings.additional_options.empty() ||
                                 !first_src->settings.excluded.empty() ||
                                 !first_src->settings.compile_as.empty() ||
                                 !first_src->settings.object_file.empty() ||
                                 !first_src->custom_command.empty();

        // Recalculate PCH settings for this file
        std::map<std::string, std::string> pch_modes_to_write;
        std::map<std::string, std::string> pch_headers_to_write;
        std::map<std::string, std::string> pch_outputs_to_write;

        for (const auto& pch_kv : first_src->settings.pch) {
            const std::string& config_key = pch_kv.first;
            const PrecompiledHeader& pch = pch_kv.second;
            bool mode_differs = (!pch.mode.empty() &&
                                (!default_pch_mode.count(config_key) ||
                                 pch.mode != default_pch_mode[config_key]));
            bool header_differs = (!pch.header.empty() &&
                                  (!default_pch_header.count(config_key) ||
                                   pch.header != default_pch_header[config_key]));

            if (mode_differs || pch.mode == "NotUsing")
                pch_modes_to_write[config_key] = pch.mode;
            if (header_differs)
                pch_headers_to_write[config_key] = pch.header;
            if (!pch.output.empty() && (!default_pch_output.count(config_key) || pch.output != default_pch_output[config_key]))
                pch_outputs_to_write[config_key] = pch.output;
        }

        std::string file_common_pch_mode, file_common_pch_header, file_common_pch_output;
        bool file_has_common_mode = all_configs_have_same_value(pch_modes_to_write, config_keys, file_common_pch_mode);
        bool file_has_common_header = all_configs_have_same_value(pch_headers_to_write, config_keys, file_common_pch_header);
        bool file_has_common_output = all_configs_have_same_value(pch_outputs_to_write, config_keys, file_common_pch_output);

        bool mode_matches_project = (file_has_common_mode && has_project_common_pch_mode &&
                                     file_common_pch_mode == project_common_pch_mode);
        bool header_matches_project = (file_has_common_header && has_project_common_pch_header &&
                                       file_common_pch_header == project_common_pch_header);
        bool output_matches_project = (file_has_common_output && has_project_common_pch_output &&
                                       file_common_pch_output == project_common_pch_output);

        if (!pch_modes_to_write.empty() && !mode_matches_project)
            has_pch_exception = true;
        if (!pch_headers_to_write.empty() && !header_matches_project)
            has_pch_exception = true;
        if (!pch_outputs_to_write.empty() && !output_matches_project)
            has_pch_exception = true;

        if (has_pch_exception || has_other_settings) {
            // Write header: either file_properties() for groups or set_file_properties() for single files
            std::string indent = "";

            if (files.size() > 1) {
                out << "\nfile_properties(";
                for (size_t i = 0; i < files.size(); ++i) {
                    out << files[i]->path;
                    if (i < files.size() - 1) out << ", ";
                }
                out << ") {\n";
                indent = "    ";  // 4 spaces for properties inside file_properties()
            } else {
                out << "\nset_file_properties(" << first_src->path << ",\n";
                indent = "    ";  // 4 spaces for properties
            }

            // Write PCH settings only if they differ from defaults
            if (has_pch_exception) {
                // Write consolidated PCH mode (only if doesn't match project default)
                if (!mode_matches_project) {
                    if (file_has_common_mode) {
                        out << indent << "pch[*] = " << file_common_pch_mode << "\n";
                    } else {
                        for (const auto& kv : pch_modes_to_write) {
                            out << indent << "pch[" << kv.first << "] = " << kv.second << "\n";
                        }
                    }
                }

                // Write consolidated PCH header (only if doesn't match project default)
                if (!header_matches_project) {
                    if (file_has_common_header) {
                        out << indent << "pch_header[*] = " << file_common_pch_header << "\n";
                    } else {
                        for (const auto& kv : pch_headers_to_write) {
                            out << indent << "pch_header[" << kv.first << "] = " << kv.second << "\n";
                        }
                    }
                }

                // Write consolidated PCH output (only if doesn't match project default)
                if (!output_matches_project) {
                    if (file_has_common_output) {
                        out << indent << "pch_output[*] = " << file_common_pch_output << "\n";
                    } else {
                        for (const auto& kv : pch_outputs_to_write) {
                            out << indent << "pch_output[" << kv.first << "] = " << kv.second << "\n";
                        }
                    }
                }
            }

            // Write additional includes - consolidate if same across all configs
            std::vector<std::string> common_includes;
            if (all_configs_have_same_vector(first_src->settings.additional_includes, config_keys, common_includes)) {
                if (!common_includes.empty())
                    out << indent << "includes[*] = " << join_vector(common_includes, ", ") << "\n";
            } else {
                for (const auto& kv : first_src->settings.additional_includes) {
                    if (!kv.second.empty())
                        out << indent << "includes[" << kv.first << "] = " << join_vector(kv.second, ", ") << "\n";
                }
            }

            // Write defines - consolidate if same across all configs
            std::vector<std::string> src_common_defines;
            if (all_configs_have_same_vector(first_src->settings.preprocessor_defines, config_keys, src_common_defines)) {
                if (!src_common_defines.empty())
                    out << indent << "defines[*] = " << join_vector(src_common_defines, ", ") << "\n";
            } else {
                for (const auto& kv : first_src->settings.preprocessor_defines) {
                    if (!kv.second.empty())
                        out << indent << "defines[" << kv.first << "] = " << join_vector(kv.second, ", ") << "\n";
                }
            }

            // Write additional options - consolidate if same across all configs
            std::vector<std::string> common_options;
            if (all_configs_have_same_vector(first_src->settings.additional_options, config_keys, common_options)) {
                if (!common_options.empty())
                    out << indent << "flags[*] = " << join_vector(common_options, ", ") << "\n";
            } else {
                for (const auto& kv : first_src->settings.additional_options) {
                    if (!kv.second.empty())
                        out << indent << "flags[" << kv.first << "] = " << join_vector(kv.second, ", ") << "\n";
                }
            }

            // Write excluded - consolidate if same across all configs
            bool common_excluded;
            if (all_configs_have_same_bool(first_src->settings.excluded, config_keys, common_excluded)) {
                if (common_excluded)
                    out << indent << "excluded[*] = true\n";
            } else {
                for (const auto& kv : first_src->settings.excluded) {
                    if (kv.second)
                        out << indent << "excluded[" << kv.first << "] = true\n";
                }
            }

            // Write compile_as - consolidate if same across all configs
            std::string common_compile_as;
            if (all_configs_have_same_value(first_src->settings.compile_as, config_keys, common_compile_as)) {
                if (!common_compile_as.empty())
                    out << indent << "compile_as[*] = " << common_compile_as << "\n";
            } else {
                for (const auto& kv : first_src->settings.compile_as) {
                    if (!kv.second.empty())
                        out << indent << "compile_as[" << kv.first << "] = " << kv.second << "\n";
                }
            }

            // Write object_file - consolidate if same across all configs
            std::string common_obj_file;
            if (all_configs_have_same_value(first_src->settings.object_file, config_keys, common_obj_file)) {
                if (!common_obj_file.empty())
                    out << indent << "object_file[*] = " << common_obj_file << "\n";
            } else {
                for (const auto& kv : first_src->settings.object_file) {
                    if (!kv.second.empty())
                        out << indent << "object_file[" << kv.first << "] = " << kv.second << "\n";
                }
            }

            // Write custom_command - consolidate if same across all configs
            std::string common_cmd;
            if (all_configs_have_same_value(first_src->custom_command, config_keys, common_cmd)) {
                if (!common_cmd.empty()) {
                    out << indent << "custom_command[*] = " << format_value(common_cmd) << "\n";
                    // Check outputs/messages
                    std::string common_outputs;
                    if (all_configs_have_same_value(first_src->custom_outputs, config_keys, common_outputs) && !common_outputs.empty()) {
                        out << indent << "custom_outputs[*] = " << common_outputs << "\n";
                    }
                    std::string common_message;
                    if (all_configs_have_same_value(first_src->custom_message, config_keys, common_message) && !common_message.empty()) {
                        out << indent << "custom_message[*] = " << format_value(common_message) << "\n";
                    }
                }
            } else {
                for (const auto& kv : first_src->custom_command) {
                    if (!kv.second.empty()) {
                        out << indent << "custom_command[" << kv.first << "] = " << format_value(kv.second) << "\n";
                        if (first_src->custom_outputs.count(kv.first))
                            out << indent << "custom_outputs[" << kv.first << "] = " << first_src->custom_outputs.at(kv.first) << "\n";
                        if (first_src->custom_message.count(kv.first))
                            out << indent << "custom_message[" << kv.first << "] = " << format_value(first_src->custom_message.at(kv.first)) << "\n";
                    }
                }
            }

            // Close file_properties() or set_file_properties()
            if (files.size() > 1) {
                out << "}\n";
            } else {
                out << ")\n";
            }
        }
    }
}

bool BuildscriptWriter::write_buildscript(const Project& project, const std::string& filepath,
                                         const std::vector<std::string>& configurations,
                                         const std::vector<std::string>& platforms) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        return false;
    }

    out << "# Generated buildscript from " << project.name << ".vcxproj\n";
    out << "# You may need to adjust paths and settings\n\n";

    write_project_content(out, project, filepath, configurations, platforms);

    out.close();
    return true;
}

// Helper function to determine if a buildscript should be merged with the solution buildscript
static bool should_merge_buildscript(
    const std::string& solution_name,
    const std::string& project_name,
    const std::string& vcxproj_rel_path)
{
    // Check if names match (case-insensitive comparison for Windows compatibility)
    std::string sln_lower = solution_name;
    std::string proj_lower = project_name;
    std::transform(sln_lower.begin(), sln_lower.end(), sln_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(proj_lower.begin(), proj_lower.end(), proj_lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    if (sln_lower != proj_lower) {
        return false;
    }

    // Check if vcxproj is in same directory as solution
    fs::path vcxproj_path(vcxproj_rel_path);
    fs::path parent = vcxproj_path.parent_path();

    // Empty or "." means same directory
    return parent.empty() || parent.string() == ".";
}

static std::string join_solution_values(const std::vector<std::string>& values, const std::string& sep = ", ") {
    if (values.empty()) return "";
    std::string result = values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        result += sep + values[i];
    }
    return result;
}

// Helper function to write [solution] section
static void write_solution_section(
    std::ostream& out,
    const std::string& solution_name,
    const std::string& solution_uuid,
    const std::vector<std::string>& configurations = {},
    const std::vector<std::string>& platforms = {})
{
    out << "[solution]\n";
    out << "name = " << solution_name << "\n";
    out << "uuid = " << solution_uuid << "\n\n";
    if (!configurations.empty()) {
        out << "configurations = " << join_solution_values(configurations) << "\n";
    }
    if (!platforms.empty()) {
        out << "platforms = " << join_solution_values(platforms) << "\n";
    }
    if (!configurations.empty() || !platforms.empty()) {
        out << "\n";
    }
}

bool BuildscriptWriter::write_solution_buildscripts(const Solution& solution, const std::string& base_dir) {
    fs::path sln_base = fs::path(base_dir);
    std::vector<std::string> buildscript_paths;
    std::vector<size_t> merged_project_indices;

    // Phase 1: Process projects - detect which ones should be merged
    for (size_t i = 0; i < solution.projects.size(); ++i) {
        const auto& project = solution.projects[i];

        if (should_merge_buildscript(solution.name, project.name, project.vcxproj_path)) {
            // Mark for merging - skip individual buildscript
            merged_project_indices.push_back(i);
            std::cout << "  Merging project '" << project.name
                      << "' with solution buildscript (same name and directory)\n";
        } else {
            // Generate individual buildscript (existing logic)
            fs::path vcxproj_rel_path(project.vcxproj_path);
            fs::path buildscript_path = sln_base / vcxproj_rel_path.parent_path() / (project.name + ".buildscript");

            std::cout << "  Generating: " << buildscript_path.string() << "\n";

            if (!write_buildscript(project, buildscript_path.string(),
                                  solution.configurations, solution.platforms)) {
                std::cerr << "Error: Failed to write " << buildscript_path.string() << "\n";
                return false;
            }

            // Store relative path from solution directory for include directive
            fs::path rel_include_path = vcxproj_rel_path.parent_path() / (project.name + ".buildscript");
            buildscript_paths.push_back(rel_include_path.string());
        }
    }

    // Phase 2: Generate merged buildscripts
    for (size_t idx : merged_project_indices) {
        const auto& project = solution.projects[idx];
        fs::path merged_path = sln_base / (solution.name + ".buildscript");

        std::cout << "  Generating merged: " << merged_path.string() << "\n";

        std::ofstream merged_out(merged_path);
        if (!merged_out.is_open()) {
            std::cerr << "Error: Failed to create merged buildscript: " << merged_path.string() << "\n";
            return false;
        }

        merged_out << "# Generated merged buildscript for solution and project: " << solution.name << "\n";
        merged_out << "# Solution and project share the same name and directory\n\n";

        // Write solution section
        write_solution_section(merged_out, solution.name, solution.uuid,
                               solution.configurations, solution.platforms);

        // Write project section (reusing existing logic)
        write_project_content(merged_out, project, merged_path.string(),
                             solution.configurations, solution.platforms);

        merged_out.close();
    }

    // Phase 3: Generate root buildscript only if there are non-merged projects
    if (!buildscript_paths.empty()) {
        fs::path root_buildscript = sln_base / (solution.name + ".buildscript");
        bool append_to_merged_root = !merged_project_indices.empty();
        std::ofstream root_out(root_buildscript,
                               append_to_merged_root ? std::ios::app : std::ios::out);
        if (!root_out.is_open()) {
            std::cerr << "Error: Failed to create root buildscript: " << root_buildscript.string() << "\n";
            return false;
        }

        std::cout << (append_to_merged_root ? "  Appending includes to root: " : "  Generating root: ")
                  << root_buildscript.string() << "\n";

        if (!append_to_merged_root) {
            root_out << "# Generated root buildscript for solution: " << solution.name << "\n";
            root_out << "# This file includes all project buildscripts\n\n";

            write_solution_section(root_out, solution.name, solution.uuid,
                                   solution.configurations, solution.platforms);
        } else {
            root_out << "\n# Included project buildscripts\n";
        }

        // Write include directives for non-merged projects
        for (const auto& include_path : buildscript_paths) {
            std::string normalized_path = include_path;
            std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
            root_out << "include = " << normalized_path << "\n";
        }

        root_out.close();
    }

    return true;
}

} // namespace vcxproj
