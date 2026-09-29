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
#include "measurement/TrafficGenArrayCalibrator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr uint64_t kSmallLlcThresholdBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDefaultLlcMultiplier = 4ULL;
constexpr uint64_t kSmallLlcMultiplier = 128ULL;

uint64_t minimum_llc_multiplier(uint64_t llc_bytes) {
    return llc_bytes <= kSmallLlcThresholdBytes
               ? kSmallLlcMultiplier
               : kDefaultLlcMultiplier;
}

void add_allowed_node_token(const std::string& token, std::vector<int>& nodes) {
    const size_t dash = token.find('-');
    try {
        if (dash == std::string::npos) {
            nodes.push_back(std::stoi(token));
            return;
        }
        int first = std::stoi(token.substr(0, dash));
        int last = std::stoi(token.substr(dash + 1));
        if (last < first) std::swap(first, last);
        for (int node = first; node <= last; ++node) {
            nodes.push_back(node);
        }
    } catch (...) {}
}

std::vector<int> read_allowed_memory_nodes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("Mems_allowed_list:", 0) != 0) {
            continue;
        }

        std::string list = line.substr(std::string("Mems_allowed_list:").size());
        list.erase(std::remove_if(list.begin(), list.end(), [](unsigned char c) {
            return std::isspace(c);
        }), list.end());

        std::vector<int> nodes;
        std::stringstream ss(list);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                add_allowed_node_token(token, nodes);
            }
        }
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        return nodes;
    }
    return {};
}

uint64_t read_node_mem_total_bytes(int node) {
    std::ifstream meminfo("/sys/devices/system/node/node" + std::to_string(node) + "/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        const size_t pos = line.find("MemTotal:");
        if (pos == std::string::npos) {
            continue;
        }
        std::istringstream iss(line.substr(pos + std::string("MemTotal:").size()));
        uint64_t kb = 0;
        iss >> kb;
        return kb * 1024ULL;
    }
    return 0;
}

uint64_t allowed_memory_bytes(const CPUCapabilities& capabilities) {
    uint64_t total = 0;
    for (int node : read_allowed_memory_nodes()) {
        total += read_node_mem_total_bytes(node);
    }
    return total > 0 ? total : static_cast<uint64_t>(capabilities.total_memory);
}

uint64_t array_memory_cap_bytes(const CPUCapabilities& capabilities) {
    const uint64_t available_bytes = allowed_memory_bytes(capabilities);
    return available_bytes / 5;
}

}

TrafficGenArrayCalibrator::TrafficGenArrayCalibrator(int verbosity)
    : verbosity_(verbosity) {}

uint64_t TrafficGenArrayCalibrator::isa_capacity_multiplier(
    const KernelConfig& kernel_config,
    const CPUCapabilities& capabilities) {
    (void)capabilities;
    return (kernel_config.isa_mode == ISAMode::AVX512) ? 2ULL : 1ULL;
}

uint64_t TrafficGenArrayCalibrator::fallback_elements(const CPUCapabilities& capabilities) {
    const uint64_t llc_based_bytes = (capabilities.l3_size > 0)
                                       ? static_cast<uint64_t>(capabilities.l3_size) *
                                             minimum_llc_multiplier(capabilities.l3_size)
                                       : 512ULL * 1024ULL * 1024ULL;
    return std::max<uint64_t>(1ULL, llc_based_bytes / sizeof(double));
}

uint64_t TrafficGenArrayCalibrator::fallback_elements(
    const KernelConfig& kernel_config,
    const CPUCapabilities& capabilities) {
    return fallback_elements(capabilities) *
           isa_capacity_multiplier(kernel_config, capabilities);
}

std::vector<uint64_t> TrafficGenArrayCalibrator::build_candidates(
    const KernelConfig& kernel_config,
    const CPUCapabilities& capabilities) {
    std::vector<uint64_t> candidates;
    const uint64_t isa_multiplier = isa_capacity_multiplier(kernel_config, capabilities);
    const uint64_t memory_cap_bytes = array_memory_cap_bytes(capabilities);

    auto add_bytes = [&](uint64_t bytes) {
        if (bytes == 0) return;
        bytes *= isa_multiplier;
        if (memory_cap_bytes > 0) {
            bytes = std::min(bytes, memory_cap_bytes);
        }
        uint64_t elements = std::max<uint64_t>(1ULL, bytes / sizeof(double));
        if (std::find(candidates.begin(), candidates.end(), elements) == candidates.end()) {
            candidates.push_back(elements);
        }
    };

    if (capabilities.l3_size == 0) {
        add_bytes(512ULL * 1024ULL * 1024ULL);
        add_bytes(1ULL * 1024ULL * 1024ULL * 1024ULL);
        add_bytes(2ULL * 1024ULL * 1024ULL * 1024ULL);
        return candidates;
    }

    const uint64_t llc = static_cast<uint64_t>(capabilities.l3_size);

    add_bytes(llc * 4ULL);
    add_bytes(llc * 5ULL);
    add_bytes(llc * 6ULL);
    add_bytes(llc * 8ULL);
    add_bytes(llc * 12ULL);

    uint64_t last_bytes = 0;
    if (!candidates.empty()) {
        last_bytes = candidates.back() * sizeof(double) / isa_multiplier;
    }

    auto add_if_bigger_than_last = [&](uint64_t bytes) {
        if (bytes > last_bytes) {
            add_bytes(bytes);
            last_bytes = bytes;
        }
    };

    add_if_bigger_than_last(llc * 20ULL);

    if (llc <= kSmallLlcThresholdBytes && !capabilities.isAppleSilicon()) {
        add_bytes(llc * kSmallLlcMultiplier);
    }
    
    return candidates;
}

uint64_t TrafficGenArrayCalibrator::capacity_elements(
    const KernelConfig& kernel_config,
    const CPUCapabilities& capabilities) {
    std::vector<uint64_t> candidates = build_candidates(kernel_config, capabilities);
    if (candidates.empty()) {
        return fallback_elements(kernel_config, capabilities);
    }
    return candidates.back();
}

TrafficGenArrayCalibrationResult TrafficGenArrayCalibrator::run(
    const std::vector<uint64_t>& candidates,
    ProbeFn probe) const {
    TrafficGenArrayCalibrationResult result;
    if (candidates.empty() || !probe) {
        return result;
    }

    const double plateau_fraction = 0.98;
    const double cache_drop_fraction = 0.85;
    bool saw_cache_drop = false;
    size_t post_drop_start_index = 0;
    double post_drop_best_bandwidth_gb_s = 0.0;

    for (uint64_t candidate : candidates) {
        TrafficGenArrayProbeResult probe_result = probe(candidate);
        probe_result.elements = candidate;
        result.probes.push_back(probe_result);

        if (probe_result.ok) {
            if (!saw_cache_drop && result.best_bandwidth_gb_s > 0.0 &&
                probe_result.bandwidth_gb_s < cache_drop_fraction * result.best_bandwidth_gb_s) {
                saw_cache_drop = true;
                post_drop_start_index = result.probes.size() - 1;
                post_drop_best_bandwidth_gb_s = probe_result.bandwidth_gb_s;
                if (verbosity_ >= 2) {
                    std::cout << "  Calibration detected out-of-cache bandwidth drop"
                              << std::endl;
                }
            } else if (saw_cache_drop) {
                if (post_drop_best_bandwidth_gb_s > 0.0 &&
                    probe_result.bandwidth_gb_s < cache_drop_fraction * post_drop_best_bandwidth_gb_s) {
                    post_drop_start_index = result.probes.size() - 1;
                    post_drop_best_bandwidth_gb_s = probe_result.bandwidth_gb_s;
                    if (verbosity_ >= 2) {
                        std::cout << "  Calibration detected deeper out-of-cache bandwidth drop"
                                  << std::endl;
                    }
                } else {
                    post_drop_best_bandwidth_gb_s =
                        std::max(post_drop_best_bandwidth_gb_s, probe_result.bandwidth_gb_s);
                }
            }

            result.best_bandwidth_gb_s = std::max(result.best_bandwidth_gb_s,
                                                  probe_result.bandwidth_gb_s);
        }
    }

    if (result.best_bandwidth_gb_s > 0.0) {
        size_t post_drop_ok_count = 0;
        if (saw_cache_drop) {
            for (size_t i = post_drop_start_index; i < result.probes.size(); ++i) {
                if (result.probes[i].ok) {
                    ++post_drop_ok_count;
                }
            }
        }

        const bool use_post_drop_plateau = saw_cache_drop && post_drop_ok_count >= 2;
        const double selection_best =
            use_post_drop_plateau ? post_drop_best_bandwidth_gb_s : result.best_bandwidth_gb_s;
        const size_t begin = use_post_drop_plateau ? post_drop_start_index : 0;
        for (size_t i = begin; i < result.probes.size(); ++i) {
            const TrafficGenArrayProbeResult& probe_result = result.probes[i];
            if (i + 1 < result.probes.size() && probe_result.ok &&
                result.probes[i + 1].ok &&
                probe_result.bandwidth_gb_s >= plateau_fraction * selection_best &&
                result.probes[i + 1].bandwidth_gb_s >= cache_drop_fraction * probe_result.bandwidth_gb_s) {
                result.selected_elements = probe_result.elements;
                break;
            }
        }
    }

    return result;
}
