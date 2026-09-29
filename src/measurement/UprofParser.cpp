#include "measurement/UprofParser.h"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace measurement::uprof {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, ',')) out.push_back(trim(field));
    return out;
}

bool to_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || !std::isfinite(v) || v < 0.0) return false;
    if (end == s.c_str()) return false;
    while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end && *end != '\0') return false;
    out = v;
    return true;
}

/**
 * @brief True for a `Umc<N>RdBw (GB/s)`-style counter name.
 *
 * The index is not parsed: every UMC is summed. Matched case-insensitively,
 * since a silent no-match would read as zero bandwidth rather than as an error.
 */
bool is_umc_metric(const std::string& name, bool& is_read) {
    std::string upper;
    upper.reserve(name.size());
    for (const char c : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper.rfind("UMC", 0) != 0 || upper.find("GB/S") == std::string::npos) return false;

    if (upper.find("RDBW") != std::string::npos) { is_read = true;  return true; }
    if (upper.find("WRBW") != std::string::npos) { is_read = false; return true; }
    return false;
}

} // namespace

/**
 * @brief How many `DF METRICS` blocks a report contains.
 *
 * A multi-socket part may emit one per socket, a layout the parsers below have
 * cannot assign to requested NUMA nodes. Reject these reports instead of
 * returning a partial machine total.
 */
int count_df_sections(const std::string& text) {
    int sections = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).rfind("DF METRICS", 0) == 0) ++sections;
    }
    return sections;
}

/** @brief Parses the `-C` Metric,Value layout. */
UprofSample parse_cumulative(const std::string& text) {
    UprofSample s;
    if (count_df_sections(text) != 1) return s;
    bool have_read = false, have_write = false;
    std::istringstream in(text);
    std::string line;
    bool in_df = false;
    int matched = 0;

    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("DF METRICS", 0) == 0) {
            if (in_df) break;
            in_df = true;
            continue;
        }
        if (!in_df || t.empty()) continue;

        const auto f = split_csv(t);
        if (f.size() < 2) continue;

        bool is_read = false;
        if (!is_umc_metric(f[0], is_read)) continue;

        // Every numeric field: a part with per-socket columns would lose most of its traffic.
        for (size_t i = 1; i < f.size(); ++i) {
            double v = 0.0;
            if (!to_double(f[i], v)) return {};
            if (is_read) s.read_gbps += v; else s.write_gbps += v;
            if (is_read) have_read = true; else have_write = true;
            ++matched;
        }
    }
    s.valid = matched > 0 && have_read && have_write &&
              std::isfinite(s.read_gbps) && std::isfinite(s.write_gbps);
    return s;
}

/** @brief Parses the time-series layout. */
std::vector<UprofSample> parse_timeseries(const std::string& text) {
    std::vector<UprofSample> rows;
    if (count_df_sections(text) != 1) return rows;
    std::istringstream in(text);
    std::string line;
    bool in_df = false;
    std::vector<int> read_cols, write_cols;

    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("DF METRICS", 0) == 0) {
            if (in_df) break;
            in_df = true;
            continue;
        }
        if (!in_df || t.empty()) continue;

        const auto f = split_csv(t);

        if (read_cols.empty() && write_cols.empty()) {
            for (size_t i = 0; i < f.size(); ++i) {
                bool is_read = false;
                if (is_umc_metric(f[i], is_read)) {
                    if (is_read) read_cols.push_back(static_cast<int>(i));
                    else         write_cols.push_back(static_cast<int>(i));
                }
            }
            continue;
        }

        UprofSample s;
        bool complete = !read_cols.empty() && !write_cols.empty();
        for (const int c : read_cols) {
            double v = 0.0;
            if (c < static_cast<int>(f.size()) && to_double(f[c], v)) s.read_gbps += v;
            else complete = false;
        }
        for (const int c : write_cols) {
            double v = 0.0;
            if (c < static_cast<int>(f.size()) && to_double(f[c], v)) s.write_gbps += v;
            else complete = false;
        }
        if (complete && std::isfinite(s.read_gbps) && std::isfinite(s.write_gbps)) {
            s.valid = true; rows.push_back(s);
        }
    }
    return rows;
}


} // namespace measurement::uprof
