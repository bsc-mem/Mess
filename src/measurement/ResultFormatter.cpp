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

#include "measurement/ResultFormatter.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace measurement {

std::string format_bandwidth_output(const BenchmarkResult& result,
                                    const BandwidthCounterSelection& selection,
                                    int bw_bytes_per_cas) {
    std::stringstream bw_ss;

    std::string read_event = "unc_m_cas_count.rd";
    std::string write_event = "unc_m_cas_count.wr";

    if (selection.type == CounterType::UPI_FLITS) {
        if (!selection.upi.rxl_data_events.empty()) read_event = selection.upi.rxl_data_events[0];
        if (!selection.upi.txl_data_events.empty()) write_event = selection.upi.txl_data_events[0];
    } else if (selection.type == CounterType::CAS_COUNT ||
               selection.type == CounterType::NVIDIA_GRACE ||
               selection.type == CounterType::TOTAL_BYTES) {
        if (!selection.cas.read_events.empty()) {
            std::string evt = selection.cas.read_events[0];
            if (evt.find("event=") == std::string::npos) read_event = evt;
        }
        if (!selection.cas.write_events.empty()) {
            std::string evt = selection.cas.write_events[0];
            if (evt.find("event=") == std::string::npos) write_event = evt;
        }
        if (!selection.cas.combined_events.empty() && selection.cas.read_events.empty()) {
            read_event = selection.cas.combined_events[0];
            write_event = selection.cas.combined_events[0];
        }
    }

    const bool is_pcm_cxl_mode = is_pcm_cxl_selection(selection);

    bw_ss << "\n Performance counter stats for system wide:\n\n";
    if (is_pcm_cxl_mode) {
        auto get_extra_scaled = [&](const std::string& key) -> double {
            auto it = result.extra_perf_values.find(key);
            if (it == result.extra_perf_values.end()) {
                return 0.0;
            }
            return static_cast<double>(it->second) / 100.0;
        };

        double read_mbps = get_extra_scaled("pcm_cxl_read_mbps_x100");
        double write_mbps = get_extra_scaled("pcm_cxl_write_mbps_x100");
        double total_mbps = get_extra_scaled("pcm_cxl_total_mbps_x100");

        if (read_mbps <= 0.0 && write_mbps <= 0.0 && result.bw_elapsed > 0.0) {
            read_mbps = (static_cast<double>(result.bw_cas_rd) * bw_bytes_per_cas) / (result.bw_elapsed * 1e6);
            write_mbps = (static_cast<double>(result.bw_cas_wr) * bw_bytes_per_cas) / (result.bw_elapsed * 1e6);
            total_mbps = read_mbps + write_mbps;
        } else if (total_mbps <= 0.0) {
            total_mbps = read_mbps + write_mbps;
        }

        bw_ss << "S0           " << result.traffic_gen_samples << "                  "
              << std::fixed << std::setprecision(2) << read_mbps
              << "      pcm_cxl_read_mbps\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  "
              << std::fixed << std::setprecision(2) << write_mbps
              << "      pcm_cxl_write_mbps\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  "
              << std::fixed << std::setprecision(2) << total_mbps
              << "      pcm_cxl_total_mbps\n";
    } else if (selection.cas.uses_read_subtract_formula) {
        auto get_extra_value = [&](const std::string& event) -> long long {
            auto it = result.extra_perf_values.find(event);
            return (it != result.extra_perf_values.end()) ? it->second : 0LL;
        };

        long long subtract_total = 0;
        for (const auto& event : selection.cas.read_subtract_events) {
            subtract_total += get_extra_value(event);
        }

        const long long raw_read = result.bw_cas_rd + subtract_total;
        const long long raw_write = result.bw_cas_wr;

        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << result.bw_cas_rd
              << "      a64fx_read_effective\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << result.bw_cas_wr
              << "      a64fx_write_effective\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << raw_read
              << "      r17\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << raw_write
              << "      r18\n";
        for (const auto& event : selection.cas.read_subtract_events) {
            bw_ss << "S0           " << result.traffic_gen_samples << "                  " << get_extra_value(event)
                  << "      " << event << "\n";
        }
    } else if (read_event.find("dram_channel_data_controller") != std::string::npos ||
        write_event.find("dram_channel_data_controller") != std::string::npos) {
        long long total_cas = result.bw_cas_rd + result.bw_cas_wr;
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << total_cas << "      dram_channel_data_controller_total\n";
    } else {
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << result.bw_cas_rd << "      " << read_event << "\n";
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << result.bw_cas_wr << "      " << write_event << "\n";
    }

    for (const auto& kv : result.extra_perf_values) {
        if (is_pcm_cxl_mode &&
            (kv.first == "pcm_cxl_read_mbps_x100" ||
             kv.first == "pcm_cxl_write_mbps_x100" ||
             kv.first == "pcm_cxl_total_mbps_x100")) {
            continue;
        }
        if (selection.cas.uses_read_subtract_formula &&
            (kv.first == "r17" || kv.first == "r18" ||
             std::find(selection.cas.read_subtract_events.begin(),
                       selection.cas.read_subtract_events.end(),
                       kv.first) != selection.cas.read_subtract_events.end())) {
            continue;
        }
        bw_ss << "S0           " << result.traffic_gen_samples << "                  " << kv.second << "      " << kv.first << "\n";
    }

    for (const auto& metric : result.extra_metric_values) {
        bw_ss << "S0           " << result.traffic_gen_samples << "                  "
              << std::setprecision(17) << metric.second << "      " << metric.first << "\n";
    }

    bw_ss << "\n       " << std::fixed << std::setprecision(9) << result.bw_elapsed << " seconds time elapsed\n\n";

    return bw_ss.str();
}

std::string format_latency_output(const BenchmarkResult& result,
                                  pid_t ptrchase_pid,
                                  bool instruction_sampling) {
    std::stringstream lat_ss;
    lat_ss << "\n Performance counter stats for process id '" << ptrchase_pid << "':\n\n";

    if (instruction_sampling) {
        lat_ss << std::fixed << std::setprecision(0);
        lat_ss << "   " << result.sampler_samples << "      samples\n";
        lat_ss << std::fixed << std::setprecision(2);
        lat_ss << "   " << result.sampler_mean_ns << "      mean_ns\n";
        lat_ss << "   " << result.sampler_iqr_mean_ns << "      iqr_mean_ns\n";
        lat_ss << "   " << result.sampler_min_ns << "      min_ns\n";
        lat_ss << "   " << result.sampler_median_ns << "      median_ns\n";
        lat_ss << "   " << result.sampler_p90_ns << "      p90_ns\n";
        lat_ss << "   " << result.sampler_p95_ns << "      p95_ns\n";
        lat_ss << "   " << result.sampler_p99_ns << "      p99_ns\n";
        lat_ss << "   " << result.sampler_p99_9_ns << "      p99_9_ns\n";
        lat_ss << "   " << result.sampler_max_ns << "      max_ns\n";
        lat_ss << "   " << result.sampler_mean_cycles << "      mean_cycles\n";
        lat_ss << "   " << result.sampler_iqr_mean_cycles << "      iqr_mean_cycles\n";
        lat_ss << "   " << result.sampler_min_cycles << "      min_cycles\n";
        lat_ss << "   " << result.sampler_median_cycles << "      median_cycles\n";
        lat_ss << "   " << result.sampler_p90_cycles << "      p90_cycles\n";
        lat_ss << "   " << result.sampler_p95_cycles << "      p95_cycles\n";
        lat_ss << "   " << result.sampler_p99_cycles << "      p99_cycles\n";
        lat_ss << "   " << result.sampler_p99_9_cycles << "      p99_9_cycles\n";
        lat_ss << "   " << result.sampler_max_cycles << "      max_cycles\n";
        lat_ss << std::fixed << std::setprecision(9);
        lat_ss << "   " << result.point_cpu_freq_ghz << "      point_cpu_freq_ghz\n";
        lat_ss << "\n       " << result.bw_elapsed << " seconds time elapsed\n\n";
    } else {
        lat_ss << std::fixed << std::setprecision(0);
        lat_ss << "   " << result.cycles << "      cycles\n";
        lat_ss << "   " << result.instructions << "      instructions\n";
        lat_ss << "   " << result.accesses << "      accesses\n";
        lat_ss << "   " << result.tlb1miss << "      " << result.tlb1_event_name << "\n";
        lat_ss << "   " << result.tlb2miss << "      " << result.tlb2_event_name << "\n";
        lat_ss << std::fixed << std::setprecision(9);
        lat_ss << "   " << result.point_cpu_freq_ghz << "      point_cpu_freq_ghz\n";
        lat_ss << "   " << result.pointer_chase_thread_ns << "      pointer_chase_thread_ns\n";
        lat_ss << "   " << result.latency_direct_ns << "      latency_direct_ns\n";
        lat_ss << "\n       " << result.duration_s << " seconds time elapsed\n\n";
    }

    return lat_ss.str();
}


}
