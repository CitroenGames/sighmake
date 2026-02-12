#pragma once

#include "common/project_types.hpp"

namespace vcxproj {

// VPC (Valve Project Creator) parser
// Converts .vpc files to sighmake Solution/Project structures
class VpcParser {
public:
    VpcParser() = default;

    // Parse a VPC file and return a Solution
    Solution parse(const std::string& filepath);

    // Parse from string content (for testing)
    Solution parse_string(const std::string& content, const std::string& base_path = ".");

    // Set target platforms to extract (default: all Windows platforms)
    void set_target_platforms(const std::vector<std::string>& platforms) {
        m_target_platforms = platforms;
    }

private:
    // Token types for VPC syntax
    enum class TokenType {
        Keyword,        // $Macro, $Include, $Project, etc.
        String,         // "quoted strings"
        OpenBrace,      // {
        CloseBrace,     // }
        Condition,      // [$WIN32], [!$LINUX]
        Identifier,     // unquoted identifiers
        EndOfFile
    };

    struct Token {
        TokenType type;
        std::string value;
        int line = 0;
    };

    // Parse state maintained during parsing
    struct ParseState {
        Solution solution;
        Project* current_project = nullptr;
        std::string base_path;
        std::string current_file;  // Current file being parsed (for error messages)
        int line_number = 0;

        // Macro table: NAME -> value (without $ prefix)
        std::map<std::string, std::string> macros;

        // Conditional state: NAME -> is_defined (without $ prefix)
        std::map<std::string, bool> conditionals;

        // Include stack for circular include detection
        std::vector<std::string> include_stack;

        // Current configuration name (when inside $Configuration block)
        std::string current_config;

        // Current folder path (when inside $Folder block)
        std::vector<std::string> folder_stack;

        // Platform-conditional data accumulation
        // Maps platform -> sources/settings that apply only to that platform
        struct PlatformSources {
            std::vector<std::string> sources;
            std::vector<std::string> libs;
        };
        std::map<std::string, PlatformSources> platform_sources;

        // Active condition for current line (if any)
        std::string active_condition;
    };

    // Target platforms to extract
    std::vector<std::string> m_target_platforms = {"WIN32", "WIN64"};

    // Tokenizer
    std::vector<Token> tokenize(const std::string& content);

    // Main parsing methods
    void parse_tokens(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void parse_block(std::vector<Token>& tokens, size_t& i, ParseState& state,
                    const std::string& block_type);

    // Keyword handlers
    void handle_macro(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_macro_required(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_conditional(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_include(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_configuration(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_project(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_folder(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_file(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_lib(std::vector<Token>& tokens, size_t& i, ParseState& state);

    // Configuration sub-handlers
    void handle_general(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_compiler(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_linker(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_librarian(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_pre_build_event(std::vector<Token>& tokens, size_t& i, ParseState& state);
    void handle_post_build_event(std::vector<Token>& tokens, size_t& i, ParseState& state);

    // Macro resolution - expands $NAME references
    std::string resolve_macros(const std::string& str, const ParseState& state);

    // Conditional evaluation - evaluates [condition] expressions
    bool evaluate_condition(const std::string& condition, const ParseState& state);
    bool parse_condition_expr(const std::string& expr, const ParseState& state);

    // Include processing
    void process_include(const std::string& path, ParseState& state);

    // Initialize default conditionals based on target platforms
    void init_conditionals(ParseState& state);

    // Map VPC platform conditional to buildscript platform
    std::string map_platform_conditional(const std::string& conditional);

    // Extract platforms from a condition string
    std::vector<std::string> extract_platforms_from_condition(const std::string& condition);

    // Finalize solution after parsing
    void finalize_solution(ParseState& state);

    // Helpers
    static std::string trim(const std::string& str);
    static std::string normalize_path(const std::string& path);
    static std::string to_upper(const std::string& str);
    std::string resolve_path(const std::string& path, const std::string& base_path);

    // Get next non-condition token value (skips condition tokens)
    std::string get_next_value(std::vector<Token>& tokens, size_t& i, ParseState& state);

    // Check for and consume a condition token, returns condition or empty
    std::string check_condition(std::vector<Token>& tokens, size_t i);

    // Skip to end of current block (matching braces)
    void skip_block(std::vector<Token>& tokens, size_t& i);

    // Error handling
    void parse_error(const std::string& message, const ParseState& state, int line = -1);
    void parse_warning(const std::string& message, const ParseState& state, int line = -1);
};

} // namespace vcxproj
