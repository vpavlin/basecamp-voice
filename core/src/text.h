#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Text from outside (catalog descriptions, module replies, model output,
// server logs) is UTF-8 that must stay valid: nlohmann's dump() throws on a
// character cut in half, and a throw on a worker thread ends the module.

// At most `bytes` bytes, never ending inside a character.
inline std::string utf8Prefix(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// The last `bytes` bytes at most, never starting inside a character.
inline std::string utf8Suffix(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t start = s.size() - bytes;
    while (start < s.size() && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) ++start;
    return s.substr(start);
}

// dump() that replaces invalid UTF-8 instead of throwing.
inline std::string safeDump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
