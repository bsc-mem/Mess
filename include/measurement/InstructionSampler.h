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

#ifndef INSTRUCTION_SAMPLER_H
#define INSTRUCTION_SAMPLER_H

#include <algorithm>
#include <numeric>
#include <vector>
#include <cstdint>
#include <sys/types.h>
#include "platform/Process.h"

/**
 * @file InstructionSampler.h
 * @brief Abstract interface for instruction-level latency sampling backends.
 */

/**
 * @brief Summary statistics returned by an instruction-sampling backend.
 */
struct InstructionSamplerStats {
    long long samples = 0;
    
    double mean_cycles = 0.0;
    double iqr_mean_cycles = 0.0;
    double min_cycles = 0.0;
    double median_cycles = 0.0;
    double p90_cycles = 0.0;
    double p95_cycles = 0.0;
    double p99_cycles = 0.0;
    double p99_9_cycles = 0.0;
    double max_cycles = 0.0;

    double mean_ns = 0.0;
    double iqr_mean_ns = 0.0;
    double min_ns = 0.0;
    double median_ns = 0.0;
    double p90_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
    double p99_9_ns = 0.0;
    double max_ns = 0.0;

    std::vector<uint64_t> raw_latencies;
};

inline InstructionSamplerStats build_instruction_sampler_stats(std::vector<uint64_t> latencies,
                                                               double cpu_freq_ghz) {
    InstructionSamplerStats stats{};
    if (latencies.empty()) {
        return stats;
    }

    std::sort(latencies.begin(), latencies.end());
    stats.samples = static_cast<long long>(latencies.size());
    stats.raw_latencies = std::move(latencies);

    double sum = std::accumulate(stats.raw_latencies.begin(), stats.raw_latencies.end(), 0.0);
    stats.mean_cycles = sum / stats.samples;
    stats.min_cycles = stats.raw_latencies.front();
    stats.max_cycles = stats.raw_latencies.back();

    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * (stats.raw_latencies.size() - 1));
        return stats.raw_latencies[idx];
    };

    stats.median_cycles = pct(0.50);
    stats.p90_cycles = pct(0.90);
    stats.p95_cycles = pct(0.95);
    stats.p99_cycles = pct(0.99);
    stats.p99_9_cycles = pct(0.999);

    size_t q1_idx = static_cast<size_t>(0.25 * (stats.raw_latencies.size() - 1));
    size_t q3_idx = static_cast<size_t>(0.75 * (stats.raw_latencies.size() - 1));
    double iqr_sum = std::accumulate(
        stats.raw_latencies.begin() + q1_idx,
        stats.raw_latencies.begin() + q3_idx + 1,
        0.0);
    stats.iqr_mean_cycles = iqr_sum / (q3_idx - q1_idx + 1);

    if (cpu_freq_ghz > 0.0) {
        stats.mean_ns = stats.mean_cycles / cpu_freq_ghz;
        stats.iqr_mean_ns = stats.iqr_mean_cycles / cpu_freq_ghz;
        stats.min_ns = stats.min_cycles / cpu_freq_ghz;
        stats.median_ns = stats.median_cycles / cpu_freq_ghz;
        stats.p90_ns = stats.p90_cycles / cpu_freq_ghz;
        stats.p95_ns = stats.p95_cycles / cpu_freq_ghz;
        stats.p99_ns = stats.p99_cycles / cpu_freq_ghz;
        stats.p99_9_ns = stats.p99_9_cycles / cpu_freq_ghz;
        stats.max_ns = stats.max_cycles / cpu_freq_ghz;
    }

    return stats;
}

/**
 * @brief Base class for hardware-backed instruction latency samplers.
 * 
 * Instead of relying on a software-based pointer chasing loop, modern architectures
 * support precise hardware instruction sampling (e.g., Intel PEBS, ARM SPE) which captures
 * the exact latency of individual memory instructions.
 * 
 */
class InstructionSampler {
public:
    virtual ~InstructionSampler() = default;

    virtual bool is_available() const = 0;

    virtual bool start_async(const std::vector<int>& cores, const std::vector<platform::ProcessId>& pids) = 0;

    virtual InstructionSamplerStats stop_and_process(double cpu_freq_ghz) = 0;

    virtual void cleanup() {}
};

#endif
