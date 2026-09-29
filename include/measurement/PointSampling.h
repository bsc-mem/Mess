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

#ifndef MEASUREMENT_POINT_SAMPLING_H
#define MEASUREMENT_POINT_SAMPLING_H

#include "BenchmarkConfig.h"
#include "CurveTracer.h"
#include <cmath>
#include <vector>
#include <utility>

namespace measurement {
struct PointSamplingPolicy {
    int repetitions = 3;
    int max_point_attempts = 10;
    int max_outlier_rounds = 3;
};

inline ProbeStats summarize_point(int pause, const std::vector<BenchmarkResult>& results) {
    ProbeStats stats;
    stats.pause = pause;
    stats.n_reps = static_cast<int>(results.size());
    if (results.empty()) return stats;
    for (const auto& result : results) {
        stats.bw += result.bandwidth_mbps / 1000.0;
        stats.lat += result.latency_ns;
    }
    stats.bw /= results.size();
    stats.lat /= results.size();
    for (const auto& result : results) {
        const double bw_delta = result.bandwidth_mbps / 1000.0 - stats.bw;
        const double lat_delta = result.latency_ns - stats.lat;
        stats.sigma_bw += bw_delta * bw_delta;
        stats.sigma_lat += lat_delta * lat_delta;
    }
    stats.sigma_bw = std::sqrt(stats.sigma_bw / results.size());
    stats.sigma_lat = std::sqrt(stats.sigma_lat / results.size());
    return stats;
}

// Backend callbacks own execution/retry mechanics; only complete stable points escape.
template<class Measure, class FindOutliers, class BeginAttempt>
bool resolve_point(const PointSamplingPolicy& policy, Measure measure,
                   FindOutliers find_outliers, BeginAttempt begin_attempt,
                   std::vector<BenchmarkResult>& results) {
    results.clear();
    if (policy.repetitions <= 0) return false;
    for (int attempt = 0; attempt < policy.max_point_attempts; ++attempt) {
        begin_attempt();
        results.clear();
        bool complete = true;
        for (int rep = 0; rep < policy.repetitions; ++rep) {
            BenchmarkResult result{};
            if (!measure(rep, result)) { complete = false; break; }
            results.push_back(std::move(result));
        }
        if (!complete) continue;
        auto outliers = find_outliers(results);
        for (int round = 0; !outliers.empty() && round < policy.max_outlier_rounds; ++round) {
            for (int rep : outliers) {
                BenchmarkResult replacement{};
                if (rep < 0 || rep >= policy.repetitions || !measure(rep, replacement)) {
                    complete = false;
                    break;
                }
                results[static_cast<size_t>(rep)] = std::move(replacement);
            }
            if (!complete) break;
            outliers = find_outliers(results);
        }
        if (complete && outliers.empty()) return true;
    }
    results.clear();
    return false;
}
}

#endif
