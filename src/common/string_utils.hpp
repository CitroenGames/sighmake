#pragma once

#include <string>
#include <vector>

namespace vcxproj {

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
