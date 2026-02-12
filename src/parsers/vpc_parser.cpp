#include "pch.h"
#include "vpc_parser.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace fs = std::filesystem;

namespace vcxproj {

// ============================================================================
// Helper Functions
// ============================================================================

std::string VpcParser::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::string VpcParser::to_upper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string VpcParser::normalize_path(const std::string& path) {
    std::string result = path;
    // Convert backslashes to forward slashes
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string VpcParser::resolve_path(const std::string& path, const std::string& base_path) {
    std::string normalized = normalize_path(path);

    // If already absolute, return as-is
    fs::path p(normalized);
    if (p.is_absolute()) {
        return normalized;
    }

    // Resolve relative to base path
    fs::path base(base_path);
    fs::path resolved = base / p;

    try {
        // Normalize the path (resolve . and ..)
        resolved = fs::weakly_canonical(resolved);
    } catch (...) {
        // If weakly_canonical fails, just use the combined path
    }

    return normalize_path(resolved.string());
}

void VpcParser::parse_error(const std::string& message, const ParseState& state, int line) {
    int actual_line = (line >= 0) ? line : state.line_number;
    std::string full_message = state.current_file + "(" + std::to_string(actual_line) + "): error: " + message;
    throw std::runtime_error(full_message);
}

void VpcParser::parse_warning(const std::string& message, const ParseState& state, int line) {
    int actual_line = (line >= 0) ? line : state.line_number;
    std::cerr << state.current_file << "(" << actual_line << "): warning: " << message << "\n";
}

// ============================================================================
// Tokenizer
// ============================================================================

std::vector<VpcParser::Token> VpcParser::tokenize(const std::string& content) {
    std::vector<Token> tokens;
    size_t i = 0;
    int line = 1;

    while (i < content.size()) {
        // Skip whitespace (but track newlines for line counting)
        while (i < content.size() && (content[i] == ' ' || content[i] == '\t' || content[i] == '\r')) {
            i++;
        }

        if (i >= content.size()) break;

        // Newline
        if (content[i] == '\n') {
            line++;
            i++;
            continue;
        }

        // Line continuation (backslash at end of line)
        if (content[i] == '\\') {
            // Check if followed by newline (possibly with spaces/tabs)
            size_t j = i + 1;
            while (j < content.size() && (content[j] == ' ' || content[j] == '\t' || content[j] == '\r')) {
                j++;
            }
            if (j < content.size() && content[j] == '\n') {
                // Skip the continuation
                i = j + 1;
                line++;
                continue;
            }
        }

        // Comment (// to end of line)
        if (i + 1 < content.size() && content[i] == '/' && content[i + 1] == '/') {
            // Skip to end of line
            while (i < content.size() && content[i] != '\n') {
                i++;
            }
            continue;
        }

        // Block comment (/* ... */)
        if (i + 1 < content.size() && content[i] == '/' && content[i + 1] == '*') {
            i += 2;
            while (i + 1 < content.size() && !(content[i] == '*' && content[i + 1] == '/')) {
                if (content[i] == '\n') line++;
                i++;
            }
            if (i + 1 < content.size()) i += 2;  // Skip */
            continue;
        }

        // Keyword ($...)
        if (content[i] == '$') {
            Token tok;
            tok.type = TokenType::Keyword;
            tok.line = line;
            i++;  // Skip $
            size_t start = i;
            while (i < content.size() && (std::isalnum(content[i]) || content[i] == '_')) {
                i++;
            }
            tok.value = "$" + content.substr(start, i - start);
            tokens.push_back(tok);
            continue;
        }

        // Quoted string
        if (content[i] == '"') {
            Token tok;
            tok.type = TokenType::String;
            tok.line = line;
            i++;  // Skip opening quote
            std::string value;
            while (i < content.size() && content[i] != '"') {
                // Handle escape sequences
                if (content[i] == '\\' && i + 1 < content.size()) {
                    char next = content[i + 1];
                    if (next == '"' || next == '\\') {
                        value += next;
                        i += 2;
                        continue;
                    } else if (next == 'n') {
                        value += '\n';
                        i += 2;
                        continue;
                    } else if (next == '\n') {
                        // Line continuation inside string
                        i += 2;
                        line++;
                        continue;
                    } else if (next == '\r') {
                        // Handle \r\n
                        i += 2;
                        if (i < content.size() && content[i] == '\n') {
                            i++;
                            line++;
                        }
                        continue;
                    }
                }
                if (content[i] == '\n') {
                    line++;
                }
                value += content[i];
                i++;
            }
            if (i < content.size()) i++;  // Skip closing quote
            tok.value = value;
            tokens.push_back(tok);
            continue;
        }

        // Condition [...]
        if (content[i] == '[') {
            Token tok;
            tok.type = TokenType::Condition;
            tok.line = line;
            i++;  // Skip [
            std::string value;
            int depth = 1;
            while (i < content.size() && depth > 0) {
                if (content[i] == '[') depth++;
                else if (content[i] == ']') depth--;
                if (depth > 0) {
                    value += content[i];
                }
                i++;
            }
            tok.value = value;
            tokens.push_back(tok);
            continue;
        }

        // Braces
        if (content[i] == '{') {
            Token tok;
            tok.type = TokenType::OpenBrace;
            tok.line = line;
            tok.value = "{";
            tokens.push_back(tok);
            i++;
            continue;
        }

        if (content[i] == '}') {
            Token tok;
            tok.type = TokenType::CloseBrace;
            tok.line = line;
            tok.value = "}";
            tokens.push_back(tok);
            i++;
            continue;
        }

        // Unquoted identifier/value
        if (std::isalnum(content[i]) || content[i] == '_' || content[i] == '-' ||
            content[i] == '.' || content[i] == '/' || content[i] == ':') {
            Token tok;
            tok.type = TokenType::Identifier;
            tok.line = line;
            size_t start = i;
            while (i < content.size() &&
                   (std::isalnum(content[i]) || content[i] == '_' || content[i] == '-' ||
                    content[i] == '.' || content[i] == '/' || content[i] == ':' ||
                    content[i] == '\\')) {
                i++;
            }
            tok.value = content.substr(start, i - start);
            tokens.push_back(tok);
            continue;
        }

        // Skip any other character
        i++;
    }

    Token eof;
    eof.type = TokenType::EndOfFile;
    eof.line = line;
    tokens.push_back(eof);

    return tokens;
}

// ============================================================================
// Macro Resolution
// ============================================================================

std::string VpcParser::resolve_macros(const std::string& str, const ParseState& state) {
    std::string result = str;

    // Keep resolving until no more changes (handles nested macros)
    bool changed;
    int iterations = 0;
    const int max_iterations = 100;  // Prevent infinite loops

    do {
        changed = false;
        std::string new_result;
        size_t i = 0;

        while (i < result.size()) {
            if (result[i] == '$' && i + 1 < result.size() &&
                (std::isalpha(result[i + 1]) || result[i + 1] == '_')) {
                // Found potential macro reference
                size_t start = i + 1;
                size_t end = start;
                while (end < result.size() &&
                       (std::isalnum(result[end]) || result[end] == '_')) {
                    end++;
                }
                std::string macro_name = result.substr(start, end - start);
                std::string macro_upper = to_upper(macro_name);

                auto it = state.macros.find(macro_upper);
                if (it != state.macros.end()) {
                    new_result += it->second;
                    changed = true;
                } else {
                    // Keep the unresolved macro reference
                    new_result += result.substr(i, end - i);
                }
                i = end;
            } else {
                new_result += result[i];
                i++;
            }
        }
        result = new_result;
        iterations++;
    } while (changed && iterations < max_iterations);

    return result;
}

// ============================================================================
// Conditional Evaluation
// ============================================================================

void VpcParser::init_conditionals(ParseState& state) {
    // Initialize platform conditionals based on target platforms
    // Default all to false, then enable based on targets
    state.conditionals["WIN32"] = false;
    state.conditionals["WIN64"] = false;
    state.conditionals["WINDOWS"] = false;
    state.conditionals["LINUX32"] = false;
    state.conditionals["LINUX64"] = false;
    state.conditionals["LINUXALL"] = false;
    state.conditionals["OSX32"] = false;
    state.conditionals["OSX64"] = false;
    state.conditionals["OSXALL"] = false;
    state.conditionals["POSIX"] = false;

    for (const auto& platform : m_target_platforms) {
        std::string upper = to_upper(platform);
        state.conditionals[upper] = true;

        // Set compound conditionals
        if (upper == "WIN32" || upper == "WIN64") {
            state.conditionals["WINDOWS"] = true;
        }
        if (upper == "LINUX32" || upper == "LINUX64") {
            state.conditionals["LINUXALL"] = true;
            state.conditionals["POSIX"] = true;
        }
        if (upper == "OSX32" || upper == "OSX64") {
            state.conditionals["OSXALL"] = true;
            state.conditionals["POSIX"] = true;
        }
    }
}

bool VpcParser::evaluate_condition(const std::string& condition, const ParseState& state) {
    std::string expr = trim(condition);
    if (expr.empty()) return true;

    return parse_condition_expr(expr, state);
}

bool VpcParser::parse_condition_expr(const std::string& expr, const ParseState& state) {
    std::string trimmed = trim(expr);
    if (trimmed.empty()) return true;

    // Handle || (lowest precedence) - find first || not inside parens
    int paren_depth = 0;
    for (size_t i = 0; i + 1 < trimmed.size(); i++) {
        if (trimmed[i] == '(') paren_depth++;
        else if (trimmed[i] == ')') paren_depth--;
        else if (paren_depth == 0 && trimmed[i] == '|' && trimmed[i + 1] == '|') {
            return parse_condition_expr(trimmed.substr(0, i), state) ||
                   parse_condition_expr(trimmed.substr(i + 2), state);
        }
    }

    // Handle && (higher precedence)
    paren_depth = 0;
    for (size_t i = 0; i + 1 < trimmed.size(); i++) {
        if (trimmed[i] == '(') paren_depth++;
        else if (trimmed[i] == ')') paren_depth--;
        else if (paren_depth == 0 && trimmed[i] == '&' && trimmed[i + 1] == '&') {
            return parse_condition_expr(trimmed.substr(0, i), state) &&
                   parse_condition_expr(trimmed.substr(i + 2), state);
        }
    }

    // Handle parentheses
    if (trimmed.front() == '(' && trimmed.back() == ')') {
        return parse_condition_expr(trimmed.substr(1, trimmed.size() - 2), state);
    }

    // Handle ! (negation)
    if (trimmed.front() == '!') {
        return !parse_condition_expr(trimmed.substr(1), state);
    }

    // Handle $CONDITIONAL reference
    std::string name = trimmed;
    if (name.front() == '$') {
        name = name.substr(1);
    }
    name = to_upper(name);

    auto it = state.conditionals.find(name);
    if (it != state.conditionals.end()) {
        return it->second;
    }

    // Unknown conditional - default to false
    return false;
}

std::string VpcParser::check_condition(std::vector<Token>& tokens, size_t i) {
    if (i < tokens.size() && tokens[i].type == TokenType::Condition) {
        return tokens[i].value;
    }
    return "";
}

std::string VpcParser::get_next_value(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    while (i < tokens.size()) {
        const Token& tok = tokens[i];
        if (tok.type == TokenType::String || tok.type == TokenType::Identifier) {
            std::string value = tok.value;
            i++;
            return resolve_macros(value, state);
        } else if (tok.type == TokenType::Condition) {
            // Skip conditions when getting values
            i++;
            continue;
        } else {
            break;
        }
    }
    return "";
}

std::string VpcParser::map_platform_conditional(const std::string& conditional) {
    std::string upper = to_upper(conditional);

    if (upper == "WIN32") return "Win32";
    if (upper == "WIN64" || upper == "X64") return "x64";
    if (upper == "WINDOWS") return "";  // Applies to both Win32 and x64
    if (upper == "LINUX32") return "Linux32";
    if (upper == "LINUX64") return "Linux64";
    if (upper == "LINUXALL") return "Linux";
    if (upper == "OSX32") return "OSX32";
    if (upper == "OSX64") return "OSX64";
    if (upper == "OSXALL") return "OSX";

    return "";  // Unknown - applies to all
}

std::vector<std::string> VpcParser::extract_platforms_from_condition(const std::string& condition) {
    std::vector<std::string> platforms;

    // Simple extraction - look for platform names in the condition
    std::string upper = to_upper(condition);

    // Check for negation - if condition is negated, we can't easily extract platforms
    if (upper.find('!') != std::string::npos) {
        // For negated conditions, return empty (applies based on evaluation)
        return platforms;
    }

    if (upper.find("WIN32") != std::string::npos) platforms.push_back("Win32");
    if (upper.find("WIN64") != std::string::npos) platforms.push_back("x64");
    if (upper.find("LINUX64") != std::string::npos) platforms.push_back("Linux64");
    if (upper.find("LINUX32") != std::string::npos) platforms.push_back("Linux32");
    if (upper.find("OSX64") != std::string::npos) platforms.push_back("OSX64");
    if (upper.find("OSX32") != std::string::npos) platforms.push_back("OSX32");

    // Compound conditionals
    if (upper.find("WINDOWS") != std::string::npos && platforms.empty()) {
        platforms.push_back("Win32");
        platforms.push_back("x64");
    }
    if (upper.find("LINUXALL") != std::string::npos && platforms.empty()) {
        platforms.push_back("Linux32");
        platforms.push_back("Linux64");
    }
    if (upper.find("OSXALL") != std::string::npos && platforms.empty()) {
        platforms.push_back("OSX32");
        platforms.push_back("OSX64");
    }

    return platforms;
}

void VpcParser::skip_block(std::vector<Token>& tokens, size_t& i) {
    int depth = 1;
    while (i < tokens.size() && depth > 0) {
        if (tokens[i].type == TokenType::OpenBrace) depth++;
        else if (tokens[i].type == TokenType::CloseBrace) depth--;
        i++;
    }
}

// ============================================================================
// Include Processing
// ============================================================================

void VpcParser::process_include(const std::string& path, ParseState& state) {
    std::string resolved = resolve_macros(path, state);
    std::string abs_path = resolve_path(resolved, state.base_path);

    // Check for circular includes
    for (const auto& inc : state.include_stack) {
        if (inc == abs_path) {
            parse_warning("Circular include detected: " + abs_path, state);
            return;
        }
    }

    // Check if file exists
    if (!fs::exists(abs_path)) {
        parse_warning("Include file not found: " + abs_path, state);
        return;
    }

    // Read file content
    std::ifstream file(abs_path);
    if (!file.is_open()) {
        parse_warning("Cannot open include file: " + abs_path, state);
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Save current state
    std::string old_base = state.base_path;
    std::string old_file = state.current_file;
    int old_line = state.line_number;

    // Update state for included file
    state.base_path = fs::path(abs_path).parent_path().string();
    state.current_file = abs_path;
    state.line_number = 1;
    state.include_stack.push_back(abs_path);

    // Tokenize and parse
    auto tokens = tokenize(content);
    size_t idx = 0;
    parse_tokens(tokens, idx, state);

    // Restore state
    state.include_stack.pop_back();
    state.base_path = old_base;
    state.current_file = old_file;
    state.line_number = old_line;
}

// ============================================================================
// Keyword Handlers
// ============================================================================

void VpcParser::handle_macro(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Macro NAME "value" [condition]
    i++;  // Skip $Macro

    std::string name = get_next_value(tokens, i, state);
    if (name.empty()) {
        parse_error("Expected macro name after $Macro", state, tokens[i - 1].line);
        return;
    }

    std::string value = get_next_value(tokens, i, state);

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            return;  // Condition not met, skip this macro
        }
    }

    // Store macro (uppercase for case-insensitive lookup)
    state.macros[to_upper(name)] = value;
}

void VpcParser::handle_macro_required(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $MacroRequired NAME or $MacroRequiredAllowEmpty NAME
    i++;  // Skip keyword

    std::string name = get_next_value(tokens, i, state);
    // Just skip - we don't enforce required macros
}

void VpcParser::handle_conditional(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Conditional NAME value
    i++;  // Skip $Conditional

    std::string name = get_next_value(tokens, i, state);
    if (name.empty()) {
        return;
    }

    // Value can be a string or identifier
    std::string value = get_next_value(tokens, i, state);

    // Check for condition on the $Conditional itself
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            return;
        }
    }

    // Set the conditional
    bool is_true = !value.empty() && value != "0" && to_upper(value) != "FALSE";
    state.conditionals[to_upper(name)] = is_true;
}

void VpcParser::handle_include(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Include "path" [condition]
    i++;  // Skip $Include

    std::string path = get_next_value(tokens, i, state);
    if (path.empty()) {
        return;
    }

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            return;  // Condition not met, skip include
        }
    }

    process_include(path, state);
}

void VpcParser::handle_project(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Project "name" { ... }
    i++;  // Skip $Project

    std::string name = get_next_value(tokens, i, state);
    if (name.empty()) {
        parse_error("Expected project name after $Project", state, tokens[i - 1].line);
        return;
    }

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            // Skip the project block
            if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                i++;
                skip_block(tokens, i);
            }
            return;
        }
    }

    // Create or find project
    Project* proj = nullptr;
    for (auto& p : state.solution.projects) {
        if (p.name == name) {
            proj = &p;
            break;
        }
    }

    if (!proj) {
        state.solution.projects.emplace_back();
        proj = &state.solution.projects.back();
        proj->name = name;
        proj->uuid = generate_uuid();
    }

    state.current_project = proj;

    // Parse project block
    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {
        parse_block(tokens, i, state, "Project");
    }

    state.current_project = nullptr;
}

void VpcParser::handle_folder(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Folder "name" { ... }
    i++;  // Skip $Folder

    std::string name = get_next_value(tokens, i, state);

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                i++;
                skip_block(tokens, i);
            }
            return;
        }
    }

    state.folder_stack.push_back(name);

    // Parse folder block
    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {
        parse_block(tokens, i, state, "Folder");
    }

    state.folder_stack.pop_back();
}

void VpcParser::handle_file(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $File "path" [condition]
    // Also handle -$File for removal (we just skip it)
    i++;  // Skip $File

    std::string path = get_next_value(tokens, i, state);
    if (path.empty()) {
        return;
    }

    // Check for condition
    std::string condition = check_condition(tokens, i);
    std::vector<std::string> platforms;
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            return;  // Condition not met
        }
        platforms = extract_platforms_from_condition(condition);
    }

    if (!state.current_project) {
        return;  // No active project
    }

    // Normalize and resolve path
    std::string resolved_path = resolve_path(path, state.base_path);

    // Add to project sources
    SourceFile sf;
    sf.path = resolved_path;
    sf.type = get_file_type(resolved_path);

    // If platform-specific, mark in settings
    if (!platforms.empty()) {
        for (const auto& plat : platforms) {
            // Mark as not excluded for this platform (excluded for others)
            // This is handled at generation time
        }
    }

    state.current_project->sources.push_back(sf);

    // Check for file-specific block
    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;
        skip_block(tokens, i);  // For now, skip file-specific settings
    }
}

void VpcParser::handle_lib(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Lib "path" [condition]
    // Also $ImpLib, $LibExternal
    i++;  // Skip keyword

    std::string path = get_next_value(tokens, i, state);
    if (path.empty()) {
        return;
    }

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            return;
        }
    }

    if (!state.current_project) {
        return;
    }

    // Resolve path and add .lib extension if missing
    std::string resolved = resolve_path(path, state.base_path);
    if (fs::path(resolved).extension().empty()) {
        resolved += ".lib";
    }

    LibraryFile lf;
    lf.path = resolved;
    state.current_project->libraries.push_back(lf);
}

void VpcParser::handle_configuration(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Configuration ["name"] { ... }
    i++;  // Skip $Configuration

    std::string config_name;

    // Check if there's a configuration name
    if (i < tokens.size() && (tokens[i].type == TokenType::String ||
                               tokens[i].type == TokenType::Identifier)) {
        config_name = get_next_value(tokens, i, state);
    }

    // Check for condition
    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                i++;
                skip_block(tokens, i);
            }
            return;
        }
    }

    state.current_config = config_name;

    // Parse configuration block
    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {
        parse_block(tokens, i, state, "Configuration");
    }

    state.current_config = "";
}

void VpcParser::handle_general(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $General { ... }
    i++;  // Skip $General

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {

        while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
            if (tokens[i].type == TokenType::Keyword) {
                std::string keyword = tokens[i].value;
                i++;

                std::string value = get_next_value(tokens, i, state);

                // Check for condition
                std::string condition = check_condition(tokens, i);
                if (!condition.empty()) {
                    i++;
                    if (!evaluate_condition(condition, state)) {
                        continue;
                    }
                }

                // Handle general settings
                // Most general settings are solution-level, not commonly needed
            } else {
                i++;
            }
        }

        if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
            i++;
        }
    }
}

void VpcParser::handle_compiler(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Compiler { ... }
    i++;  // Skip $Compiler

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {

        while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
            if (tokens[i].type == TokenType::Keyword) {
                std::string keyword = to_upper(tokens[i].value);
                i++;

                std::string value = get_next_value(tokens, i, state);

                // Check for condition
                std::string condition = check_condition(tokens, i);
                std::vector<std::string> platforms;
                if (!condition.empty()) {
                    i++;
                    if (!evaluate_condition(condition, state)) {
                        continue;
                    }
                    platforms = extract_platforms_from_condition(condition);
                }

                // Determine which configurations to apply to
                std::vector<std::string> configs;
                if (state.current_config.empty()) {
                    configs = {"Debug", "Release"};
                } else {
                    configs = {state.current_config};
                }

                if (platforms.empty()) {
                    platforms = {"Win32", "x64"};
                }

                // Apply to all config/platform combinations
                for (const auto& config : configs) {
                    for (const auto& platform : platforms) {
                        std::string key = config + "|" + platform;

                        if (state.current_project) {
                            auto& cfg = state.current_project->configurations[key];

                            // Handle $BASE - append to existing
                            bool append = (value.find("$BASE") != std::string::npos);
                            std::string clean_value = value;
                            size_t base_pos = clean_value.find("$BASE");
                            if (base_pos != std::string::npos) {
                                clean_value = trim(clean_value.substr(0, base_pos) +
                                                  clean_value.substr(base_pos + 5));
                            }

                            if (keyword == "$PREPROCESSORDEFINITIONS") {
                                // Split by semicolon
                                std::stringstream ss(clean_value);
                                std::string item;
                                while (std::getline(ss, item, ';')) {
                                    item = trim(item);
                                    if (!item.empty()) {
                                        cfg.cl_compile.preprocessor_definitions.push_back(item);
                                    }
                                }
                            } else if (keyword == "$ADDITIONALINCLUDEDIRECTORIES") {
                                std::stringstream ss(clean_value);
                                std::string item;
                                while (std::getline(ss, item, ';')) {
                                    item = trim(item);
                                    if (!item.empty()) {
                                        std::string resolved = resolve_path(item, state.base_path);
                                        cfg.cl_compile.additional_include_directories.push_back(resolved);
                                    }
                                }
                            } else if (keyword == "$OPTIMIZATION") {
                                cfg.cl_compile.optimization = clean_value;
                            } else if (keyword == "$WARNINGLEVEL") {
                                cfg.cl_compile.warning_level = clean_value;
                            } else if (keyword == "$RUNTIMELIBRARY") {
                                cfg.cl_compile.runtime_library = clean_value;
                            } else if (keyword == "$DEBUGINFORMATIONFORMAT") {
                                cfg.cl_compile.debug_information_format = clean_value;
                            } else if (keyword == "$DISABLESPECIFICWARNINGS") {
                                std::stringstream ss(clean_value);
                                std::string item;
                                while (std::getline(ss, item, ';')) {
                                    item = trim(item);
                                    if (!item.empty()) {
                                        cfg.cl_compile.disable_specific_warnings.push_back(item);
                                    }
                                }
                            } else if (keyword == "$ADDITIONALOPTIONS") {
                                if (append && !cfg.cl_compile.additional_options.empty()) {
                                    cfg.cl_compile.additional_options += " " + clean_value;
                                } else {
                                    cfg.cl_compile.additional_options = clean_value;
                                }
                            } else if (keyword == "$EXCEPTIONHANDLING") {
                                cfg.cl_compile.exception_handling = clean_value;
                            } else if (keyword == "$PRECOMPILEDHEADER") {
                                cfg.cl_compile.pch.mode = clean_value;
                            } else if (keyword == "$PRECOMPILEDHEADERFILE") {
                                cfg.cl_compile.pch.header = clean_value;
                            }
                        }
                    }
                }
            } else {
                i++;
            }
        }

        if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
            i++;
        }
    }
}

void VpcParser::handle_linker(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Linker { ... }
    i++;  // Skip $Linker

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;  // Skip {

        while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
            if (tokens[i].type == TokenType::Keyword) {
                std::string keyword = to_upper(tokens[i].value);
                i++;

                std::string value = get_next_value(tokens, i, state);

                // Check for condition
                std::string condition = check_condition(tokens, i);
                std::vector<std::string> platforms;
                if (!condition.empty()) {
                    i++;
                    if (!evaluate_condition(condition, state)) {
                        continue;
                    }
                    platforms = extract_platforms_from_condition(condition);
                }

                std::vector<std::string> configs;
                if (state.current_config.empty()) {
                    configs = {"Debug", "Release"};
                } else {
                    configs = {state.current_config};
                }

                if (platforms.empty()) {
                    platforms = {"Win32", "x64"};
                }

                for (const auto& config : configs) {
                    for (const auto& platform : platforms) {
                        std::string key = config + "|" + platform;

                        if (state.current_project) {
                            auto& cfg = state.current_project->configurations[key];

                            // Handle $BASE
                            std::string clean_value = value;
                            size_t base_pos = clean_value.find("$BASE");
                            if (base_pos != std::string::npos) {
                                clean_value = trim(clean_value.substr(0, base_pos) +
                                                  clean_value.substr(base_pos + 5));
                            }

                            if (keyword == "$ADDITIONALDEPENDENCIES") {
                                // Split by space or semicolon
                                std::regex sep_re("[;\\s]+");
                                std::sregex_token_iterator iter(clean_value.begin(), clean_value.end(), sep_re, -1);
                                std::sregex_token_iterator end;
                                for (; iter != end; ++iter) {
                                    std::string lib = trim(*iter);
                                    if (!lib.empty()) {
                                        cfg.link.additional_dependencies.push_back(lib);
                                    }
                                }
                            } else if (keyword == "$ADDITIONALLIBRARYDIRECTORIES") {
                                std::stringstream ss(clean_value);
                                std::string item;
                                while (std::getline(ss, item, ';')) {
                                    item = trim(item);
                                    if (!item.empty()) {
                                        std::string resolved = resolve_path(item, state.base_path);
                                        cfg.link.additional_library_directories.push_back(resolved);
                                    }
                                }
                            } else if (keyword == "$OUTPUTFILE") {
                                cfg.link.output_file = clean_value;
                            } else if (keyword == "$SUBSYSTEM") {
                                cfg.link.sub_system = clean_value;
                            } else if (keyword == "$GENERATEDEBUGINFO" || keyword == "$GENERATEDEBUGINFORMATION") {
                                cfg.link.generate_debug_info = (to_upper(clean_value) == "TRUE" ||
                                                                to_upper(clean_value) == "YES" ||
                                                                clean_value == "1");
                            } else if (keyword == "$ADDITIONALOPTIONS") {
                                cfg.link.additional_options = clean_value;
                            } else if (keyword == "$STACKRESERVESIZE") {
                                // Stack reserve - store in additional options
                                cfg.link.additional_options += " /STACK:" + clean_value;
                            }
                        }
                    }
                }
            } else {
                i++;
            }
        }

        if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
            i++;
        }
    }
}

void VpcParser::handle_librarian(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $Librarian { ... }
    i++;  // Skip $Librarian

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;
        skip_block(tokens, i);  // Basic implementation - skip for now
    }
}

void VpcParser::handle_pre_build_event(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $PreBuildEvent [condition] { ... }
    i++;  // Skip $PreBuildEvent

    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                i++;
                skip_block(tokens, i);
            }
            return;
        }
    }

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;

        std::string command;
        std::string message;

        while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
            if (tokens[i].type == TokenType::Keyword) {
                std::string kw = to_upper(tokens[i].value);
                i++;
                std::string val = get_next_value(tokens, i, state);

                if (kw == "$COMMANDLINE") {
                    command = val;
                } else if (kw == "$DESCRIPTION") {
                    message = val;
                }
            } else {
                i++;
            }
        }

        if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
            i++;
        }

        // Apply to configurations
        if (state.current_project && !command.empty()) {
            for (auto& [key, cfg] : state.current_project->configurations) {
                cfg.pre_build_event.command = command;
                cfg.pre_build_event.message = message;
            }
        }
    }
}

void VpcParser::handle_post_build_event(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    // $PostBuildEvent [condition] { ... }
    i++;  // Skip $PostBuildEvent

    std::string condition = check_condition(tokens, i);
    if (!condition.empty()) {
        i++;
        if (!evaluate_condition(condition, state)) {
            if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                i++;
                skip_block(tokens, i);
            }
            return;
        }
    }

    if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
        i++;

        std::string command;
        std::string message;

        while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
            if (tokens[i].type == TokenType::Keyword) {
                std::string kw = to_upper(tokens[i].value);
                i++;
                std::string val = get_next_value(tokens, i, state);

                if (kw == "$COMMANDLINE") {
                    command = val;
                } else if (kw == "$DESCRIPTION") {
                    message = val;
                }
            } else {
                i++;
            }
        }

        if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
            i++;
        }

        if (state.current_project && !command.empty()) {
            for (auto& [key, cfg] : state.current_project->configurations) {
                cfg.post_build_event.command = command;
                cfg.post_build_event.message = message;
            }
        }
    }
}

// ============================================================================
// Main Parsing
// ============================================================================

void VpcParser::parse_block(std::vector<Token>& tokens, size_t& i, ParseState& state,
                            const std::string& block_type) {
    while (i < tokens.size() && tokens[i].type != TokenType::CloseBrace) {
        state.line_number = tokens[i].line;

        if (tokens[i].type == TokenType::Keyword) {
            std::string keyword = to_upper(tokens[i].value);

            if (keyword == "$MACRO") {
                handle_macro(tokens, i, state);
            } else if (keyword == "$MACROREQUIRED" || keyword == "$MACROREQUIREDALLOWEMPTY") {
                handle_macro_required(tokens, i, state);
            } else if (keyword == "$CONDITIONAL") {
                handle_conditional(tokens, i, state);
            } else if (keyword == "$INCLUDE") {
                handle_include(tokens, i, state);
            } else if (keyword == "$CONFIGURATION") {
                handle_configuration(tokens, i, state);
            } else if (keyword == "$PROJECT") {
                handle_project(tokens, i, state);
            } else if (keyword == "$FOLDER") {
                handle_folder(tokens, i, state);
            } else if (keyword == "$FILE" || keyword == "$DYNAMICFILE" || keyword == "$SCHEMAFILE") {
                handle_file(tokens, i, state);
            } else if (keyword == "-$FILE") {
                // Remove file - skip
                i++;
                get_next_value(tokens, i, state);
                check_condition(tokens, i);
                if (!check_condition(tokens, i).empty()) i++;
            } else if (keyword == "$LIB" || keyword == "$IMPLIB" || keyword == "$LIBEXTERNAL") {
                handle_lib(tokens, i, state);
            } else if (keyword == "$GENERAL") {
                handle_general(tokens, i, state);
            } else if (keyword == "$COMPILER") {
                handle_compiler(tokens, i, state);
            } else if (keyword == "$LINKER") {
                handle_linker(tokens, i, state);
            } else if (keyword == "$LIBRARIAN") {
                handle_librarian(tokens, i, state);
            } else if (keyword == "$PREBUILDEVENT") {
                handle_pre_build_event(tokens, i, state);
            } else if (keyword == "$POSTBUILDEVENT") {
                handle_post_build_event(tokens, i, state);
            } else {
                // Unknown keyword - skip it and any following value/block
                i++;
                // Skip any following string/identifier
                while (i < tokens.size() &&
                       (tokens[i].type == TokenType::String ||
                        tokens[i].type == TokenType::Identifier ||
                        tokens[i].type == TokenType::Condition)) {
                    i++;
                }
                // Skip any block
                if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                    i++;
                    skip_block(tokens, i);
                }
            }
        } else if (tokens[i].type == TokenType::Condition) {
            // Standalone condition - skip
            i++;
        } else {
            i++;
        }
    }

    // Skip closing brace
    if (i < tokens.size() && tokens[i].type == TokenType::CloseBrace) {
        i++;
    }
}

void VpcParser::parse_tokens(std::vector<Token>& tokens, size_t& i, ParseState& state) {
    while (i < tokens.size() && tokens[i].type != TokenType::EndOfFile) {
        state.line_number = tokens[i].line;

        if (tokens[i].type == TokenType::Keyword) {
            std::string keyword = to_upper(tokens[i].value);

            if (keyword == "$MACRO") {
                handle_macro(tokens, i, state);
            } else if (keyword == "$MACROREQUIRED" || keyword == "$MACROREQUIREDALLOWEMPTY") {
                handle_macro_required(tokens, i, state);
            } else if (keyword == "$CONDITIONAL") {
                handle_conditional(tokens, i, state);
            } else if (keyword == "$INCLUDE") {
                handle_include(tokens, i, state);
            } else if (keyword == "$CONFIGURATION") {
                handle_configuration(tokens, i, state);
            } else if (keyword == "$PROJECT") {
                handle_project(tokens, i, state);
            } else {
                // Unknown top-level keyword - skip
                i++;
                while (i < tokens.size() &&
                       (tokens[i].type == TokenType::String ||
                        tokens[i].type == TokenType::Identifier ||
                        tokens[i].type == TokenType::Condition)) {
                    i++;
                }
                if (i < tokens.size() && tokens[i].type == TokenType::OpenBrace) {
                    i++;
                    skip_block(tokens, i);
                }
            }
        } else {
            i++;
        }
    }
}

void VpcParser::finalize_solution(ParseState& state) {
    // Set solution name from first project if not set
    if (state.solution.name.empty() && !state.solution.projects.empty()) {
        state.solution.name = state.solution.projects[0].name;
    }

    // Ensure configurations and platforms are set
    if (state.solution.configurations.empty()) {
        state.solution.configurations = {"Debug", "Release"};
    }
    if (state.solution.platforms.empty()) {
        state.solution.platforms = {"Win32", "x64"};
    }

    // Generate UUID if missing
    if (state.solution.uuid.empty()) {
        state.solution.uuid = generate_uuid();
    }

    // Ensure each project has configurations for all solution config/platform combos
    for (auto& proj : state.solution.projects) {
        for (const auto& config : state.solution.configurations) {
            for (const auto& platform : state.solution.platforms) {
                std::string key = config + "|" + platform;
                if (proj.configurations.find(key) == proj.configurations.end()) {
                    proj.configurations[key] = Configuration{};
                }

                auto& cfg = proj.configurations[key];

                // Set defaults if not already set
                if (cfg.config_type.empty()) {
                    cfg.config_type = "Application";  // Default to exe
                }
                if (cfg.platform_toolset.empty()) {
                    cfg.platform_toolset = "v143";  // VS2022
                }

                // Set debug-specific defaults
                if (config == "Debug") {
                    cfg.use_debug_libraries = true;
                    if (cfg.cl_compile.optimization.empty()) {
                        cfg.cl_compile.optimization = "Disabled";
                    }
                    if (cfg.cl_compile.runtime_library.empty()) {
                        cfg.cl_compile.runtime_library = "MultiThreadedDebugDLL";
                    }
                } else {
                    if (cfg.cl_compile.optimization.empty()) {
                        cfg.cl_compile.optimization = "MaxSpeed";
                    }
                    if (cfg.cl_compile.runtime_library.empty()) {
                        cfg.cl_compile.runtime_library = "MultiThreadedDLL";
                    }
                }
            }
        }
    }
}

// ============================================================================
// Public Interface
// ============================================================================

Solution VpcParser::parse(const std::string& filepath) {
    // Read file content
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open VPC file: " + filepath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    std::string base_path = fs::path(filepath).parent_path().string();
    if (base_path.empty()) {
        base_path = ".";
    }

    return parse_string(content, base_path);
}

Solution VpcParser::parse_string(const std::string& content, const std::string& base_path) {
    ParseState state;
    state.base_path = base_path;
    state.current_file = base_path + "/input.vpc";
    state.line_number = 1;

    // Initialize default conditionals
    init_conditionals(state);

    // Initialize default macros
    state.macros["QUOTE"] = "\"";
    state.macros["SRCDIR"] = base_path;

    // Tokenize
    auto tokens = tokenize(content);

    // Parse
    size_t i = 0;
    parse_tokens(tokens, i, state);

    // Finalize
    finalize_solution(state);

    return state.solution;
}

} // namespace vcxproj
