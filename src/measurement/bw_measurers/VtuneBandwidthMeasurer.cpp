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

#include "measurement/bw_measurers/VtuneBandwidthMeasurer.h"
#include "measurement/BandwidthSampleAggregation.h"
#include "measurement/BandwidthStabilizer.h"
#include "utils/StringUtils.h"
#include "platform/Process.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

using measurement::vtune::VtuneSample;
std::string VtuneBandwidthMeasurer::find_vtune_binary() const { return locate_binary(); }
std::string VtuneBandwidthMeasurer::locate_binary() { return measurement::vtune::locate_binary(); }
bool VtuneBandwidthMeasurer::process_elevated() { return measurement::vtune::process_elevated(); }
bool VtuneBandwidthMeasurer::probe_available(int verbosity) { return measurement::vtune::probe_available(verbosity); }
std::string VtuneBandwidthMeasurer::setup_guidance() { return measurement::vtune::setup_guidance(); }

bool VtuneBandwidthMeasurer::sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed, const std::vector<int>& mem_nodes) const {
    (void)mem_nodes;
    const std::string vtune_binary = find_vtune_binary();
    if (vtune_binary.empty()) {
        std::cerr << "Error: vtune not found in PATH." << std::endl;
        return false;
    }

    extra_perf_values_.clear();

    const double sample_seconds = collection_->window_seconds(sampling_interval_ms_);
    VtuneSample sample = collection_->collect(vtune_binary, counter_selection_, sample_seconds,
        collection_->helper_cpu(sys_info_, config_, get_traffic_gen_cores()), config_.verbosity);

    if (!sample.success) {
        if (config_.verbosity >= 2 && !sample.error.empty()) {
            std::cerr << "VTune sampling failed: " << sample.error
                      << " (exit-code=" << sample.exit_code << ")" << std::endl;
        }
        return false;
    }

    cas_rd = sample.cas_rd;
    cas_wr = sample.cas_wr;
    elapsed = sample.elapsed_s;
    extra_perf_values_ = sample.extra_values;
    return true;
}

bool VtuneBandwidthMeasurer::wait_for_stabilization(int& samples_taken, long long& last_cas_rd, long long& last_cas_wr, double& last_elapsed, int pause, int ratio_pct, bool fast_resume, std::function<void()> on_sample_callback) {
    (void)fast_resume;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!traffic_gen_manager_ || !storage_) {
        return false;
    }

    int relaunch_attempts = 0;
    if (!ensure_traffic_gen_running(ratio_pct, pause, relaunch_attempts)) {
        return false;
    }

    const double theoretical_peak_gb_s = achievable_peak_gbps(counter_selection_.type == CounterType::UPI_FLITS);

    const size_t stabilization_window = 3;
    BandwidthStabilizer stabilizer = make_stabilizer(theoretical_peak_gb_s, pause, stabilization_window);

    const double sample_window_seconds = collection_->window_seconds(sampling_interval_ms_);
#if defined(_WIN32)
    const double timeout_window_seconds = sample_window_seconds;
#else
    const double timeout_window_seconds = std::max(0.5, sample_window_seconds);
#endif
    const int max_samples = std::max(3, static_cast<int>(60.0 / timeout_window_seconds));
    int sample_failures = 0;
    bool success = false;
    double successful_sample_elapsed_sum = 0.0;
    int successful_sample_count = 0;

    // Sized to fill the stabilizer's window with spares; drift otherwise costs a whole extra
    // batch.
    const int batch_loops = static_cast<int>(stabilization_window) + 3;

    std::vector<VtuneSample> batch;
    const std::string vtune_binary = find_vtune_binary();
    auto collect_batch = [&]() -> bool {
        if (vtune_binary.empty()) {
            std::cerr << "Error: vtune not found in PATH." << std::endl;
            return false;
        }
        batch = collection_->collect_batch(vtune_binary, counter_selection_, sample_window_seconds,
            batch_loops, collection_->helper_cpu(sys_info_, config_, get_traffic_gen_cores()), config_.verbosity);
        extra_perf_values_.clear();
        if (batch.empty()) return false;
        if (!batch.front().success) {
            if (config_.verbosity >= 2 && !batch.front().error.empty()) {
                std::cerr << "VTune sampling failed: " << batch.front().error
                          << " (exit-code=" << batch.front().exit_code << ")" << std::endl;
            }
            return false;
        }
        extra_perf_values_ = batch.back().extra_values;
        return true;
    };

    for (int total_samples = 0; total_samples < max_samples; ) {
        if (!traffic_gen_manager_->is_traffic_gen_running(traffic_gen_manager_->active_traffic_gen_pid())) {
            if (relaunch_attempts >= 2 || !relaunch_traffic_gen(ratio_pct, pause)) {
                samples_taken = total_samples;
                return false;
            }
            relaunch_attempts++;
            stabilizer.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (!collect_batch()) {
            sample_failures++;
            if (sample_failures >= 3) {
                samples_taken = total_samples;
                return false;
            }
            continue;
        }

        sample_failures = 0;
        for (const VtuneSample& sample : batch) {
            const long long sample_cas_rd = sample.cas_rd;
            const long long sample_cas_wr = sample.cas_wr;
            const double sample_elapsed = sample.elapsed_s;

            total_samples++;
            successful_sample_elapsed_sum += sample_elapsed;
            successful_sample_count++;
            if (on_sample_callback) {
                on_sample_callback();
            }

            const double bw_gb_s = calculate_bandwidth_gbps(
                sample_cas_rd, sample_cas_wr, sample_elapsed,
                counter_selection_.type, cached_cache_line_size_, cached_scaling_factor_);

            const StabilizationResult result = stabilizer.add_sample(sample_cas_rd, sample_cas_wr, bw_gb_s);
            if (result == StabilizationResult::ZOMBIE_DETECTED) {
                samples_taken = total_samples;
                return false;
            }

            if (config_.verbosity >= 3) {
                double read_ratio = 0.0;
                const long long total_cas = sample_cas_rd + sample_cas_wr;
                if (counter_selection_.type == CounterType::NVIDIA_GRACE) {
                    const double read_bytes = static_cast<double>(sample_cas_rd) * 32.0;
                    const double write_bytes = static_cast<double>(sample_cas_wr);
                    const double total_bytes = read_bytes + write_bytes;
                    if (total_bytes > 0.0) {
                        read_ratio = read_bytes / total_bytes;
                    }
                } else if (total_cas > 0) {
                    read_ratio = static_cast<double>(sample_cas_rd) / static_cast<double>(total_cas);
                }
                std::cout << "      [VTune Sample " << total_samples << "] ";
                stabilizer.print_status(read_ratio, bw_gb_s, counter_selection_.cas.has_read_write);
            }

            if (stabilizer.is_stable()) {
                success = true;
                samples_taken = total_samples;
                break;
            }
        }

        if (success) break;
    }

    if (!success) {
        return false;
    }

    const auto& samples = stabilizer.get_samples();
    const auto totals = measurement::aggregate_cas_samples(samples);
    last_cas_rd = totals.read;
    last_cas_wr = totals.write;
    const double average_sample_elapsed =
        (successful_sample_count > 0) ? (successful_sample_elapsed_sum / successful_sample_count)
                                      : collection_->window_seconds(sampling_interval_ms_);
    last_elapsed = samples.size() * average_sample_elapsed;
    return true;
}

bool VtuneBandwidthMeasurer::monitor_command(const std::string& command,
                                             MonitorCallback callback,
                                             bool summary_mode) {
    const std::string vtune_binary = find_vtune_binary();
    if (vtune_binary.empty()) {
        std::cerr << "Error: vtune not found in PATH." << std::endl;
        return false;
    }

    const std::string trimmed_command = string_utils::trim(command);
    if (trimmed_command.empty()) {
        std::cerr << "Error: No command specified for VTune monitoring." << std::endl;
        return false;
    }
    if (trimmed_command.rfind("-p ", 0) == 0 || trimmed_command.rfind("--pid ", 0) == 0) {
        std::cerr << "Error: VTune backend does not support PID attach mode in MessProfiler." << std::endl;
        return false;
    }

    extra_perf_values_.clear();

    if (!summary_mode) {
        const double window_seconds = collection_->window_seconds(sampling_interval_ms_);
        std::optional<platform::Process> child = platform::spawn_shell(trimmed_command);
        if (!child) {
            std::cerr << "Error: Failed to launch command for VTune monitoring." << std::endl;
            return false;
        }

        double timestamp = 0.0;
        bool saw_sample = false;

        while (true) {
            VtuneSample sample = collection_->collect(vtune_binary, counter_selection_, window_seconds,
                collection_->helper_cpu(sys_info_, config_, get_traffic_gen_cores()), config_.verbosity);
            if (sample.success) {
                saw_sample = true;
                timestamp += sample.elapsed_s;
                const double bw_gb_s = calculate_bandwidth_gbps(
                    sample.cas_rd, sample.cas_wr, sample.elapsed_s,
                    counter_selection_.type, cached_cache_line_size_, cached_scaling_factor_);
                for (const auto& kv : sample.extra_values) {
                    extra_perf_values_[kv.first] += kv.second;
                }
                if (callback) {
                    callback(timestamp, bw_gb_s, sample.cas_rd, sample.cas_wr, sample.extra_values);
                }
            }

            if (!child->running()) {
                break;
            }
        }

        int child_exit_code = -1;
        child->wait(std::chrono::milliseconds(-1), child_exit_code);
        return saw_sample;
    }

    VtuneSample sample = collection_->profile(
        vtune_binary, counter_selection_, trimmed_command, config_.verbosity);
    if (!sample.success) {
        if (!sample.error.empty()) {
            std::cerr << "Error: " << sample.error << " (exit-code=" << sample.exit_code << ")" << std::endl;
        }
        return false;
    }

    extra_perf_values_ = sample.extra_values;

    const double bw_gb_s = calculate_bandwidth_gbps(
        sample.cas_rd, sample.cas_wr, sample.elapsed_s,
        counter_selection_.type, cached_cache_line_size_, cached_scaling_factor_);

    if (callback) {
        callback(sample.elapsed_s, bw_gb_s, sample.cas_rd, sample.cas_wr, sample.extra_values);
    }
    return true;
}
