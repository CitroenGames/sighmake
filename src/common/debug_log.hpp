#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace vcxproj {

// Runtime-gated diagnostic logging. Enable by setting SIGHMAKE_DEBUG to any
// value other than "0". Replaces the old compile-time NDEBUG gates so debug
// builds stay quiet by default and release builds can still be diagnosed.
inline bool debug_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SIGHMAKE_DEBUG");
        return value && *value && std::string(value) != "0";
    }();
    return enabled;
}

// Stream for [DEBUG] messages; discards output when debugging is disabled.
inline std::ostream& debug_stream() {
    struct NullBuffer : std::streambuf {
        int overflow(int c) override { return c; }
    };
    static NullBuffer null_buffer;
    static std::ostream null_stream(&null_buffer);
    return debug_enabled() ? std::cout : null_stream;
}

} // namespace vcxproj
