/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          javier.beiro     [at] bsc [dot] es
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

#include "gpu/GpuMeasurement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

namespace gpu {
namespace {
const char* const kReadRate = "dram__bytes_read.sum.per_second";
const char* const kWriteRate = "dram__bytes_write.sum.per_second";
const char* const kClockRate = "sm__cycles_elapsed.avg.per_second";
const char* const kCycles = "sm__cycles_elapsed.avg";

bool number(const std::string& text, double& value) {
    try {
        size_t end = 0;
        value = std::stod(text, &end);
        return end == text.size() && std::isfinite(value) && value >= 0.0;
    } catch (const std::exception&) {
        return false;
    }
}
}

std::vector<std::string> measurement_metrics(const std::vector<std::string>& extra_metrics) {
    std::vector<std::string> metrics = {kReadRate, kWriteRate, kClockRate, kCycles};
    for (const auto& metric : extra_metrics)
        if (std::find(metrics.begin(), metrics.end(), metric) == metrics.end()) metrics.push_back(metric);
    return metrics;
}

bool parse_sample(const std::string& line, const std::vector<std::string>& metrics, BenchmarkResult& result, std::string& error) {
    error.clear();
    std::istringstream fields(line);
    std::string tag;
    if (!(fields >> tag) || tag != "SAMPLE") {
        error = "GPU worker did not return a sample: " + line;
        return false;
    }
    std::map<std::string, double> values;
    for (std::string field; fields >> field;) {
        const auto separator = field.find('=');
        double value = 0.0;
        if (separator == std::string::npos || !number(field.substr(separator + 1), value) ||
            !values.emplace(field.substr(0, separator), value).second) {
            error = "Invalid or duplicate GPU sample field: " + field;
            return false;
        }
    }
    std::vector<std::string> required = {"ratio", "pause", "accesses", "cycles", "ns"};
    required.insert(required.end(), metrics.begin(), metrics.end());
    for (const auto& key : required) {
        if (!values.count(key)) {
            error = "GPU sample is missing " + key;
            return false;
        }
    }
    const double read = values[kReadRate], write = values[kWriteRate];
    const double clock = values[kClockRate], cycles = values[kCycles];
    const double accesses = values["accesses"], elapsed_ns = values["ns"];
    if (clock <= 0 || cycles <= 0 || read + write <= 0 || accesses <= 0 || elapsed_ns <= 0 ||
        values["cycles"] <= 0) {
        error = "Invalid GPU clock, duration, bandwidth, or access count";
        return false;
    }
    // The shared result format stores bandwidth rates in signed integer fields.
    const double maximum_rate = static_cast<double>(std::numeric_limits<long long>::max());
    if (read >= maximum_rate || write >= maximum_rate) {
        error = "GPU bandwidth exceeds the result format range";
        return false;
    }
    BenchmarkResult parsed{};
    parsed.ratio_pct = values["ratio"];
    parsed.pause = static_cast<int>(values["pause"]);
    parsed.bw_cas_rd = read;
    parsed.bw_cas_wr = write;
    parsed.bw_elapsed = cycles / clock;
    parsed.traffic_gen_samples = 1;
    parsed.repetitions = 1;
    parsed.measured_ratio_pct = 100.0 * read / (read + write);
    parsed.bandwidth_mbps = (read + write) / 1e6;
    parsed.cycles = values["cycles"];
    parsed.instructions = accesses;
    parsed.accesses = accesses;
    parsed.duration_s = elapsed_ns / 1e9;
    parsed.latency_ns = elapsed_ns / accesses;
    parsed.latency_direct_ns = parsed.latency_ns;
    for (size_t i = 4; i < metrics.size(); ++i) parsed.extra_metric_values[metrics[i]] = values[metrics[i]];
    result = parsed;
    return true;
}

bool parse_memory(const std::string& ready, GPUInfo& info) {
    double clock = 0.0, width = 0.0;
    std::istringstream fields(ready);
    for (std::string field; fields >> field;) {
        const auto separator = field.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = field.substr(0, separator);
        if (key == "memory_clock_mhz") number(field.substr(separator + 1), clock);
        else if (key == "bus_width_bits") number(field.substr(separator + 1), width);
    }
    if (clock <= 0.0 || width <= 0.0) return false;
    info.max_mem_clock_mhz = clock;
    info.bus_width_bits = static_cast<int>(width);
    return true;
}
}
