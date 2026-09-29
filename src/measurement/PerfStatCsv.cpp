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

#include "measurement/PerfStatCsv.h"
#include "utils/StringUtils.h"

#include <cmath>
#include <cctype>
#include <sstream>
#include <vector>

namespace perf_stat_csv {
namespace {

bool parse_long_long_token(const std::string& token, long long& value) {
    const std::string trimmed = string_utils::trim(token);
    if (trimmed.empty()) return false;
    try {
        size_t consumed = 0;
        value = std::stoll(trimmed, &consumed, 10);
        return consumed == trimmed.size();
    } catch (...) {
        return false;
    }
}

bool parse_double_token(const std::string& token, double& value) {
    const std::string trimmed = string_utils::trim(token);
    if (trimmed.empty()) return false;
    try {
        size_t consumed = 0;
        value = std::stod(trimmed, &consumed);
        return consumed == trimmed.size();
    } catch (...) {
        return false;
    }
}

bool token_looks_like_event_name(const std::string& token) {
    const std::string trimmed = string_utils::trim(token);
    if (trimmed.empty()) return false;
    for (unsigned char ch : trimmed) {
        if (ch == '/' || ch == '_' || ch == '.' || ch == ':' || ch == '-') return true;
    }
    if (trimmed.size() >= 2 && (trimmed[0] == 'r' || trimmed[0] == 'R')) {
        for (size_t i = 1; i < trimmed.size(); ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(trimmed[i]))) return false;
        }
        return true;
    }
    return trimmed == "cycles" || trimmed == "instructions";
}

bool token_looks_like_scope_name(const std::string& token) {
    const std::string trimmed = string_utils::trim(token);
    if (trimmed.empty()) return false;
    if (trimmed.size() >= 2 && (trimmed[0] == 'S' || trimmed[0] == 's')) {
        for (size_t i = 1; i < trimmed.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) return false;
        }
        return true;
    }
    if (trimmed.size() >= 4 &&
        (trimmed.rfind("CPU", 0) == 0 || trimmed.rfind("cpu", 0) == 0)) {
        for (size_t i = 3; i < trimmed.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) return false;
        }
        return true;
    }
    return false;
}

bool token_looks_like_metric_unit(const std::string& token) {
    const std::string trimmed = string_utils::trim(token);
    if (trimmed.empty()) return false;
    const std::string unit = string_utils::to_lower(trimmed);
    return unit == "b" || unit == "kb" || unit == "mb" || unit == "gb" ||
           unit == "tb" || unit == "kib" || unit == "mib" || unit == "gib" ||
           unit == "tib" || unit == "%" || unit == "hz" || unit == "khz" ||
           unit == "mhz" || unit == "ghz";
}

double metric_unit_to_bytes_multiplier(const std::string& unit_raw) {
    std::string unit = string_utils::to_lower(string_utils::trim(unit_raw));
    if (unit.empty()) return 0.0;
    const size_t slash = unit.find('/');
    if (slash != std::string::npos) unit = unit.substr(0, slash);
    if (unit == "b") return 1.0;
    if (unit == "kb") return 1e3;
    if (unit == "mb") return 1e6;
    if (unit == "gb") return 1e9;
    if (unit == "tb") return 1e12;
    if (unit == "kib") return 1024.0;
    if (unit == "mib") return 1024.0 * 1024.0;
    if (unit == "gib") return 1024.0 * 1024.0 * 1024.0;
    if (unit == "tib") return 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return 0.0;
}

}

bool parse_row(const std::string& line, bool expect_timestamp, Row& row) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(string_utils::trim(field));
    if (fields.empty()) return false;
    row = Row{};
    size_t index = 0;
    if (expect_timestamp) {
        if (!parse_double_token(fields[index], row.timestamp)) return false;
        row.has_timestamp = true;
        ++index;
    }
    long long parsed_value = 0;
    auto assign_raw = [&](size_t scope_index, size_t value_index, size_t event_index) -> bool {
        if (value_index >= fields.size() || event_index >= fields.size()) return false;
        if (!parse_long_long_token(fields[value_index], parsed_value) ||
            !token_looks_like_event_name(fields[event_index])) return false;
        row.scope = (scope_index < fields.size()) ? fields[scope_index] : "";
        row.value = parsed_value;
        row.has_metric_value = false;
        row.metric_value = 0.0;
        row.metric_unit.clear();
        row.event_name = fields[event_index];
        return true;
    };
    auto assign_metric = [&](size_t scope_index, size_t metric_value_index, size_t unit_index, size_t event_index) -> bool {
        if (metric_value_index >= fields.size() || event_index >= fields.size()) return false;
        if (unit_index < fields.size() && !fields[unit_index].empty() &&
            !token_looks_like_metric_unit(fields[unit_index])) return false;
        if (!token_looks_like_event_name(fields[event_index])) return false;
        double metric_value = 0.0;
        if (!parse_double_token(fields[metric_value_index], metric_value)) return false;
        row.scope = (scope_index < fields.size()) ? fields[scope_index] : "";
        row.value = static_cast<long long>(std::llround(metric_value));
        row.metric_value = metric_value;
        row.metric_unit = (unit_index < fields.size()) ? fields[unit_index] : "";
        row.has_metric_value = true;
        row.event_name = fields[event_index];
        return true;
    };
    const bool has_scope = (index < fields.size()) && token_looks_like_scope_name(fields[index]);
    if (has_scope && assign_metric(index, index + 2, index + 3, index + 4)) {
        if (!token_looks_like_metric_unit(row.event_name)) return true;
        row = Row{};
    }
    if (has_scope && assign_metric(index, index + 3, index + 4, index + 5)) return true;
    if (has_scope && assign_metric(index, index + 1, index + 2, index + 3)) return true;
    if (has_scope && assign_raw(index, index + 2, index + 4)) return true;
    if (has_scope && assign_raw(index, index + 1, index + 3)) return true;
    if (!has_scope && assign_metric(fields.size(), index, index + 1, index + 2)) return true;
    if (!has_scope && assign_raw(fields.size(), index, index + 2)) return true;
    return false;
}

long long normalize_row_value(const Row& row, int cache_line_size) {
    if (!row.has_metric_value) return row.value;
    const double bytes_multiplier = metric_unit_to_bytes_multiplier(row.metric_unit);
    if (bytes_multiplier > 0.0) {
        const double bytes = row.metric_value * bytes_multiplier;
        if (cache_line_size > 0) {
            return static_cast<long long>(std::llround(bytes / static_cast<double>(cache_line_size)));
        }
        return static_cast<long long>(std::llround(bytes));
    }
    return static_cast<long long>(std::llround(row.metric_value));
}

}
