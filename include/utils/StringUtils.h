/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the copyright holder nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef UTILS_STRING_UTILS_H
#define UTILS_STRING_UTILS_H

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace string_utils {

inline std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\n\v\f\r");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t\n\v\f\r");
    return value.substr(first, last - first + 1);
}

inline std::string to_lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

inline bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }
    ) != haystack.end();
}

#if defined(_WIN32)
inline std::string shell_quote(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char c : value) {
        if (c == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('"');
    return quoted;
}
#else
inline std::string shell_quote(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}
#endif

inline bool parse_int_strict(const std::string& value, int& out) {
    try {
        size_t parsed = 0;
        const int parsed_value = std::stoi(value, &parsed);
        if (parsed != value.size()) {
            return false;
        }
        out = parsed_value;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parse_int_range(const std::string& token, int& lo, int& hi) {
    try {
        const size_t dash = token.find('-');
        if (dash == std::string::npos) {
            lo = hi = std::stoi(token);
        } else {
            lo = std::stoi(token.substr(0, dash));
            hi = std::stoi(token.substr(dash + 1));
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline bool is_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

inline std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += sep;
        out += items[i];
    }
    return out;
}

inline std::string join(const std::vector<int>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += sep;
        out += std::to_string(items[i]);
    }
    return out;
}

inline bool contains_word(const std::string& haystack, const std::string& word) {
    const std::string lower = to_lower(haystack);
    const std::string w = to_lower(word);
    if (w.empty()) return false;
    size_t pos = 0;
    while ((pos = lower.find(w, pos)) != std::string::npos) {
        const bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
        const bool right_ok = (pos + w.size() == lower.size()) ||
                              !std::isalnum(static_cast<unsigned char>(lower[pos + w.size()]));
        if (left_ok && right_ok) return true;
        pos += w.size();
    }
    return false;
}

}

#endif
