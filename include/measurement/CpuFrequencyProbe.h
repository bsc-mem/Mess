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

#ifndef CPU_FREQUENCY_PROBE_H
#define CPU_FREQUENCY_PROBE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct CpuFrequencySample {
    uint64_t cycles = 0;
    uint64_t time_enabled_ns = 0;
    uint64_t time_running_ns = 0;
    double thread_seconds = 0.0;
    int cpu = -1;
};

struct CpuFrequencyProbeResult {
    double frequency_ghz = 0.0;
    double spread_pct = 0.0;
    int cpu = -1;
    size_t accepted_samples = 0;
    std::string error;
    bool ok() const { return frequency_ghz > 0.0 && error.empty(); }
};

// Validates counter coverage and CPU identity, then takes a stable median.
CpuFrequencyProbeResult summarize_cpu_frequency_samples(
    int cpu, const std::vector<CpuFrequencySample>& samples);

// Runs on the pointer-chase CPU and restores the calling thread's affinity.
CpuFrequencyProbeResult probe_cpu_frequency(int cpu);

#endif
