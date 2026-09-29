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
