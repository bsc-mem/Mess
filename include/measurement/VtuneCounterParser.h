#pragma once
#include "architecture/BandwidthCounterStrategy.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace measurement::vtune {
struct VtuneSample {
    bool success = false;
    double elapsed_s = 0.0;
    long long cas_rd = 0;
    long long cas_wr = 0;
    std::map<std::string, long long> extra_values;
    std::string error;
    std::string raw_output;
    int exit_code = -1;
    bool elapsed_found = false;
    int matched_lines = 0;
};
std::string canonicalize_vtune_event_name(const std::string& event);

std::vector<std::string> vtune_event_aliases(const std::string& requested_event);

std::string match_extra_counter_key(const std::string& reported_event,
                                    const BandwidthCounterSelection& selection);

std::string regex_escape(const std::string& value);

long long parse_counter_value(const std::string& raw);

int accumulate_exact_vtune_event_matches(const std::string& output,
                                         const std::vector<std::string>& events,
                                         long long& total,
                                         int verbosity,
                                         const char* label);

int accumulate_exact_vtune_extra_matches(const std::string& output,
                                         const std::vector<std::string>& events,
                                         std::map<std::string, long long>& extra_values,
                                         int verbosity);

bool parse_vtune_counter_line(const std::string& line, const BandwidthCounterSelection& selection,
    long long& cas_rd, long long& cas_wr, std::map<std::string, long long>& extra_values, int verbosity);
bool emon_is_separator(const std::string& line);
std::vector<std::string> emon_split_samples(const std::string& output, const std::set<std::string>& wanted);
long long emon_first_tsc(const std::string& output, const std::string& event);
long long emon_sum_event(const std::string& output, const std::string& event, int& matched_lines);

std::string first_meaningful_vtune_error(const std::string& output);
VtuneSample parse_vtune_report(const std::string& output, const BandwidthCounterSelection& selection,
    VtuneSample sample, bool command_ok, const std::string& error, bool legacy_events, int verbosity);
VtuneSample parse_emon_report(const std::string& output, const BandwidthCounterSelection& selection,
    double window_seconds, VtuneSample sample, int verbosity);
std::vector<VtuneSample> parse_emon_batch(const std::string& output, const BandwidthCounterSelection& selection,
    double seconds, double wall, bool timed_shape, int exit_code, const std::string& command, int verbosity);
}
