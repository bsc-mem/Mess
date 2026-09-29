#include "measurement/VtuneCounterParser.h"
#include "utils/StringUtils.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>
#include <iomanip>

namespace measurement::vtune {
std::string canonicalize_vtune_event_name(const std::string& event) {
    if (event.empty()) {
        return event;
    }
    if (event.find('/') != std::string::npos || event[0] == 'r') {
        return event;
    }

    std::string canonical = event;
    for (char& c : canonical) {
        if (std::islower(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return canonical;
}

std::vector<std::string> vtune_event_aliases(const std::string& requested_event) {
    std::vector<std::string> aliases;
    const std::string canonical = canonicalize_vtune_event_name(requested_event);
    if (!canonical.empty()) {
        aliases.push_back(canonical);
    }

    const std::string lower = string_utils::to_lower(requested_event);
    if (lower == "instructions") {
        aliases.push_back("INST_RETIRED.ANY");
    } else if (lower == "cycles" || lower == "cpu-cycles") {
        aliases.push_back("CPU_CLK_UNHALTED.THREAD");
    } else if (lower == "ref-cycles") {
        aliases.push_back("CPU_CLK_UNHALTED.REF_TSC");
    }

    std::sort(aliases.begin(), aliases.end());
    aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
    return aliases;
}

std::string match_extra_counter_key(const std::string& reported_event,
                                    const BandwidthCounterSelection& selection) {
    for (const auto& requested_event : selection.extra_counters) {
        const std::vector<std::string> aliases = vtune_event_aliases(requested_event);
        for (const auto& alias : aliases) {
            if (string_utils::contains_ci(reported_event, alias)) {
                return requested_event;
            }
        }
    }
    return "";
}

std::string regex_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 2);
    for (char c : value) {
        switch (c) {
            case '\\':
            case '^':
            case '$':
            case '.':
            case '|':
            case '?':
            case '*':
            case '+':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
                escaped.push_back('\\');
                escaped.push_back(c);
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

long long parse_counter_value(const std::string& raw) {
    std::string digits;
    digits.reserve(raw.size());
    for (char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        }
    }
    if (digits.empty()) {
        return 0;
    }
    try {
        return std::stoll(digits);
    } catch (...) {
        return 0;
    }
}

int accumulate_exact_vtune_event_matches(const std::string& output,
                                         const std::vector<std::string>& events,
                                         long long& total,
                                         int verbosity,
                                         const char* label) {
    int matches = 0;
    for (const auto& raw_event : events) {
        const std::string event = canonicalize_vtune_event_name(raw_event);
        if (event.empty()) {
            continue;
        }

        const std::regex pattern(
            "(?:^|\\n)\\s*" + regex_escape(event) + R"((?:\[[^\]\n]+\])?\s+([0-9][0-9,]*))",
            std::regex::icase);

        for (std::sregex_iterator it(output.begin(), output.end(), pattern), end; it != end; ++it) {
            const long long value = parse_counter_value((*it)[1].str());
            total += value;
            matches++;

            if (verbosity >= 4) {
                std::cout << "[VTUNE DEBUG] exact-match[" << label << "]: "
                          << string_utils::trim((*it)[0].str())
                          << " value=" << value << std::endl;
            }
        }
    }
    return matches;
}

int accumulate_exact_vtune_extra_matches(const std::string& output,
                                         const std::vector<std::string>& events,
                                         std::map<std::string, long long>& extra_values,
                                         int verbosity) {
    int matches = 0;
    for (const auto& requested_event : events) {
        const std::vector<std::string> aliases = vtune_event_aliases(requested_event);
        if (aliases.empty()) {
            continue;
        }

        long long total = 0;
        for (const auto& alias : aliases) {
            const std::regex pattern(
                "(?:^|\\n)\\s*" + regex_escape(alias) + R"((?:\[[^\]\n]+\])?\s+([0-9][0-9,]*))",
                std::regex::icase);

            for (std::sregex_iterator it(output.begin(), output.end(), pattern), end; it != end; ++it) {
                const long long value = parse_counter_value((*it)[1].str());
                total += value;
                matches++;

                if (verbosity >= 4) {
                    std::cout << "[VTUNE DEBUG] exact-match[extra]: "
                              << string_utils::trim((*it)[0].str())
                              << " value=" << value
                              << " requested=" << requested_event << std::endl;
                }
            }
        }

        if (total > 0) {
            extra_values[requested_event] += total;
        }
    }
    return matches;
}

bool parse_vtune_counter_line(const std::string& line,
                              const BandwidthCounterSelection& selection,
                              long long& cas_rd,
                              long long& cas_wr,
                              std::map<std::string, long long>& extra_values,
                              int verbosity) {
    const std::string trimmed = string_utils::trim(line);
    if (trimmed.empty()) {
        return false;
    }

    size_t value_end = trimmed.size();
    while (value_end > 0 && std::isspace(static_cast<unsigned char>(trimmed[value_end - 1]))) {
        value_end--;
    }
    if (value_end == 0) {
        return false;
    }

    size_t value_start = value_end;
    while (value_start > 0) {
        char c = trimmed[value_start - 1];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == ',') {
            value_start--;
            continue;
        }
        break;
    }

    if (value_start == value_end) {
        return false;
    }

    const std::string value_token = trimmed.substr(value_start, value_end - value_start);
    std::string event_name = string_utils::trim(trimmed.substr(0, value_start));
    if (event_name.empty()) {
        return false;
    }
    if (event_name.find("Event Type") != std::string::npos) {
        return false;
    }

    const long long value = parse_counter_value(value_token);
    EventClassification classification = EventClassification::classify(event_name, selection);

    if (verbosity >= 4) {
        std::cout << "[VTUNE DEBUG] counter-line: " << event_name
                  << " value=" << value
                  << " rd=" << classification.is_read
                  << " wr=" << classification.is_write
                  << " comb=" << classification.is_combined
                  << " extra=" << classification.is_extra
                  << std::endl;
    }

    if (classification.is_combined) {
        cas_rd += value;
    } else if (classification.is_read) {
        cas_rd += value;
    } else if (classification.is_write) {
        cas_wr += value;
    } else if (classification.is_extra) {
        EventClassification::accumulate_extra(extra_values, classification.extra_key, value);
    }

    return true;
}

bool emon_is_separator(const std::string& trimmed) {
    if (trimmed.size() < 10) return false;
    const char c = trimmed.front();
    if (c != '-' && c != '=') return false;
    return trimmed.find_first_not_of(c) == std::string::npos;
}

std::vector<std::string> emon_split_samples(const std::string& output,
                                            const std::set<std::string>& wanted_lower) {
    std::vector<std::string> blocks;
    std::string current;
    std::set<std::string> seen;

    auto flush = [&]() {
        if (!current.empty()) blocks.push_back(current);
        current.clear();
        seen.clear();
    };

    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = string_utils::trim(line);
        if (trimmed.empty()) continue;
        if (emon_is_separator(trimmed)) {
            flush();
            continue;
        }

        const size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            const std::string name =
                string_utils::to_lower(string_utils::trim(line.substr(0, tab)));
            if (wanted_lower.count(name)) {
                if (seen.count(name)) flush();
                seen.insert(name);
            }
        }
        current += line;
        current += '\n';
    }
    flush();
    return blocks;
}

/** @brief Returns the sample TSC count, or 0. */
long long emon_first_tsc(const std::string& output, const std::string& event) {
    std::istringstream in(output);
    std::string line;
    const std::string wanted = string_utils::to_lower(event);

    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name;
        if (!std::getline(fields, name, '\t')) continue;
        if (string_utils::to_lower(string_utils::trim(name)) != wanted) continue;

        std::string field;
        while (std::getline(fields, field, '\t')) {
            const std::string t = string_utils::trim(field);
            if (t.empty()) continue;
            std::string digits;
            for (const char c : t) {
                if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
                else if (c == ',') continue;
                else break;
            }
            if (digits.empty()) return 0;
            try { return std::stoll(digits); } catch (...) { return 0; }
        }
    }
    return 0;
}

long long emon_sum_event(const std::string& output, const std::string& event,
                         int& matched_lines) {
    long long total = 0;
    std::istringstream in(output);
    std::string line;
    const std::string wanted = string_utils::to_lower(event);

    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name;
        if (!std::getline(fields, name, '\t')) continue;
        if (string_utils::to_lower(string_utils::trim(name)) != wanted) continue;

        std::string field;
        bool skipped_tsc = false;
        bool any = false;
        while (std::getline(fields, field, '\t')) {
            const std::string t = string_utils::trim(field);
            if (t.empty()) continue;
            if (!skipped_tsc) { skipped_tsc = true; continue; }
            if (t.find("N/A") != std::string::npos) continue;

            std::string digits;
            for (const char c : t) {
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    digits.push_back(c);
                } else if (c == ',') {
                    continue;
                } else {
                    break;
                }
            }
            if (digits.empty()) continue;
            try {
                total += std::stoll(digits);
                any = true;
            } catch (...) {
            }
        }
        if (any) ++matched_lines;
    }
    return total;
}

std::string first_meaningful_vtune_error(const std::string& output) {
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = string_utils::trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.find("vtune: Error:") != std::string::npos) {
            return trimmed;
        }
    }
    return "";
}
VtuneSample parse_vtune_report(const std::string& parse_output, const BandwidthCounterSelection& selection,
    VtuneSample sample, bool command_ok, const std::string& vtune_error, bool legacy_events, int verbosity) {
    std::smatch elapsed_match;
    if (std::regex_search(parse_output, elapsed_match, std::regex(R"(Elapsed Time:\s*([0-9]+(?:\.[0-9]+)?))"))) {
        try {
            sample.elapsed_s = std::stod(elapsed_match[1].str());
            sample.elapsed_found = true;
        } catch (...) {
            sample.elapsed_s = 0.0;
        }
    }

    if (verbosity >= 4) {
        std::cout << "[VTUNE DEBUG] exit-code: " << sample.exit_code
                  << " elapsed-found: " << (sample.elapsed_found ? "yes" : "no")
                  << " elapsed: " << sample.elapsed_s << std::endl;
    }

    std::istringstream iss(parse_output);
    std::string line;
    const bool using_combined = selection.cas.has_combined_counter && !selection.cas.has_read_write;
    int matched_counter_lines = 0;

    if (selection.type == CounterType::CAS_COUNT ||
        selection.type == CounterType::NVIDIA_GRACE ||
        selection.type == CounterType::TOTAL_BYTES) {
        matched_counter_lines += accumulate_exact_vtune_event_matches(
            parse_output, selection.cas.read_events, sample.cas_rd, verbosity, "rd");
        matched_counter_lines += accumulate_exact_vtune_event_matches(
            parse_output, selection.cas.write_events, sample.cas_wr, verbosity, "wr");
        matched_counter_lines += accumulate_exact_vtune_event_matches(
            parse_output, selection.cas.combined_events, sample.cas_rd, verbosity, "combined");
        if (legacy_events) {
            matched_counter_lines += accumulate_exact_vtune_event_matches(
                parse_output, std::vector<std::string>{"UNC_M_CAS_COUNT.ALL"}, sample.cas_rd, verbosity, "legacy-combined");
            matched_counter_lines += accumulate_exact_vtune_event_matches(
                parse_output, std::vector<std::string>{"UNC_M_CAS_COUNT.RD"}, sample.cas_rd, verbosity, "legacy-rd");
            matched_counter_lines += accumulate_exact_vtune_event_matches(
                parse_output, std::vector<std::string>{"UNC_M_CAS_COUNT.WR"}, sample.cas_wr, verbosity, "legacy-wr");
        }
    } else if (selection.type == CounterType::UPI_FLITS) {
        matched_counter_lines += accumulate_exact_vtune_event_matches(
            parse_output, selection.upi.rxl_data_events, sample.cas_rd, verbosity, "rxl");
        matched_counter_lines += accumulate_exact_vtune_event_matches(
            parse_output, selection.upi.txl_data_events, sample.cas_wr, verbosity, "txl");
    }
    matched_counter_lines += accumulate_exact_vtune_extra_matches(
        parse_output, selection.extra_counters, sample.extra_values, verbosity);

    bool in_uncore_summary = false;
    int uncore_debug_lines = 0;

    if (matched_counter_lines == 0) {
        while (std::getline(iss, line)) {
            const std::string trimmed = string_utils::trim(line);
            if (trimmed == "Uncore Event summary") {
                in_uncore_summary = true;
                continue;
            }

            if (in_uncore_summary) {
                if (trimmed.empty()) {
                    if (matched_counter_lines > 0) {
                        break;
                    }
                    continue;
                }
                if (trimmed.find("Uncore Event Type") != std::string::npos ||
                    trimmed.find("--------") == 0) {
                    continue;
                }

                if (verbosity >= 4 && uncore_debug_lines < 20) {
                    std::cout << "[VTUNE DEBUG] uncore-raw-line: " << trimmed << std::endl;
                    uncore_debug_lines++;
                }

                if (parse_vtune_counter_line(line, selection, sample.cas_rd, sample.cas_wr, sample.extra_values, verbosity)) {
                    matched_counter_lines++;
                }
            }
        }

        sample.cas_rd = 0;
        sample.cas_wr = 0;
        sample.extra_values.clear();
        iss.clear();
        iss.str(parse_output);
        while (std::getline(iss, line)) {
            if (parse_vtune_counter_line(line, selection, sample.cas_rd, sample.cas_wr, sample.extra_values, verbosity)) {
                matched_counter_lines++;
            }
        }
    }

    sample.matched_lines = matched_counter_lines;

    if (sample.elapsed_s <= 0.0) {
        sample.error = !vtune_error.empty() ? vtune_error : "VTune did not report elapsed time";
        return sample;
    }

    const bool combined = using_combined || (legacy_events && sample.cas_rd > 0 &&
        string_utils::contains_ci(parse_output, "UNC_M_CAS_COUNT.ALL"));
    if (combined) {
        sample.success = sample.cas_rd > 0;
    } else {
        sample.success = sample.cas_rd > 0 && sample.cas_wr > 0;
    }

    if (!sample.success && sample.error.empty()) {
        sample.error = !vtune_error.empty()
            ? vtune_error
            : (command_ok ? "VTune did not report usable counter values" : "VTune command failed");
    }

    return sample;
}
VtuneSample parse_emon_report(const std::string& output, const BandwidthCounterSelection& selection,
    double window_seconds, VtuneSample sample, int verbosity) {
    const std::string rd_event = string_utils::to_lower("UNC_M_CAS_COUNT.RD");
    const std::string wr_event = string_utils::to_lower("UNC_M_CAS_COUNT.WR");
    bool found_rd = false;
    bool found_wr = false;
    int counter_groups = 0;
    bool group_has_counter = false;

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmed = string_utils::trim(line);
        if (!trimmed.empty() &&
            std::all_of(trimmed.begin(), trimmed.end(), [](char c) { return c == '-'; })) {
            if (group_has_counter) {
                counter_groups++;
                group_has_counter = false;
            }
            continue;
        }

        std::istringstream tokens(trimmed);
        std::string name;
        if (!(tokens >> name)) {
            continue;
        }

        const std::string lower_name = string_utils::to_lower(name);
        const bool is_rd = (lower_name == rd_event);
        const bool is_wr = (lower_name == wr_event);
        const std::string extra_key = (!is_rd && !is_wr)
            ? match_extra_counter_key(name, selection)
            : "";
        if (!is_rd && !is_wr && extra_key.empty()) {
            continue;
        }
        group_has_counter = true;

        long long row_total = 0;
        std::string token;
        bool clocktick_column = true;
        while (tokens >> token) {
            if (clocktick_column) {
                clocktick_column = false;
                continue;
            }
            row_total += parse_counter_value(token);
        }

        if (verbosity >= 4) {
            std::cout << "[VTUNE DEBUG] emon-row: " << name
                      << " total=" << row_total << std::endl;
        }

        if (is_rd) {
            sample.cas_rd += row_total;
            found_rd = true;
        } else if (is_wr) {
            sample.cas_wr += row_total;
            found_wr = true;
        } else {
            EventClassification::accumulate_extra(sample.extra_values, extra_key, row_total);
        }
    }
    if (group_has_counter) {
        counter_groups++;
    }

    std::smatch elapsed_match;
    if (std::regex_search(output, elapsed_match,
                          std::regex(R"(([0-9]+(?:\.[0-9]+)?)s\s+real)"))) {
        try {
            sample.elapsed_s = std::stod(elapsed_match[1].str());
            sample.elapsed_found = true;
        } catch (...) {
            sample.elapsed_s = 0.0;
        }
    }
    if (!sample.elapsed_found || sample.elapsed_s <= 0.0) {
        sample.elapsed_s = std::max(0.5, window_seconds);
        sample.elapsed_found = true;
    }
    if (counter_groups > 1) {
        sample.elapsed_s /= counter_groups;
    }
    sample.success = found_rd && found_wr && (sample.cas_rd + sample.cas_wr) > 0;
    if (!sample.success && sample.error.empty()) {
        const std::string vtune_error = first_meaningful_vtune_error(output);
        sample.error = !vtune_error.empty()
            ? vtune_error
            : "emon did not report usable counter values";
    }
    return sample;
}
std::vector<VtuneSample> parse_emon_batch(const std::string& output, const BandwidthCounterSelection& selection,
    double seconds, double wall, bool timed_shape, int code, const std::string& cmd, int verbosity) {
    std::vector<VtuneSample> samples;
    std::set<std::string> wanted;
    for (const auto& e : selection.cas.read_events) {
        wanted.insert(string_utils::to_lower(canonicalize_vtune_event_name(e)));
    }
    for (const auto& e : selection.cas.write_events) {
        wanted.insert(string_utils::to_lower(canonicalize_vtune_event_name(e)));
    }

    const std::vector<std::string> blocks = emon_split_samples(output, wanted);

    for (const auto& block : blocks) {
        VtuneSample sample;
        sample.exit_code = code;

        int matched = 0;
        for (const auto& e : selection.cas.read_events) {
            sample.cas_rd += emon_sum_event(block, canonicalize_vtune_event_name(e), matched);
        }
        for (const auto& e : selection.cas.write_events) {
            sample.cas_wr += emon_sum_event(block, canonicalize_vtune_event_name(e), matched);
        }
        sample.matched_lines = matched;
        if (matched < 2) continue;

        sample.elapsed_s = timed_shape
            ? seconds
            : (wall / static_cast<double>(std::max<size_t>(1, blocks.size())));
        sample.elapsed_found = true;
        sample.success = true;

        if (verbosity >= 3 && samples.empty() && !selection.cas.read_events.empty()) {
            // Compare TSC/window with the nominal clock to detect bandwidth scaling errors.
            const long long tsc = emon_first_tsc(
                block, canonicalize_vtune_event_name(selection.cas.read_events.front()));
            if (tsc > 0 && sample.elapsed_s > 0.0) {
                std::ostringstream note;
                note << std::fixed << std::setprecision(3) << sample.elapsed_s
                     << " s, tsc " << tsc << " -> implied " << std::setprecision(2)
                     << (tsc / sample.elapsed_s / 1e9) << " GHz";
                std::cout << "    emon window " << note.str()
                          << " (expect the nominal clock)" << std::endl;
            }
        }

        samples.push_back(sample);
    }

    if (verbosity >= 4) {
        std::cout << "[EMON DEBUG] " << cmd << "\n[EMON DEBUG] blocks=" << blocks.size()
                  << " samples=" << samples.size() << " wall=" << wall << "s" << std::endl;
        for (size_t i = 0; i < samples.size(); ++i) {
            std::cout << "[EMON DEBUG]   [" << i << "] rd=" << samples[i].cas_rd
                      << " wr=" << samples[i].cas_wr
                      << " elapsed=" << samples[i].elapsed_s << std::endl;
        }
    }

    if (samples.empty()) {
        VtuneSample failed;
        failed.exit_code  = code;
        failed.raw_output = output;
        failed.error      = "emon returned no counter lines";
        samples.push_back(failed);
    }
    return samples;
}
}
