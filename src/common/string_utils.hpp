#pragma once

#include <string>
#include <vector>

namespace vcxproj {

// Trim leading/trailing whitespace (spaces, tabs, CR, LF).
// Single shared implementation — parsers and generators should use this
// instead of defining their own local copies.
inline std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// Split a CMake-style semicolon-separated list into individual entries
inline std::vector<std::string> split_semicolons(const std::string& value) {
    std::vector<std::string> result;
    std::string current;
    for (char c : value) {
        if (c == ';') {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

// Unescape newlines/backslashes that the buildscript parser encoded with \x01
inline std::string unescape_newlines(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    for (size_t i = 0; i < str.length(); ++i) {
        if (i + 1 < str.length() && str[i] == '\x01') {
            if (str[i + 1] == 'n') {
                result += '\n';
                ++i;
            } else if (str[i + 1] == '\\') {
                result += '\\';
                ++i;
            } else {
                result += str[i];
            }
        } else {
            result += str[i];
        }
    }
    return result;
}

} // namespace vcxproj
