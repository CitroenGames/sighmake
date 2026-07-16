#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace vcxproj {

// Lowercase a copy of the string (ASCII).
// Single shared implementation — use this instead of inlining std::transform.
inline std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Uppercase a copy of the string (ASCII).
inline std::string to_upper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                  [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

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
