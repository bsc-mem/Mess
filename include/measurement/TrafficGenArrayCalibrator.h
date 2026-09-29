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
#ifndef TRAFFIC_GEN_ARRAY_CALIBRATOR_H
#define TRAFFIC_GEN_ARRAY_CALIBRATOR_H

#include <cstdint>
#include <functional>
#include <vector>

#include "KernelTypes.h"
#include "SystemDetection.h"

struct TrafficGenArrayProbeResult {
    uint64_t elements = 0;
    double bandwidth_gb_s = 0.0;
    double latency_ns = 0.0;
    bool ok = false;
};

struct TrafficGenArrayCalibrationResult {
    uint64_t selected_elements = 0;
    double best_bandwidth_gb_s = 0.0;
    std::vector<TrafficGenArrayProbeResult> probes;
};

class TrafficGenArrayCalibrator {
public:
    using ProbeFn = std::function<TrafficGenArrayProbeResult(uint64_t elements)>;

    explicit TrafficGenArrayCalibrator(int verbosity = 1);

    static uint64_t isa_capacity_multiplier(const KernelConfig& kernel_config,
                                            const CPUCapabilities& capabilities);
    static std::vector<uint64_t> build_candidates(const KernelConfig& kernel_config,
                                                  const CPUCapabilities& capabilities);
    static uint64_t capacity_elements(const KernelConfig& kernel_config,
                                      const CPUCapabilities& capabilities);
    static uint64_t fallback_elements(const CPUCapabilities& capabilities);
    static uint64_t fallback_elements(const KernelConfig& kernel_config,
                                      const CPUCapabilities& capabilities);

    TrafficGenArrayCalibrationResult run(const std::vector<uint64_t>& candidates,
                                         ProbeFn probe) const;

private:
    int verbosity_;
};

#endif
