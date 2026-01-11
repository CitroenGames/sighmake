#pragma once

#include "common/project_types.hpp"
#include <string>
#include <vector>
#include <map>
#include <fstream>

namespace vcxproj {

class CMakeParser {
public:
    CMakeParser() = default;

    // Parse a CMakeLists.txt file and return a Solution
    Solution parse(const std::string& filepath);

    // Parse from string content
    Solution parse_string(const std::string& content, const std::string& base_path = ".");

private:
    // Basic token types
    enum class TokenType {
        Identifier,
        String,
        OpenParen,
        CloseParen,
        VariableReference,
        EndOfFile
    };

    struct Token {
        TokenType type;
        std::string value;
        int line;
    };

    struct FunctionDef {
        std::vector<std::string> params;
        std::vector<Token> body;
    };

    struct ParseState {
        Solution* solution = nullptr;
        std::string base_path;
        int line_number = 0;
        
        // Variables state
        std::map<std::string, std::string> variables;
        
        // Current context
        std::string current_source_dir;
        std::string current_binary_dir;

        // Defined functions and macros
        std::map<std::string, FunctionDef> functions;
        std::map<std::string, FunctionDef> macros;
    };

    // Helper to tokenize content
    std::vector<Token> tokenize(const std::string& content);

    // Helper to resolve variables in a string
    std::string resolve_variables(const std::string& str, const ParseState& state);

    // Helper to execute a sequence of tokens
    void execute_tokens(const std::vector<Token>& tokens, size_t& i, ParseState& state);

    // Command handlers
    void handle_project(const std::vector<std::string>& args, ParseState& state);
    void handle_add_executable(const std::vector<std::string>& args, ParseState& state);
    void handle_add_library(const std::vector<std::string>& args, ParseState& state);
    void handle_set(const std::vector<std::string>& args, ParseState& state);
    void handle_target_include_directories(const std::vector<std::string>& args, ParseState& state);
    void handle_target_link_libraries(const std::vector<std::string>& args, ParseState& state);
    void handle_target_compile_definitions(const std::vector<std::string>& args, ParseState& state);
    void handle_target_sources(const std::vector<std::string>& args, ParseState& state);
    void handle_include_directories(const std::vector<std::string>& args, ParseState& state);
    void handle_link_directories(const std::vector<std::string>& args, ParseState& state);
    void handle_file(const std::vector<std::string>& args, ParseState& state);
    void handle_add_subdirectory(const std::vector<std::string>& args, ParseState& state);
    
    // New handlers
    void handle_message(const std::vector<std::string>& args, ParseState& state);
    void handle_option(const std::vector<std::string>& args, ParseState& state);
    void handle_list(const std::vector<std::string>& args, ParseState& state);
    void handle_target_compile_options(const std::vector<std::string>& args, ParseState& state);
    void handle_find_library(const std::vector<std::string>& args, ParseState& state);
    void handle_find_path(const std::vector<std::string>& args, ParseState& state);
    // Stubs for ignored/simple commands
    void handle_cmake_minimum_required(const std::vector<std::string>& args, ParseState& state);
    void handle_enable_testing(const std::vector<std::string>& args, ParseState& state);
    void handle_add_test(const std::vector<std::string>& args, ParseState& state);

    void handle_function_def(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state);
    void handle_macro_def(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state);
    void handle_if(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state);
    void handle_while(const std::vector<std::string>& args, size_t& i, const std::vector<Token>& tokens, ParseState& state);

    // Helper to find or create a project
    Project* find_project(const std::string& name, ParseState& state);
    
    // Helper to add files to a project
    void add_sources_to_project(Project* project, const std::vector<std::string>& sources, ParseState& state);

    // Helper to expand globs
    std::vector<std::string> expand_glob(const std::string& pattern, const std::string& base_path);

    // Helper to evaluate conditions
    bool evaluate_condition(const std::vector<std::string>& args, ParseState& state);

    // Helper to propagate include directories from linked projects
    void propagate_include_directories(Solution& solution);
};

} // namespace vcxproj