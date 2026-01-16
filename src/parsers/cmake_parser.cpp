#include "pch.h"
#include "cmake_parser.hpp"

namespace fs = std::filesystem;

namespace vcxproj {

namespace {
    // Helper to generate UUIDs (copied from buildscript_parser.cpp or project_types.hpp if available)
    // Since it's inline in project_types.hpp (based on previous read), I can use it if I include it.
    // Wait, project_types.hpp has inline generate_uuid(). Yes.
    
    std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    // Sanitize CMake target names for use as filenames
    // Replaces :: (namespace separator) with _ for filesystem compatibility
    std::string sanitize_target_name(const std::string& name) {
        std::string result = name;
        size_t pos = 0;
        while ((pos = result.find("::", pos)) != std::string::npos) {
            result.replace(pos, 2, "_");
            pos += 1;
        }
        return result;
    }
}

Solution CMakeParser::parse(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open CMakeLists.txt: " + filepath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    fs::path base = fs::path(filepath).parent_path();
    return parse_string(buffer.str(), base.empty() ? "." : base.string());
}

Solution CMakeParser::parse_string(const std::string& content, const std::string& base_path) {
    Solution solution;
    solution.uuid = generate_uuid();
    // Default configurations
    solution.configurations = {"Debug", "Release"};
    solution.platforms = {"Win32", "x64"};

    ParseState state;
    state.solution = &solution;
    state.base_path = base_path;
    
    // Initialize standard CMake variables
    state.variables["CMAKE_SOURCE_DIR"] = base_path;
    state.variables["CMAKE_CURRENT_SOURCE_DIR"] = base_path;
    state.variables["PROJECT_SOURCE_DIR"] = base_path;

    auto tokens = tokenize(content);

    size_t i = 0;
    execute_tokens(tokens, i, state);

    // Post-processing: ensure all projects have configurations
    for (auto& proj : solution.projects) {
        for (const auto& config_key : solution.get_config_keys()) {
            if (proj.configurations.find(config_key) == proj.configurations.end()) {
                auto& cfg = proj.configurations[config_key];
                // Apply sensible defaults
                auto [config, platform] = parse_config_key(config_key);

                cfg.windows_target_platform_version = "10.0";
                cfg.character_set = "MultiByte";

                if (config == "Debug") {
                    cfg.use_debug_libraries = true;
                    cfg.cl_compile.optimization = "Disabled";
                    cfg.cl_compile.runtime_library = "MultiThreadedDebug";
                    cfg.link.generate_debug_info = true;
                } else {
                    cfg.cl_compile.optimization = "MaxSpeed";
                    cfg.cl_compile.runtime_library = "MultiThreaded";
                    cfg.cl_compile.function_level_linking = true;
                    cfg.cl_compile.intrinsic_functions = true;
                    cfg.link.enable_comdat_folding = true;
                    cfg.link.optimize_references = true;
                }
            }
        }
    }

    // Post-process: Convert additional_dependencies to project_references if they match projects
    // This handles forward references where target_link_libraries is called before the library target is defined
    for (auto& proj : solution.projects) {
        for (auto& [config_key, cfg] : proj.configurations) {
            auto& deps = cfg.link.additional_dependencies;

            for (auto it = deps.begin(); it != deps.end(); ) {
                std::string dep_name = *it;

                // Remove .lib extension for comparison
                if (dep_name.size() > 4 && dep_name.substr(dep_name.size() - 4) == ".lib") {
                    dep_name = dep_name.substr(0, dep_name.size() - 4);
                }

                // Check if it matches a project name
                Project* dep_proj = nullptr;
                for (auto& p : solution.projects) {
                    if (p.name == dep_name) {
                        dep_proj = &p;
                        break;
                    }
                }

                if (dep_proj) {
                    // Move to project_references
                    auto ref_it = std::find_if(proj.project_references.begin(),
                                               proj.project_references.end(),
                                               [&](const ProjectDependency& d) {
                                                   return d.name == dep_name;
                                               });
                    if (ref_it == proj.project_references.end()) {
                        proj.project_references.push_back(ProjectDependency(dep_name));
                    }

                    // Remove from additional_dependencies
                    it = deps.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // Post-process: Propagate include directories from linked projects
    propagate_include_directories(solution);

    // If no project() command was found, use a default name
    if (solution.name.empty()) {
        solution.name = "CMakeProject";
    }

    return solution;
}

std::vector<CMakeParser::Token> CMakeParser::tokenize(const std::string& content) {
    std::vector<Token> tokens;
    size_t pos = 0;
    int line = 1;

    while (pos < content.size()) {
        char c = content[pos];

        if (std::isspace(c)) {
            if (c == '\n') line++;
            pos++;
            continue;
        }

        if (c == '#') {
            // Comment
            while (pos < content.size() && content[pos] != '\n') {
                pos++;
            }
            continue;
        }

        if (c == '(') {
            tokens.push_back({TokenType::OpenParen, "(", line});
            pos++;
        } else if (c == ')') {
            tokens.push_back({TokenType::CloseParen, ")", line});
            pos++;
        } else if (c == '"') {
            // String literal
            std::string value;
            pos++; // skip opening quote
            while (pos < content.size()) {
                if (content[pos] == '"' && (pos == 0 || content[pos-1] != '\\')) {
                    break;
                }
                value += content[pos];
                if (content[pos] == '\n') line++;
                pos++;
            }
            if (pos < content.size()) pos++; // skip closing quote
            tokens.push_back({TokenType::String, value, line});
        } else {
            // Identifier or unquoted string
            std::string value;
            while (pos < content.size()) {
                char current = content[pos];
                if (std::isspace(current) || current == '(' || current == ')' || current == '#') {
                    break;
                }
                value += current;
                pos++;
            }
            tokens.push_back({TokenType::Identifier, value, line});
        }
    }

    return tokens;
}

std::string CMakeParser::resolve_variables(const std::string& str, const ParseState& state) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find("${", pos)) != std::string::npos) {
        size_t end = result.find('}', pos);
        if (end == std::string::npos) break; // Unclosed variable
        
        std::string var_name = result.substr(pos + 2, end - (pos + 2));
        
        std::string var_value;
        auto it = state.variables.find(var_name);
        if (it != state.variables.end()) {
            var_value = it->second;
        }
        
        result.replace(pos, end - pos + 1, var_value);
        pos += var_value.length();
    }
    return result;
}

void CMakeParser::handle_project(const std::vector<std::string>& args, ParseState& state) {
    if (!args.empty()) {
        // Only set solution name from the first project() command (top-level)
        // Subdirectories can have their own project() calls, but those don't change the solution name
        if (state.solution->name.empty()) {
            state.solution->name = args[0];
        }
        state.variables["PROJECT_NAME"] = args[0];
    }
}

void CMakeParser::handle_add_executable(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;

    // Sanitize target name - replace :: with _ for filesystem compatibility
    std::string target_name = sanitize_target_name(args[0]);

    Project* proj = find_project(target_name, state);
    if (!proj) {
        state.solution->projects.emplace_back();
        proj = &state.solution->projects.back();
        proj->name = target_name;
        proj->project_name = target_name;
        proj->uuid = generate_uuid();
        proj->root_namespace = target_name;
    }

    // Set type to Application for all configs
    for (const auto& config_key : state.solution->get_config_keys()) {
        proj->configurations[config_key].config_type = "Application";
    }

    // Apply CMAKE_CXX_STANDARD if set
    auto std_it = state.variables.find("CMAKE_CXX_STANDARD");
    if (std_it != state.variables.end() && !std_it->second.empty()) {
        std::string std_value = "stdcpp" + std_it->second;
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj->configurations[config_key].cl_compile.language_standard = std_value;
        }
    }

    // Add sources (remaining args)
    std::vector<std::string> sources;
    for (size_t i = 1; i < args.size(); ++i) {
        // Skip keywords if any (WIN32, MACOSX_BUNDLE, EXCLUDE_FROM_ALL)
        if (args[i] == "WIN32" || args[i] == "MACOSX_BUNDLE" || args[i] == "EXCLUDE_FROM_ALL") continue;
        sources.push_back(args[i]);
    }
    add_sources_to_project(proj, sources, state);
}

void CMakeParser::handle_add_library(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;

    // Handle ALIAS targets - they don't create real build artifacts
    if (args.size() > 2 && args[1] == "ALIAS") {
        // Don't create a project for ALIAS targets
        // They're just references to existing targets
        return;
    }

    // Handle IMPORTED targets - they're external libraries, not built by this project
    if (args.size() > 1 && args[1] == "IMPORTED") {
        // Don't create a project for IMPORTED targets
        return;
    }

    // Sanitize target name - replace :: with _ for filesystem compatibility
    std::string target_name = sanitize_target_name(args[0]);

    Project* proj = find_project(target_name, state);
    if (!proj) {
        state.solution->projects.emplace_back();
        proj = &state.solution->projects.back();
        proj->name = target_name;
        proj->project_name = target_name;
        proj->uuid = generate_uuid();
        proj->root_namespace = target_name;
    }

    // Check for type
    std::string lib_type = "StaticLibrary";
    size_t source_start_idx = 1;

    if (args.size() > 1) {
        std::string type_arg = args[1];
        if (type_arg == "SHARED" || type_arg == "MODULE") {
            lib_type = "DynamicLibrary";
            source_start_idx = 2;
        } else if (type_arg == "STATIC") {
            lib_type = "StaticLibrary";
            source_start_idx = 2;
        } else if (type_arg == "INTERFACE") {
            // Interface libraries don't build anything, but we track them for includes
            // Use Utility type so no build artifact is generated
             lib_type = "Utility";
             source_start_idx = 2;
        }
    }

    // Set type for all configs
    for (const auto& config_key : state.solution->get_config_keys()) {
        proj->configurations[config_key].config_type = lib_type;
    }

    // Apply CMAKE_CXX_STANDARD if set
    auto std_it = state.variables.find("CMAKE_CXX_STANDARD");
    if (std_it != state.variables.end() && !std_it->second.empty()) {
        std::string std_value = "stdcpp" + std_it->second;
        for (const auto& config_key : state.solution->get_config_keys()) {
            proj->configurations[config_key].cl_compile.language_standard = std_value;
        }
    }

    // Add sources
    std::vector<std::string> sources;
    for (size_t i = source_start_idx; i < args.size(); ++i) {
        if (args[i] == "EXCLUDE_FROM_ALL") continue;
        sources.push_back(args[i]);
    }
    add_sources_to_project(proj, sources, state);
}

void CMakeParser::handle_add_subdirectory(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;

    std::string subdir = args[0];
    // Resolve path relative to current source dir
    fs::path subdir_path(subdir);
    if (!subdir_path.is_absolute()) {
        subdir_path = fs::path(state.base_path) / subdir_path;
    }

    // Look for CMakeLists.txt in the subdirectory
    fs::path cmakelists_path = subdir_path / "CMakeLists.txt";
    if (!fs::exists(cmakelists_path)) {
        std::cerr << "Warning: " << cmakelists_path << " not found in add_subdirectory call.\n";
        return;
    }

    // Create a new scope
    // We create a copy of the state but share the solution pointer
    ParseState sub_state = state;
    sub_state.base_path = subdir_path.string();
    sub_state.current_source_dir = subdir_path.string();
    
    // Update CMake variables for the new scope
    sub_state.variables["CMAKE_CURRENT_SOURCE_DIR"] = subdir_path.string();
    
    // Clear parent-scope vars before parsing subdirectory
    sub_state.parent_scope_vars.clear();

    // Read and parse
    try {
        std::ifstream file(cmakelists_path);
        if (!file.is_open()) return;

        std::stringstream buffer;
        buffer << file.rdbuf();

        auto tokens = tokenize(buffer.str());
        size_t i = 0;
        execute_tokens(tokens, i, sub_state);

        // Apply parent-scope variables back to parent state
        for (const auto& [name, value] : sub_state.parent_scope_vars) {
            state.variables[name] = value;
        }

        // Note: Other variables modified in sub_state are discarded (proper scoping)
        // Targets added to state.solution are preserved because it's a pointer.
    } catch (const std::exception& e) {
        std::cerr << "[CMake] Error parsing subdirectory " << subdir << ": " << e.what() << "\n";
    }
}

void CMakeParser::handle_set(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;

    std::string var_name = args[0];
    bool parent_scope = false;
    bool is_cache = false;

    // Check for PARENT_SCOPE and CACHE keywords
    size_t value_end = args.size();
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "PARENT_SCOPE") {
            parent_scope = true;
            value_end = i;
            break;
        }
        if (args[i] == "CACHE") {
            is_cache = true;
            value_end = i;
            // Note: CACHE has additional parameters (type, docstring) which we ignore for now
            break;
        }
    }

    // Collect value (skip variable name at index 0)
    std::string value;
    for (size_t i = 1; i < value_end; ++i) {
        if (i > 1) value += ";";
        value += args[i];
    }

    if (parent_scope) {
        // Set in parent scope (will be applied by caller)
        state.parent_scope_vars[var_name] = value;
    } else if (is_cache) {
        // For CACHE variables, set in current scope
        // In real CMake, CACHE variables persist across runs, but we simulate
        state.variables[var_name] = value;
    } else {
        // Normal variable set
        state.variables[var_name] = value;
    }

    // Handle special CMake variables
    if (var_name == "CMAKE_CXX_STANDARD" && !value.empty()) {
        // Apply C++ standard to all existing projects
        std::string std_value = "stdcpp" + value;
        for (auto& proj : state.solution->projects) {
            for (const auto& config_key : state.solution->get_config_keys()) {
                proj.configurations[config_key].cl_compile.language_standard = std_value;
            }
        }
    }
}

void CMakeParser::handle_option(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;
    std::string opt_name = args[0];
    std::string opt_val = "OFF";
    
    if (args.size() > 2) {
        opt_val = args[2];
    }
    
    // Only set if not already set (cache behavior simulation)
    if (state.variables.find(opt_name) == state.variables.end()) {
        state.variables[opt_name] = opt_val;
    }
}

void CMakeParser::handle_list(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;
    std::string subcmd = args[0];
    std::string list_name = args[1];
    
    std::string current_val = state.variables[list_name];
    std::vector<std::string> current_list;
    
    // Split current value by ;
    std::stringstream ss(current_val);
    std::string item;
    while (std::getline(ss, item, ';')) {
        current_list.push_back(item);
    }
    
    if (subcmd == "APPEND") {
        for (size_t i = 2; i < args.size(); ++i) {
            current_list.push_back(args[i]);
        }
    }
    // Other subcommands (REMOVE_ITEM, etc.) ignored for now
    
    // Join back
    std::string new_val;
    for (const auto& it : current_list) {
        if (!new_val.empty()) new_val += ";";
        new_val += it;
    }
    state.variables[list_name] = new_val;
}

void CMakeParser::handle_target_compile_options(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;
    std::string target_name = sanitize_target_name(args[0]);
    Project* proj = find_project(target_name, state);
    if (!proj) return;

    for (size_t i = 1; i < args.size(); ++i) {
        std::string opt = args[i];
        if (opt == "PRIVATE" || opt == "PUBLIC" || opt == "INTERFACE") continue;

        // Evaluate generator expressions if present
        if (is_generator_expression(opt)) {
            for (const auto& config_key : state.solution->get_config_keys()) {
                size_t pipe_pos = config_key.find('|');
                std::string config = config_key.substr(0, pipe_pos);
                std::string platform = (pipe_pos != std::string::npos) ? config_key.substr(pipe_pos + 1) : "";

                GenExprContext ctx;
                ctx.current_config = config;
                ctx.current_platform = platform;
                ctx.state = &state;
                ctx.project = proj;

                std::string evaluated = evaluate_generator_expression(opt, ctx);
                if (!evaluated.empty()) {
                    std::string& opts = proj->configurations[config_key].cl_compile.additional_options;
                    if (!opts.empty()) opts += " ";
                    opts += evaluated;
                }
            }
        } else {
            // Add to all configs
            for (const auto& config_key : state.solution->get_config_keys()) {
                std::string& opts = proj->configurations[config_key].cl_compile.additional_options;
                if (!opts.empty()) opts += " ";
                opts += opt;
            }
        }
    }
}

void CMakeParser::handle_find_library(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;
    std::string var_name = args[0];
    
    // Simulation: check if NAMES are provided and just set variable to first name
    // Real implementation would search file system.
    std::string lib_name;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "NAMES" && i + 1 < args.size()) {
            lib_name = args[i+1];
            break;
        }
    }
    
    if (!lib_name.empty()) {
        // Just set it to the name (assuming it's in system path)
        // or relative path if we want to simulate finding it?
        state.variables[var_name] = lib_name + ".lib"; // Assume .lib on Windows
    }
}

void CMakeParser::handle_find_path(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;
    std::string var_name = args[0];
    
    // Simulation: set to some dummy path or check HINTS
    std::string path;
    bool hints_next = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "HINTS") {
            hints_next = true;
            continue;
        }
        if (hints_next) {
            // Check if hint path exists
            // This list can be semicolon separated
            std::stringstream ss(args[i]);
            std::string hint;
            while(std::getline(ss, hint, ';')) {
                 fs::path p(hint);
                 if (p.is_relative()) p = fs::path(state.base_path) / p;
                 if (fs::exists(p)) {
                     path = p.string();
                     break;
                 }
            }
            if (!path.empty()) break;
        }
    }
    
    if (!path.empty()) {
        state.variables[var_name] = path;
    }
}

void CMakeParser::handle_find_package(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return;

    std::string package_name = args[0];
    bool required = false;
    std::vector<std::string> components;

    // Parse arguments
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "REQUIRED") {
            required = true;
        } else if (args[i] == "COMPONENTS") {
            // Collect components
            for (size_t j = i + 1; j < args.size(); ++j) {
                if (args[j] == "REQUIRED" || args[j] == "QUIET" || args[j] == "CONFIG") break;
                components.push_back(args[j]);
            }
            break;
        }
    }

    // Set found variable
    state.variables[package_name + "_FOUND"] = "TRUE";
    state.variables[package_name + "_VERSION"] = "1.0.0";

    // Check for environment variable override
    std::string env_var = package_name + "_DIR";
    const char* env_path = std::getenv(env_var.c_str());

    if (env_path) {
        state.variables[package_name + "_DIR"] = env_path;
        state.variables[package_name + "_INCLUDE_DIRS"] = std::string(env_path) + "/include";
        state.variables[package_name + "_LIBRARIES"] = package_name;
        std::cout << "[CMake] Found package " << package_name << " at " << env_path << "\n";
    } else {
        // Hardcoded well-known packages
        if (package_name == "Boost") {
            state.variables["Boost_INCLUDE_DIRS"] = "C:/boost/include";
            state.variables["Boost_LIBRARY_DIRS"] = "C:/boost/lib";
            std::string libs;
            for (const auto& comp : components) {
                if (!libs.empty()) libs += ";";
                libs += "boost_" + comp;
            }
            state.variables["Boost_LIBRARIES"] = libs.empty() ? "boost_system" : libs;
        } else if (package_name == "OpenGL") {
            state.variables["OPENGL_FOUND"] = "TRUE";
            state.variables["OPENGL_INCLUDE_DIR"] = "";
            state.variables["OPENGL_LIBRARIES"] = "opengl32.lib";
        } else if (package_name == "Threads") {
            state.variables["CMAKE_THREAD_LIBS_INIT"] = "";
            state.variables["Threads_FOUND"] = "TRUE";
        } else if (package_name == "OpenCV") {
            state.variables["OpenCV_INCLUDE_DIRS"] = "C:/opencv/include";
            state.variables["OpenCV_LIBS"] = "opencv_core;opencv_imgproc;opencv_highgui";
        } else if (package_name == "Qt5" || package_name == "Qt6") {
            state.variables[package_name + "_FOUND"] = "TRUE";
            state.variables[package_name + "_INCLUDE_DIRS"] = "C:/" + package_name + "/include";
            for (const auto& comp : components) {
                state.variables[package_name + comp + "_FOUND"] = "TRUE";
                state.variables[package_name + comp + "_LIBRARIES"] = "Qt::" + comp;
            }
        } else if (package_name == "GTest" || package_name == "gtest") {
            state.variables["GTest_FOUND"] = "TRUE";
            state.variables["GTEST_INCLUDE_DIRS"] = "";
            state.variables["GTEST_LIBRARIES"] = "gtest;gtest_main";
            state.variables["GTEST_MAIN_LIBRARIES"] = "gtest_main";
        } else {
            // Generic simulation
            state.variables[package_name + "_INCLUDE_DIRS"] = "";
            state.variables[package_name + "_LIBRARIES"] = package_name;
        }

        std::cout << "[CMake] Simulated finding package " << package_name;
        if (!components.empty()) {
            std::cout << " (components: ";
            for (size_t i = 0; i < components.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << components[i];
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }
}

void CMakeParser::handle_cmake_minimum_required(const std::vector<std::string>& /*args*/, ParseState& /*state*/) {
    // No-op
}

void CMakeParser::handle_enable_testing(const std::vector<std::string>& /*args*/, ParseState& /*state*/) {
    // No-op
}

void CMakeParser::handle_add_test(const std::vector<std::string>& /*args*/, ParseState& /*state*/) {
    // No-op
}

void CMakeParser::handle_target_include_directories(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;

    std::string target_name = sanitize_target_name(args[0]);
    Project* proj = find_project(target_name, state);
    if (!proj) return;

    // Iterate args, skipping PUBLIC/PRIVATE/INTERFACE keywords
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "PUBLIC" || arg == "PRIVATE" || arg == "INTERFACE") continue;

        // Evaluate generator expressions for each configuration
        for (const auto& config_key : state.solution->get_config_keys()) {
            // Parse config_key to get config and platform
            size_t pipe_pos = config_key.find('|');
            std::string config = config_key.substr(0, pipe_pos);
            std::string platform = (pipe_pos != std::string::npos) ? config_key.substr(pipe_pos + 1) : "";

            // Setup generator expression context
            GenExprContext ctx;
            ctx.current_config = config;
            ctx.current_platform = platform;
            ctx.state = &state;
            ctx.project = proj;

            // Evaluate generator expression
            std::string inc_dir = evaluate_generator_expression(arg, ctx);

            // Skip if expression evaluated to empty
            if (inc_dir.empty()) continue;

            // Resolve path relative to current source dir if relative
            fs::path p(inc_dir);
            if (!p.is_absolute()) {
                fs::path base_path_normalized = fs::path(state.base_path).lexically_normal();
                fs::path inc_dir_normalized = fs::path(inc_dir).lexically_normal();

                // Convert to strings with forward slashes for consistent comparison
                std::string base_str = base_path_normalized.string();
                std::string inc_str = inc_dir_normalized.string();
                std::replace(base_str.begin(), base_str.end(), '\\', '/');
                std::replace(inc_str.begin(), inc_str.end(), '\\', '/');

                // Check if inc_dir equals or starts with base_path
                // If so, it's already project-root-relative (from CMAKE_CURRENT_SOURCE_DIR)
                if (inc_str == base_str || inc_str.find(base_str + "/") == 0) {
                    // Already project-root-relative, use as-is
                    p = inc_dir_normalized;
                } else {
                    // Truly relative path (like "include" or "../other"), resolve it
                    p = base_path_normalized / inc_dir_normalized;
                }
            } else {
                p = fs::path(inc_dir).lexically_normal();
            }

            proj->configurations[config_key].cl_compile.additional_include_directories.push_back(p.string());
        }
    }
}

void CMakeParser::handle_target_link_libraries(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;

    std::string target_name = sanitize_target_name(args[0]);
    Project* proj = find_project(target_name, state);
    if (!proj) return;

    // State machine: track current visibility (default PUBLIC for backward compat)
    DependencyVisibility current_visibility = DependencyVisibility::PUBLIC;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        // Check for visibility keywords (DON'T skip, track them!)
        if (arg == "PUBLIC") {
            current_visibility = DependencyVisibility::PUBLIC;
            continue;
        } else if (arg == "PRIVATE") {
            current_visibility = DependencyVisibility::PRIVATE;
            continue;
        } else if (arg == "INTERFACE") {
            current_visibility = DependencyVisibility::INTERFACE;
            continue;
        }

        // If there's a generator expression, evaluate it per-config
        if (is_generator_expression(arg)) {
            for (const auto& config_key : state.solution->get_config_keys()) {
                // Parse config_key to get config and platform
                size_t pipe_pos = config_key.find('|');
                std::string config = config_key.substr(0, pipe_pos);
                std::string platform = (pipe_pos != std::string::npos) ? config_key.substr(pipe_pos + 1) : "";

                // Setup generator expression context
                GenExprContext ctx;
                ctx.current_config = config;
                ctx.current_platform = platform;
                ctx.state = &state;
                ctx.project = proj;

                // Evaluate generator expression
                std::string evaluated = evaluate_generator_expression(arg, ctx);
                if (evaluated.empty()) continue;

                // Process the evaluated library for this config
                std::string sanitized_name = sanitize_target_name(evaluated);

                // Check if it's an internal project reference
                Project* dep_proj = find_project(sanitized_name, state);
                if (dep_proj) {
                    // Add to project_references with visibility
                    auto it = std::find_if(proj->project_references.begin(),
                                           proj->project_references.end(),
                                           [&](const ProjectDependency& d) {
                                               return d.name == sanitized_name;
                                           });
                    if (it == proj->project_references.end()) {
                        proj->project_references.push_back(ProjectDependency(sanitized_name, current_visibility));
                    } else {
                        it->visibility = current_visibility;  // Update visibility
                    }
                    continue;
                }

                // System/external library for this config only
                std::string lib_name = sanitized_name;
                fs::path lib_path(sanitized_name);
                if (!lib_path.has_extension()) {
                    lib_name += ".lib";
                }

                auto& deps = proj->configurations[config_key].link.additional_dependencies;
                auto it = std::find(deps.begin(), deps.end(), lib_name);
                if (it == deps.end()) {
                    deps.push_back(lib_name);
                }
            }
            continue;
        }

        // No generator expression - original logic
        std::string sanitized_name = sanitize_target_name(arg);

        // 1. Check if it's an internal project reference
        Project* dep_proj = find_project(sanitized_name, state);
        if (dep_proj) {
            // Internal project - add to project_references with visibility
            auto it = std::find_if(proj->project_references.begin(),
                                   proj->project_references.end(),
                                   [&](const ProjectDependency& d) {
                                       return d.name == sanitized_name;
                                   });
            if (it == proj->project_references.end()) {
                proj->project_references.push_back(ProjectDependency(sanitized_name, current_visibility));
            } else {
                it->visibility = current_visibility;  // Update visibility
            }
            continue;
        }

        // 2. Check if it's a file-based library (has path separators)
        if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos) {
            // File-based library - resolve path and add to libraries
            fs::path lib_path(arg);
            if (!lib_path.is_absolute()) {
                lib_path = fs::path(state.base_path) / lib_path;
            }

            // Check if already in libraries
            auto it = std::find_if(proj->libraries.begin(), proj->libraries.end(),
                                  [&](const LibraryFile& lf) {
                                      return lf.path == lib_path.string();
                                  });
            if (it == proj->libraries.end()) {
                LibraryFile lf;
                lf.path = lib_path.string();
                proj->libraries.push_back(lf);
            }
            continue;
        }

        // 3. System/external library - add to additional_dependencies per-config
        std::string lib_name = sanitized_name;

        // Only add .lib extension if it doesn't already have an extension
        fs::path lib_path(sanitized_name);
        if (!lib_path.has_extension()) {
            lib_name += ".lib";
        }

        // Add to all configurations
        for (const auto& config_key : state.solution->get_config_keys()) {
            auto& deps = proj->configurations[config_key].link.additional_dependencies;

            // Avoid duplicates
            auto it = std::find(deps.begin(), deps.end(), lib_name);
            if (it == deps.end()) {
                deps.push_back(lib_name);
            }
        }
    }
}

void CMakeParser::handle_target_compile_definitions(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;

    std::string target_name = sanitize_target_name(args[0]);
    Project* proj = find_project(target_name, state);
    if (!proj) return;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "PUBLIC" || arg == "PRIVATE" || arg == "INTERFACE") continue;

        // Evaluate generator expressions if present
        if (is_generator_expression(arg)) {
            // Evaluate for each configuration
            for (const auto& config_key : state.solution->get_config_keys()) {
                size_t pipe_pos = config_key.find('|');
                std::string config = config_key.substr(0, pipe_pos);
                std::string platform = (pipe_pos != std::string::npos) ? config_key.substr(pipe_pos + 1) : "";

                GenExprContext ctx;
                ctx.current_config = config;
                ctx.current_platform = platform;
                ctx.state = &state;
                ctx.project = proj;

                std::string evaluated = evaluate_generator_expression(arg, ctx);
                if (!evaluated.empty()) {
                    proj->configurations[config_key].cl_compile.preprocessor_definitions.push_back(evaluated);
                }
            }
        } else {
            // Add to project level definitions (applies to all configs)
            proj->project_level_preprocessor_definitions.push_back(arg);
        }
    }
}

void CMakeParser::handle_target_sources(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 2) return;
    
    std::string target_name = sanitize_target_name(args[0]);
    Project* proj = find_project(target_name, state);
    if (!proj) return;

    std::vector<std::string> sources;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "PUBLIC" || arg == "PRIVATE" || arg == "INTERFACE") continue;
        sources.push_back(arg);
    }
    add_sources_to_project(proj, sources, state);
}

void CMakeParser::handle_include_directories(const std::vector<std::string>& args, ParseState& state) {
    // Apply to ALL projects defined SO FAR? Or global?
    // CMake applies to targets defined in current scope and below.
    // For simplicity, we apply to all currently existing projects.
    for (size_t i = 0; i < args.size(); ++i) {
        std::string inc_dir = args[i];
        fs::path p(inc_dir);
        if (!p.is_absolute()) {
            p = fs::path(state.base_path) / p;
        }

        for (auto& proj : state.solution->projects) {
            for (const auto& config_key : state.solution->get_config_keys()) {
                proj.configurations[config_key].cl_compile.additional_include_directories.push_back(p.string());
            }
        }
    }
}

void CMakeParser::handle_link_directories(const std::vector<std::string>& args, ParseState& state) {
    for (size_t i = 0; i < args.size(); ++i) {
        std::string lib_dir = args[i];
        fs::path p(lib_dir);
        if (!p.is_absolute()) {
            p = fs::path(state.base_path) / p;
        }

        for (auto& proj : state.solution->projects) {
            for (const auto& config_key : state.solution->get_config_keys()) {
                proj.configurations[config_key].link.additional_library_directories.push_back(p.string());
            }
        }
    }
}

void CMakeParser::handle_file(const std::vector<std::string>& args, ParseState& state) {
    if (args.size() < 3) return;

    std::string mode = args[0];
    if (mode == "GLOB" || mode == "GLOB_RECURSE") {
        std::string out_var = args[1];
        std::vector<std::string> patterns;
        for (size_t i = 2; i < args.size(); ++i) {
            patterns.push_back(args[i]);
        }

        bool recursive = (mode == "GLOB_RECURSE");
        std::vector<std::string> found_files;

        for (const auto& pattern : patterns) {
            fs::path base_search_path = state.base_path;
            std::string filename_pattern = pattern;
            
            if (pattern.find('/') != std::string::npos || pattern.find('\\') != std::string::npos) {
                 fs::path p(pattern);
                 if (p.has_parent_path()) {
                     base_search_path /= p.parent_path();
                     filename_pattern = p.filename().string();
                 } else {
                     filename_pattern = p.string();
                 }
            }
            
            std::string regex_str = "^";
            for (char c : filename_pattern) {
                if (c == '*') regex_str += ".*";
                else if (c == '.') regex_str += "\\.";
                else regex_str += c;
            }
            regex_str += "$";
            
            try {
                std::regex re(regex_str, std::regex::icase);
                if (fs::exists(base_search_path)) {
                    if (recursive) {
                        for (const auto& entry : fs::recursive_directory_iterator(base_search_path)) {
                            if (entry.is_regular_file()) {
                                if (std::regex_match(entry.path().filename().string(), re)) {
                                    found_files.push_back(entry.path().string());
                                }
                            }
                        }
                    } else {
                         for (const auto& entry : fs::directory_iterator(base_search_path)) {
                            if (entry.is_regular_file()) {
                                if (std::regex_match(entry.path().filename().string(), re)) {
                                    found_files.push_back(entry.path().string());
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        std::string result;
        for (const auto& f : found_files) {
            if (!result.empty()) result += ";";
            result += f;
        }
        state.variables[out_var] = result;
    }
}

Project* CMakeParser::find_project(const std::string& name, ParseState& state) {
    for (auto& proj : state.solution->projects) {
        if (proj.name == name) return &proj;
    }
    return nullptr;
}

void CMakeParser::add_sources_to_project(Project* project, const std::vector<std::string>& sources, ParseState& state) {
    for (const auto& src : sources) {
        // Handle list expansion (semicolon separated in one string)
        std::stringstream ss(src);
        std::string item;
        while (std::getline(ss, item, ';')) {
            if (item.empty()) continue;
            
            // Check for wildcards
            if (item.find('*') != std::string::npos) {
                 // Expand glob
                 auto expanded = expand_glob(item, state.base_path);
                 for (const auto& ex : expanded) {
                     project->sources.push_back({ex, get_file_type(ex)});
                 }
            } else {
                 // Normal file
                 fs::path p(item);
                 if (!p.is_absolute()) {
                     p = fs::path(state.base_path) / p;
                 }
                 project->sources.push_back({p.string(), get_file_type(p.string())});
            }
        }
    }
}

std::vector<std::string> CMakeParser::expand_glob(const std::string& pattern, const std::string& base_path) {
    std::vector<std::string> result;

    // Check if pattern includes ** for recursive search
    bool recursive = pattern.find("**") != std::string::npos;

    std::string dir;
    std::string file_pattern;

    if (recursive) {
        // Extract the base directory (part before **)
        size_t star_pos = pattern.find("**");
        std::string prefix = pattern.substr(0, star_pos);

        // Remove trailing slash/backslash from prefix
        while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\')) {
            prefix.pop_back();
        }

        // Get the file pattern (part after **)
        std::string suffix = pattern.substr(star_pos + 2);
        // Remove leading slash/backslash from suffix
        while (!suffix.empty() && (suffix.front() == '/' || suffix.front() == '\\')) {
            suffix.erase(0, 1);
        }

        // Directory is the prefix, file pattern is the filename part of suffix
        dir = prefix.empty() ? "." : prefix;
        fs::path suffix_path(suffix);
        file_pattern = suffix_path.filename().string();
    } else {
        fs::path full_pattern = fs::path(base_path) / pattern;
        dir = full_pattern.parent_path().string();
        file_pattern = full_pattern.filename().string();
    }

    if (dir.empty()) dir = ".";

    // Check if this is a wildcard pattern
    if (file_pattern.find('*') == std::string::npos && file_pattern.find('?') == std::string::npos) {
        // Not a wildcard, just return the path
        result.push_back(pattern);
        return result;
    }

    // Convert wildcard to regex
    std::string regex_pattern = file_pattern;
    // Escape special regex characters except * and ?
    for (size_t i = 0; i < regex_pattern.size(); ++i) {
        char c = regex_pattern[i];
        if (c == '.' || c == '+' || c == '[' || c == ']' ||
            c == '(' || c == ')' || c == '{' || c == '}' || c == '^' || c == '$') {
            regex_pattern.insert(i, "\\");
            ++i;
        } else if (c == '*') {
            regex_pattern.replace(i, 1, ".*");
            ++i;
        } else if (c == '?') {
            regex_pattern.replace(i, 1, ".");
        }
    }

    std::regex re(regex_pattern, std::regex::icase);

    try {
        fs::path search_dir = fs::path(base_path) / dir;
        if (fs::exists(search_dir) && fs::is_directory(search_dir)) {
            if (recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(search_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (std::regex_match(filename, re)) {
                            // Make path relative to base_path
                            std::string rel_path = fs::relative(entry.path(), base_path).string();
                            // Normalize path separators to forward slashes (CMake convention)
                            std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
                            result.push_back(rel_path);
                        }
                    }
                }
            } else {
                for (const auto& entry : fs::directory_iterator(search_dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (std::regex_match(filename, re)) {
                            // Make path relative to base_path
                            std::string rel_path = fs::relative(entry.path(), base_path).string();
                            // Normalize path separators to forward slashes (CMake convention)
                            std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
                            result.push_back(rel_path);
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CMake] Warning: Error expanding glob " << pattern << ": " << e.what() << "\n";
    }

    return result;
}

bool CMakeParser::is_generator_expression(const std::string& str) {
    return str.find("$<") != std::string::npos;
}

std::string CMakeParser::evaluate_generator_expression(const std::string& expr, const GenExprContext& ctx) {
    if (!is_generator_expression(expr)) return expr;

    std::string result = expr;
    size_t pos = 0;

    while ((pos = result.find("$<", pos)) != std::string::npos) {
        // Find matching >
        int depth = 1;
        size_t end = pos + 2;
        while (end < result.size() && depth > 0) {
            if (result[end] == '<' && end > 0 && result[end-1] == '$') depth++;
            if (result[end] == '>') depth--;
            end++;
        }

        if (depth != 0) {
            std::cerr << "[CMake] Warning: Unmatched generator expression: " << expr << "\n";
            return result;
        }

        // Extract expression content
        std::string gen_expr = result.substr(pos + 2, end - pos - 3);
        std::string replacement = evaluate_simple_gen_expr(gen_expr, ctx);

        result.replace(pos, end - pos, replacement);
        pos += replacement.size();
    }

    return result;
}

std::string CMakeParser::evaluate_simple_gen_expr(const std::string& expr, const GenExprContext& ctx) {
    // Parse expression type and arguments
    size_t colon = expr.find(':');

    // Boolean expressions without colon
    if (colon == std::string::npos) {
        // $<0>, $<1> - literal boolean
        if (expr == "0") return "0";
        if (expr == "1") return "1";

        // Check for boolean prefix
        if (expr.find("BOOL:") == 0) {
            std::string val = expr.substr(5);
            // Recursively evaluate in case it's nested
            val = evaluate_generator_expression(val, ctx);
            return (val == "1" || val == "ON" || val == "TRUE" || val == "true") ? "1" : "0";
        }

        return "";
    }

    std::string type = expr.substr(0, colon);
    std::string args = expr.substr(colon + 1);

    // CONFIG expression: $<CONFIG:Debug>
    if (type == "CONFIG") {
        return (args == ctx.current_config) ? "1" : "0";
    }

    // Platform/architecture checks
    if (type == "PLATFORM_ID") {
        #ifdef _WIN32
        return (args == "Windows") ? "1" : "0";
        #else
        return (args == "Linux") ? "1" : "0";
        #endif
    }

    // BUILD_INTERFACE: Always include (sighmake is build-time)
    if (type == "BUILD_INTERFACE") {
        return evaluate_generator_expression(args, ctx);
    }

    // INSTALL_INTERFACE: Never include (sighmake doesn't handle install)
    if (type == "INSTALL_INTERFACE") {
        return "";
    }

    // TARGET_PROPERTY: Access target properties
    if (type == "TARGET_PROPERTY") {
        // Format: TARGET_PROPERTY:target,property or TARGET_PROPERTY:property (for current target)
        // For now, return empty as we don't track all properties
        return "";
    }

    // Conditional: $<$<condition>:true_value>
    if (type.find("$<") == 0) {
        std::string condition = type;
        std::string true_value = args;
        std::string cond_result = evaluate_generator_expression(condition, ctx);
        if (cond_result == "1" || cond_result == "TRUE" || cond_result == "true") {
            return evaluate_generator_expression(true_value, ctx);
        }
        return "";
    }

    // Logical operators
    if (type == "AND") {
        // $<AND:expr1,expr2,...>
        std::vector<std::string> parts;
        std::string current;
        int depth = 0;
        for (char c : args) {
            if (c == '$' && current.size() > 0 && current.back() == '<') depth++;
            if (c == '>') depth--;
            if (c == ',' && depth == 0) {
                parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(current);

        for (const auto& part : parts) {
            std::string result = evaluate_generator_expression(part, ctx);
            if (result != "1") return "0";
        }
        return "1";
    }

    if (type == "OR") {
        // $<OR:expr1,expr2,...>
        std::vector<std::string> parts;
        std::string current;
        int depth = 0;
        for (char c : args) {
            if (c == '$' && current.size() > 0 && current.back() == '<') depth++;
            if (c == '>') depth--;
            if (c == ',' && depth == 0) {
                parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(current);

        for (const auto& part : parts) {
            std::string result = evaluate_generator_expression(part, ctx);
            if (result == "1") return "1";
        }
        return "0";
    }

    if (type == "NOT") {
        std::string result = evaluate_generator_expression(args, ctx);
        return (result == "1") ? "0" : "1";
    }

    // COMPILE_LANGUAGE: Language-specific expressions
    if (type == "COMPILE_LANGUAGE") {
        // For C++ projects, assume CXX
        return (args == "CXX" || args == "C") ? "1" : "0";
    }

    // Unknown expression - warn and return empty
    std::cerr << "[CMake] Warning: Unknown generator expression: $<" << expr << ">\n";
    return "";
}

std::vector<CMakeParser::Token> CMakeParser::capture_until(const std::string& end_keyword, size_t& i, const std::vector<Token>& tokens) {
    std::vector<Token> body;
    int depth = 1;

    while (i < tokens.size() && depth > 0) {
        if (tokens[i].type == TokenType::Identifier) {
            std::string cmd = tokens[i].value;
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            // Increase depth for nested control structures
            if (cmd == "foreach" || cmd == "while" || cmd == "if" ||
                cmd == "function" || cmd == "macro") {
                depth++;
            }
            // Decrease depth for matching end keywords
            if (cmd == end_keyword) {
                depth--;
                if (depth == 0) {
                    i++; // Skip the end keyword
                    break;
                }
            }
        }
        body.push_back(tokens[i]);
        i++;
    }

    return body;
}

void CMakeParser::handle_foreach(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state) {
    if (args.empty()) {
        std::cerr << "[CMake] Error: foreach() requires at least 1 argument\n";
        return;
    }

    std::string loop_var = args[0];
    std::vector<std::string> items;

    // Parse foreach syntax
    if (args.size() > 2 && args[1] == "IN") {
        std::string mode = args[2];
        if (mode == "LISTS") {
            // foreach(var IN LISTS list1 list2 ...)
            for (size_t j = 3; j < args.size(); ++j) {
                std::string list_val = state.variables[args[j]];
                // Split by semicolon
                std::stringstream ss(list_val);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    if (!item.empty()) items.push_back(item);
                }
            }
        } else if (mode == "ITEMS") {
            // foreach(var IN ITEMS item1 item2 ...)
            items.assign(args.begin() + 3, args.end());
        }
    } else if (args.size() > 2 && args[1] == "RANGE") {
        // foreach(var RANGE stop) or foreach(var RANGE start stop [step])
        int start = 0, stop = 0, step = 1;
        try {
            if (args.size() == 3) {
                stop = std::stoi(args[2]);
            } else if (args.size() >= 4) {
                start = std::stoi(args[2]);
                stop = std::stoi(args[3]);
                if (args.size() >= 5) step = std::stoi(args[4]);
            }
            for (int val = start; val <= stop; val += step) {
                items.push_back(std::to_string(val));
            }
        } catch (const std::exception&) {
            std::cerr << "[CMake] Error: Invalid RANGE parameters in foreach()\n";
            // Skip the loop body
            capture_until("endforeach", i, tokens);
            return;
        }
    } else {
        // Simple: foreach(var item1 item2 ...)
        items.assign(args.begin() + 1, args.end());
    }

    // Capture loop body
    std::vector<Token> body = capture_until("endforeach", i, tokens);

    // Save old variable value
    auto old_it = state.variables.find(loop_var);
    std::string old_value;
    bool had_value = (old_it != state.variables.end());
    if (had_value) old_value = old_it->second;

    // Execute loop
    for (const auto& item : items) {
        state.variables[loop_var] = item;
        size_t body_i = 0;
        execute_tokens(body, body_i, state);
    }

    // Restore old variable value
    if (had_value) {
        state.variables[loop_var] = old_value;
    } else {
        state.variables.erase(loop_var);
    }
}

void CMakeParser::execute_tokens(const std::vector<Token>& tokens, size_t& i, ParseState& state) {
    while (i < tokens.size()) {
        if (tokens[i].type == TokenType::Identifier) {
            std::string command = tokens[i].value;
            // Case-insensitive command matching
            std::transform(command.begin(), command.end(), command.begin(), 
                          [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            
            i++; // Skip command
            
            if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                i++; // Skip (
                
                std::vector<std::string> args;
                while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) {
                    if (tokens[i].type == TokenType::String || tokens[i].type == TokenType::Identifier) {
                        // Resolve variables in arguments
                        std::string arg = resolve_variables(tokens[i].value, state);
                        args.push_back(arg);
                    }
                    i++;
                }
                
                if (i < tokens.size() && tokens[i].type == TokenType::CloseParen) {
                    i++; // Skip )
                    
                    // Dispatch command
                    if (command == "project") handle_project(args, state);
                    else if (command == "add_executable") handle_add_executable(args, state);
                    else if (command == "add_library") handle_add_library(args, state);
                    else if (command == "add_subdirectory") handle_add_subdirectory(args, state);
                    else if (command == "set") handle_set(args, state);
                    else if (command == "option") handle_option(args, state);
                    else if (command == "list") handle_list(args, state);
                    else if (command == "target_include_directories") handle_target_include_directories(args, state);
                    else if (command == "target_link_libraries") handle_target_link_libraries(args, state);
                    else if (command == "target_compile_definitions") handle_target_compile_definitions(args, state);
                    else if (command == "target_compile_options") handle_target_compile_options(args, state);
                    else if (command == "target_sources") handle_target_sources(args, state);
                    else if (command == "include_directories") handle_include_directories(args, state);
                    else if (command == "link_directories") handle_link_directories(args, state);
                    else if (command == "file") handle_file(args, state);
                    else if (command == "find_library") handle_find_library(args, state);
                    else if (command == "find_path") handle_find_path(args, state);
                    else if (command == "find_package") handle_find_package(args, state);
                    else if (command == "cmake_minimum_required") handle_cmake_minimum_required(args, state);
                    else if (command == "enable_testing") handle_enable_testing(args, state);
                    else if (command == "add_test") handle_add_test(args, state);
                    else if (command == "message") handle_message(args, state);
                    else if (command == "function") handle_function_def(args, i, tokens, state);
                    else if (command == "macro") handle_macro_def(args, i, tokens, state);
                    else if (command == "if") handle_if(args, i, tokens, state);
                    else if (command == "while") handle_while(args, i, tokens, state);
                    else if (command == "foreach") handle_foreach(args, i, tokens, state);
                    else if (command == "return") return; // Return from function/file
                    else {
                        // Check user-defined functions
                        auto it = state.functions.find(command);
                        if (it != state.functions.end()) {
                            // Execute function
                            ParseState func_state = state; // Copy global state
                            
                            // Map arguments
                            for (size_t k = 0; k < it->second.params.size() && k < args.size(); ++k) {
                                func_state.variables[it->second.params[k]] = args[k];
                            }
                            
                            // ARGN support (simple version)
                            std::string argn;
                            for (size_t k = it->second.params.size(); k < args.size(); ++k) {
                                if (!argn.empty()) argn += ";";
                                argn += args[k];
                            }
                            func_state.variables["ARGN"] = argn;
                            
                            size_t func_i = 0;
                            execute_tokens(it->second.body, func_i, func_state);
                            
                            // Propagate changes back for parent scope variables if explicitly set?
                            // CMake functions have new scope, macros don't.
                            // This implementation copies state, so it behaves like a function scope.
                            // Modifications to `solution` (pointer) are persisted.
                        } else {
                             auto macro_it = state.macros.find(command);
                             if (macro_it != state.macros.end()) {
                                 // Execute macro (in current scope)
                                 // Arguments are replaced textually in real CMake, but here we can try binding variables
                                 // Map arguments
                                 std::map<std::string, std::string> old_vars;
                                 for (size_t k = 0; k < macro_it->second.params.size() && k < args.size(); ++k) {
                                     std::string param = macro_it->second.params[k];
                                     if (state.variables.count(param)) old_vars[param] = state.variables[param];
                                     state.variables[param] = args[k];
                                 }
                                 
                                 // ARGN
                                 std::string argn;
                                 for (size_t k = macro_it->second.params.size(); k < args.size(); ++k) {
                                     if (!argn.empty()) argn += ";";
                                     argn += args[k];
                                 }
                                 if (state.variables.count("ARGN")) old_vars["ARGN"] = state.variables["ARGN"];
                                 state.variables["ARGN"] = argn;

                                 size_t macro_i = 0;
                                 execute_tokens(macro_it->second.body, macro_i, state);
                                 
                                 // Restore variables? Macros typically overwrite.
                                 // Real macros don't have scope, so variables persist.
                             }
                        }
                    }
                }
            }
        } else {
            i++;
        }
    }
}

void CMakeParser::handle_message(const std::vector<std::string>& args, ParseState& /*state*/) {
    // Join args
    std::string msg;
    for (const auto& arg : args) {
        msg += arg;
    }
    std::cout << "[CMake] " << msg << "\n";
}

void CMakeParser::handle_function_def(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state) {
    if (args.empty()) return;
    std::string func_name = args[0];
    
    // Case-insensitive name
    std::transform(func_name.begin(), func_name.end(), func_name.begin(), 
                  [](unsigned char c) { return (char)std::tolower(c); });

    FunctionDef def;
    // Store params
    for (size_t k = 1; k < args.size(); ++k) {
        def.params.push_back(args[k]);
    }
    
    // Capture body until endfunction()
    int nesting = 1;
    while (i < tokens.size()) {
        if (tokens[i].type == TokenType::Identifier) {
            std::string cmd = tokens[i].value;
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            
            if (cmd == "function") nesting++;
            else if (cmd == "endfunction") nesting--;
            
            if (nesting == 0) {
                // Skip the endfunction(...) part
                i++; // Identifier
                if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                     i++;
                     while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) i++;
                     if (i < tokens.size()) i++; // CloseParen
                }
                break; 
            }
        }
        def.body.push_back(tokens[i]);
        i++;
    }
    
    state.functions[func_name] = def;
}

void CMakeParser::handle_macro_def(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state) {
    if (args.empty()) return;
    std::string macro_name = args[0];
    std::transform(macro_name.begin(), macro_name.end(), macro_name.begin(), 
                  [](unsigned char c) { return (char)std::tolower(c); });

    FunctionDef def;
    for (size_t k = 1; k < args.size(); ++k) {
        def.params.push_back(args[k]);
    }
    
    int nesting = 1;
    while (i < tokens.size()) {
        if (tokens[i].type == TokenType::Identifier) {
            std::string cmd = tokens[i].value;
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            
            if (cmd == "macro") nesting++;
            else if (cmd == "endmacro") nesting--;
            
            if (nesting == 0) {
                // Skip endmacro(...)
                i++; 
                if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                     i++;
                     while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) i++;
                     if (i < tokens.size()) i++; 
                }
                break; 
            }
        }
        def.body.push_back(tokens[i]);
        i++;
    }
    
    state.macros[macro_name] = def;
}

void CMakeParser::handle_if(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state) {
    // Structure to hold blocks: condition args -> body tokens
    struct IfBlock {
        std::vector<std::string> condition_args; // empty for 'else'
        std::vector<Token> body;
    };
    
    std::vector<IfBlock> blocks;
    blocks.push_back({args, {}}); // Initial 'if' block
    
    int nesting = 1;
    while (i < tokens.size()) {
        Token t = tokens[i];
        
        if (t.type == TokenType::Identifier) {
            std::string cmd = t.value;
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            
            if (cmd == "if") {
                nesting++;
            } else if (cmd == "endif") {
                nesting--;
                if (nesting == 0) {
                     // End of entire if structure
                     i++; // Skip 'endif'
                     if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                         i++;
                         while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) i++;
                         if (i < tokens.size()) i++; 
                     }
                     break; 
                }
            } else if (nesting == 1) {
                if (cmd == "else") {
                     // Start 'else' block
                     blocks.push_back({{}, {}});
                     
                     // Skip 'else(...)'
                     i++;
                     if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                         i++;
                         while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) i++;
                         if (i < tokens.size()) i++; 
                     }
                     continue;
                } else if (cmd == "elseif") {
                     // Start 'elseif' block
                     i++; // Skip 'elseif'
                     std::vector<std::string> elseif_args;
                     if (i < tokens.size() && tokens[i].type == TokenType::OpenParen) {
                         i++;
                         while (i < tokens.size() && tokens[i].type != TokenType::CloseParen) {
                            if (tokens[i].type == TokenType::String || tokens[i].type == TokenType::Identifier) {
                                elseif_args.push_back(resolve_variables(tokens[i].value, state));
                            }
                            i++;
                         }
                         if (i < tokens.size()) i++;
                     }
                     
                     blocks.push_back({elseif_args, {}});
                     continue;
                }
            }
        }
        
        // Add token to current block
        blocks.back().body.push_back(t);
        i++;
    }
    
    // Execute first matching block
    for (const auto& block : blocks) {
        bool match = false;
        if (block.condition_args.empty()) {
            // 'else' block always matches if reached
            match = true;
        } else {
            match = evaluate_condition(block.condition_args, state);
        }
        
        if (match) {
            size_t block_i = 0;
            execute_tokens(block.body, block_i, state);
            return; // Only execute one block
        }
    }
}

void CMakeParser::handle_while(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state) {
    // Capture body
    // size_t body_start = i;
    std::vector<Token> body;
    int nesting = 1;
    
    // Scan to find end of while loop
    size_t scan = i;
    while (scan < tokens.size()) {
        if (tokens[scan].type == TokenType::Identifier) {
             std::string cmd = tokens[scan].value;
             std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
             if (cmd == "while") nesting++;
             else if (cmd == "endwhile") nesting--;
             
             if (nesting == 0) {
                 // Found end
                 break;
             }
        }
        body.push_back(tokens[scan]);
        scan++;
    }
    
    // Skip past endwhile(...)
    scan++; 
    if (scan < tokens.size() && tokens[scan].type == TokenType::OpenParen) {
         scan++;
         while (scan < tokens.size() && tokens[scan].type != TokenType::CloseParen) scan++;
         if (scan < tokens.size()) scan++; 
    }
    
    size_t end_i = scan;

    // Loop
    while (evaluate_condition(args, state)) {
        size_t loop_i = 0;
        execute_tokens(body, loop_i, state);
    }
    
    i = end_i;
}

bool CMakeParser::evaluate_condition(const std::vector<std::string>& args, ParseState& state) {
    if (args.empty()) return false;
    
    // Basic condition evaluation
    std::string lhs = args[0];
    
    if (lhs == "TRUE" || lhs == "1" || lhs == "ON" || lhs == "YES" || lhs == "Y") return true;
    if (lhs == "FALSE" || lhs == "0" || lhs == "OFF" || lhs == "NO" || lhs == "N" || lhs == "IGNORE") return false;
    
    // Variable reference check (is it defined/true?)
    if (state.variables.count(lhs)) {
        std::string val = state.variables[lhs];
         if (val == "TRUE" || val == "1" || val == "ON" || val == "YES" || val == "Y") return true;
         // Non-empty string usually true except "FALSE" etc.
         if (val != "FALSE" && val != "0" && val != "OFF" && val != "NO" && !val.empty()) return true;
         return false;
    }
    
    // Binary ops
    if (args.size() >= 3) {
        std::string op = args[1];
        std::string rhs = args[2];
        
        if (op == "STREQUAL") return lhs == rhs;
        if (op == "NOT") return !evaluate_condition({args.begin()+1, args.end()}, state); // Recurse? NOT is unary.
    }
    
    if (lhs == "NOT" && args.size() > 1) {
         return !evaluate_condition({args.begin()+1, args.end()}, state);
    }
    
    return !lhs.empty();
}

void CMakeParser::propagate_include_directories(Solution& solution) {
    // For each project, propagate include directories from its dependencies
    for (auto& proj : solution.projects) {
        // Now propagate from dependencies (recursively)
        std::vector<std::string> to_process;
        std::set<std::string> processed_deps; // Track which dependencies we've visited

        // Initialize with direct dependency names
        for (const auto& dep : proj.project_references) {
            to_process.push_back(dep.name);
        }

        while (!to_process.empty()) {
            std::string dep_name = to_process.back();
            to_process.pop_back();

            // Skip if we've already processed this dependency
            if (processed_deps.count(dep_name)) continue;
            processed_deps.insert(dep_name);

            // Find the dependency project
            Project* dep_proj = nullptr;
            for (auto& p : solution.projects) {
                if (p.name == dep_name) {
                    dep_proj = &p;
                    break;
                }
            }

            if (!dep_proj) continue;

            // Copy include directories from dependency to current project
            // Process EACH config separately to avoid cross-config pollution
            for (const auto& config_key : solution.get_config_keys()) {
                // Create per-config set for deduplication
                std::set<std::string> processed_includes;

                // Mark project's OWN includes for THIS config
                auto& proj_includes = proj.configurations[config_key].cl_compile.additional_include_directories;
                for (const auto& inc : proj_includes) {
                    processed_includes.insert(inc);
                }

                // Add dependency includes
                auto& dep_includes = dep_proj->configurations[config_key].cl_compile.additional_include_directories;
                for (const auto& inc : dep_includes) {
                    // Only add if not already present in THIS config
                    if (processed_includes.count(inc) == 0) {
                        proj_includes.push_back(inc);
                        processed_includes.insert(inc);
                    }
                }
            }

            // Add transitive dependencies (dependencies of dependencies)
            for (const auto& transitive_dep : dep_proj->project_references) {
                if (processed_deps.count(transitive_dep.name) == 0) {
                    to_process.push_back(transitive_dep.name);
                }
            }
        }
    }
}

} // namespace vcxproj
