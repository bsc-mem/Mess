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

#include "measurement/bw_measurers/AMDuProfBandwidthMeasurer.h"

#include "measurement/BandwidthStabilizer.h"
#include "measurement/UprofParser.h"
#include "measurement/UprofCollection.h"
#include "measurement/BandwidthSampleAggregation.h"
#include "measurement/TheoreticalPeakCalculator.h"
#include "platform/Fs.h"
#include "platform/Process.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <system_error>
#include <thread>


namespace {

using measurement::uprof::kUprofMinDurationSeconds;
using measurement::uprof::kUprofReportIntervalMs;
using measurement::uprof::uprof_collection_seconds;
using measurement::uprof::run_collection;
using measurement::uprof::UprofSample;
using measurement::uprof::parse_cumulative;
using measurement::uprof::parse_timeseries;
using measurement::uprof::count_df_sections;

}  // namespace


std::string AMDuProfBandwidthMeasurer::find_uprof_binary() const {
    if (const auto found = platform::which("AMDuProfPcm")) {
        return found->string();
    }

    std::vector<std::filesystem::path> roots;
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        if (const char* v = std::getenv(var)) roots.emplace_back(std::filesystem::path(v) / "AMD");
    }

    std::error_code ec;
    for (const auto& root : roots) {
        const std::filesystem::path direct = root / "AMDuProf" / "bin" / platform::exe_name("AMDuProfPcm");
        if (std::filesystem::exists(direct, ec)) return direct.string();
    }

    // Newest by write time, not by name: a name sort puts 5.1 above 10.0.
    std::filesystem::path best;
    std::filesystem::file_time_type best_time{};
    for (const auto& root : roots) {
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_directory()) continue;
            if (e.path().filename().string().rfind("AMDuProf", 0) != 0) continue;
            const std::filesystem::path cand = e.path() / "bin" / platform::exe_name("AMDuProfPcm");
            if (!std::filesystem::exists(cand, ec)) continue;
            const auto t = std::filesystem::last_write_time(cand, ec);
            if (ec) continue;
            if (best.empty() || t > best_time) { best = cand; best_time = t; }
        }
    }
    return best.empty() ? std::string{} : best.string();
}


bool AMDuProfBandwidthMeasurer::counters_available(std::string& reason) {
    // Cached: this costs a real one-second collection.
    static bool  cached      = false;
    static bool  cached_ok   = false;
    static std::string cached_reason;

    if (cached) {
        reason = cached_reason;
        return cached_ok;
    }
    cached = true;

    BenchmarkConfig      dummy_config{};
    system_info          dummy_info{};
    CPUCapabilities      dummy_caps{};
    AMDuProfBandwidthMeasurer probe(dummy_config, dummy_info, dummy_caps,
                                    nullptr, nullptr, nullptr);

    const std::string uprof = probe.find_uprof_binary();
    if (uprof.empty()) {
        cached_reason =
            "AMDuProfPcm was not found. Install AMD uProf (it ships a signed "
            "driver) and ensure AMDuProfPcm is in PATH.";
        reason = cached_reason;
        return false;
    }

    std::string err;
    const std::string csv = run_collection(uprof, kUprofMinDurationSeconds,
                                           /*cumulative=*/true, 0, err);
    if (count_df_sections(csv) > 1) {
        cached_reason = "multiple Data Fabric blocks are unsupported; refusing partial-socket bandwidth";
        reason = cached_reason;
        return false;
    }
    const UprofSample s = parse_cumulative(csv);

    if (!s.valid) {
        cached_reason = err.empty()
            ? "AMDuProfPcm ran but reported no Data Fabric counters. This is "
              "normally either a missing Administrator token or a hypervisor "
              "hiding the uncore counters (Windows 11 enables VBS by default)."
            : ("AMDuProfPcm could not read the Data Fabric counters: " + err);
        reason = cached_reason;
        return false;
    }


    cached_ok = true;
    reason.clear();
    return true;
}


// Inverts calculate_bandwidth_gbps() with the same cached values, so the figure is not scaled
// twice.
static long long gbps_to_cas(double gbps, double window_s, int line_size, double scaling) {
    const double line = (line_size > 0) ? line_size : 64.0;
    const double sf   = (scaling > 0.0) ? scaling : 1.0;
    return static_cast<long long>(std::llround(gbps * 1e9 * window_s / (line * sf)));
}

bool AMDuProfBandwidthMeasurer::sample_bandwidth(long long& cas_rd,
                                                 long long& cas_wr,
                                                 double& elapsed,
                                                 const std::vector<int>& mem_nodes) const {
    // Data Fabric counters cover the whole package, so there is nothing per-node to select.
    (void)mem_nodes;

    const std::string uprof = find_uprof_binary();
    if (uprof.empty()) return false;

    // -d is whole seconds, and one second measures nothing. See kUprofMinDurationSeconds.
    const double requested_s = std::max(1.0, sampling_interval_ms_) / 1000.0;
    const int duration_s = uprof_collection_seconds(
        static_cast<int>(std::lround(requested_s)));

    std::string err;
    const std::string csv = run_collection(uprof, duration_s, /*cumulative=*/true, 0, err);
    const UprofSample s = parse_cumulative(csv);
    if (!s.valid) return false;

    ensure_scaling_factor_cached();
    elapsed = static_cast<double>(duration_s);
    cas_rd = gbps_to_cas(s.read_gbps,  elapsed, cached_cache_line_size_, cached_scaling_factor_);
    cas_wr = gbps_to_cas(s.write_gbps, elapsed, cached_cache_line_size_, cached_scaling_factor_);
    return true;
}


bool AMDuProfBandwidthMeasurer::wait_for_stabilization(int& samples_taken,
                                                       long long& last_cas_rd,
                                                       long long& last_cas_wr,
                                                       double& last_elapsed,
                                                       int pause,
                                                       int ratio,
                                                       bool fast_resume,
                                                       std::function<void()> on_sample) {
    extra_perf_values_.clear();
    samples_taken = 0;

    const std::string uprof = find_uprof_binary();
    if (uprof.empty()) {
        std::cerr << "ERROR: AMDuProfPcm is not available." << std::endl;
        return false;
    }

    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;

    // uProf reports once a second at best, so Mess's 50 ms default cannot be honoured.
    constexpr double kMinIntervalMs = static_cast<double>(kUprofReportIntervalMs);
    double interval_ms = sampling_interval_ms_;
    if (interval_ms < kMinIntervalMs) {
        if (verbose2) {
            std::cout << "    Notice: raising the sampling interval from " << interval_ms
                      << " ms to " << kMinIntervalMs
                      << " ms - AMDuProfPcm reports no faster than that." << '\n';
        }
        interval_ms = kMinIntervalMs;
    }

    auto relaunch_current_traffic_gen = [&]() -> bool {
        if (!this->relaunch_traffic_gen(ratio, pause, get_traffic_gen_cores())) {
            return false;
        }
        return true;
    };

    int relaunch_attempts = 0;
    const platform::ProcessId traffic_gen_pid = traffic_gen_manager_->active_traffic_gen_pid();
    if (!traffic_gen_manager_->is_traffic_gen_running(traffic_gen_pid)) {
        if (verbose2) {
            std::cout << "    TrafficGen not running, relaunching..." << '\n';
        }
        if (!relaunch_current_traffic_gen()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        relaunch_attempts++;
    }

    const int expected_cores = get_traffic_gen_cores();
    const int socket_cores = (sys_info_.socket_count > 0) ? sys_info_.sockets[0].core_count : 0;
    const double theoretical_peak_gb_s =
        TheoreticalPeakCalculator::calculate_achievable_peak(caps_, expected_cores,
                                                             socket_cores, false);

    TrafficGenHealthChecker health_checker;
    health_checker.is_pid_alive = [this](int pid) -> bool {
        return traffic_gen_manager_->is_traffic_gen_running(pid);
    };
    health_checker.count_running_instances = [this]() -> int {
        return static_cast<int>(traffic_gen_manager_->traffic_gen_worker_pids().size());
    };
    health_checker.expected_instance_count = expected_cores;

    ensure_scaling_factor_cached();
    const int    cache_line_size = cached_cache_line_size_;
    const double scaling_factor  = cached_scaling_factor_;

    BandwidthStabilizer stabilizer(theoretical_peak_gb_s, pause, health_checker,
                                   fast_resume ? 5 : 7, 0.05, config_.verbosity);

    // The CSV appears only when the process exits, so samples arrive a batch at a time.
    const int samples_per_batch = (fast_resume ? 5 : 7) + 3;
    const int batch_duration_s = std::max(kUprofMinDurationSeconds,
                                          samples_per_batch + 1);

    const int OVERALL_TIMEOUT_SECONDS = 180;
    const auto loop_start = std::chrono::steady_clock::now();

    long long final_cas_rd = 0, final_cas_wr = 0;
    double    final_elapsed = interval_ms / 1000.0;
    bool      success = false;
    int       total_samples = 0;
    int       zero_rows = 0;

    // Every exit must leave the caller's values defined, or a re-check sample is recorded as
    // the measurement.
    auto publish = [&]() {
        last_cas_rd  = final_cas_rd;
        last_cas_wr  = final_cas_wr;
        last_elapsed = final_elapsed;
        samples_taken = total_samples;
    };

    while (true) {
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - loop_start).count() > OVERALL_TIMEOUT_SECONDS) {
            std::cerr << "\n    ERROR: uProf stabilization timed out after "
                      << OVERALL_TIMEOUT_SECONDS << " seconds." << '\n';
            traffic_gen_manager_->kill_all_traffic_gen();
            publish();
            return false;
        }

        std::string err;
        const std::string csv = run_collection(uprof, batch_duration_s,
                                               /*cumulative=*/false,
                                               static_cast<int>(interval_ms), err);
        std::vector<UprofSample> rows = parse_timeseries(csv);

        if (rows.empty()) {
            std::cerr << "\n    ERROR: AMDuProfPcm returned no Data Fabric samples."
                      << (err.empty() ? "" : (" " + err)) << '\n';
            publish();
            return false;
        }

        // uProf drops its own partial leading interval, so nothing is discarded here.

        // Check worker health after each blocking collection.
        if (!traffic_gen_manager_->is_traffic_gen_running(
                traffic_gen_manager_->active_traffic_gen_pid())) {
            if (relaunch_attempts < 2 && relaunch_current_traffic_gen()) {
                relaunch_attempts++;
                stabilizer.reset();
                continue;
            }
            std::cerr << "\n    ERROR: TrafficGen is no longer running; "
                         "abandoning this point." << '\n';
            publish();
            return false;
        }

        // All zeros while the generators are alive is a tool problem, not a measurement.
        bool batch_all_zero = true;
        for (const auto& row : rows) {
            if (row.read_gbps + row.write_gbps > 0.0) { batch_all_zero = false; break; }
        }
        if (batch_all_zero) {
            zero_rows += static_cast<int>(rows.size());
            if (zero_rows == static_cast<int>(rows.size())) {
                std::cerr << "\n    WARNING: AMDuProfPcm reported 0.00 GB/s across "
                          << rows.size() << " intervals while TrafficGen is running. "
                             "The Data Fabric counters are readable but returning "
                             "nothing - check that this uProf build reports UMC "
                             "bandwidth for this processor." << '\n';
            }
        }

        const double window_s = interval_ms / 1000.0;

        for (const auto& row : rows) {
            const long long cas_rd = gbps_to_cas(row.read_gbps,  window_s,
                                                 cache_line_size, scaling_factor);
            const long long cas_wr = gbps_to_cas(row.write_gbps, window_s,
                                                 cache_line_size, scaling_factor);

            total_samples++;
            if (on_sample) on_sample();

            final_cas_rd  = cas_rd;
            final_cas_wr  = cas_wr;
            final_elapsed = window_s;

            const double bw_gb_s = row.read_gbps + row.write_gbps;

            const StabilizationResult result = stabilizer.add_sample(cas_rd, cas_wr, bw_gb_s);

            if (result == StabilizationResult::ZOMBIE_DETECTED) {
                if (verbose2) {
                    std::cout << "    [ZOMBIE] TrafficGen died, aborting stabilization" << '\n';
                }
                publish();
                return false;
            }

            if (verbose3) {
                const long long total_cas = cas_rd + cas_wr;
                const double read_ratio = (total_cas > 0)
                    ? static_cast<double>(cas_rd) / static_cast<double>(total_cas)
                    : 0.0;
                std::cout << "      [Sample " << total_samples << "] CAS: " << total_cas;
                stabilizer.print_status(read_ratio, bw_gb_s, true);
            }

            if (stabilizer.is_stable()) { success = true; break; }
        }

        if (success) break;
    }

    if (success) {
        const auto& samples = stabilizer.get_samples();
        const auto totals = measurement::aggregate_cas_samples(samples);
        last_cas_rd  = totals.read;
        last_cas_wr  = totals.write;
        last_elapsed = samples.size() * (interval_ms / 1000.0);

        if (verbose3) {
            std::cout << "      [BW AGGREGATE] Using " << samples.size()
                      << " stable samples (" << last_elapsed << "s total)" << '\n';
        }
        samples_taken = total_samples;
        return true;
    }

    publish();
    return false;
}


// The counters are package-wide, so nothing attaches to the child.
bool AMDuProfBandwidthMeasurer::monitor_command(const std::string& command,
                                                MonitorCallback callback,
                                                bool summary_mode) {
    const std::string uprof = find_uprof_binary();
    if (uprof.empty()) return false;

    auto child = platform::spawn_shell(command);
    if (!child) return false;

    ensure_scaling_factor_cached();
    const int    line = cached_cache_line_size_;
    const double sf   = cached_scaling_factor_;

    const int duration_s = uprof_collection_seconds(
        static_cast<int>(std::lround(std::max(1.0, sampling_interval_ms_) / 1000.0)));

    const auto start = std::chrono::steady_clock::now();
    double total_rd_gb = 0.0, total_wr_gb = 0.0;

    while (child->running()) {
        std::string err;
        const std::string csv = run_collection(uprof, duration_s, /*cumulative=*/true, 0, err);
        const UprofSample s = parse_cumulative(csv);
        if (!s.valid) {
            std::cerr << "Error: uProf monitoring failed: "
                      << (err.empty() ? "invalid Data Fabric report" : err) << '\n';
            return false;
        }

        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();

        total_rd_gb += s.read_gbps  * duration_s;
        total_wr_gb += s.write_gbps * duration_s;

        if (!summary_mode && callback) {
            callback(now, s.read_gbps + s.write_gbps,
                     gbps_to_cas(s.read_gbps,  duration_s, line, sf),
                     gbps_to_cas(s.write_gbps, duration_s, line, sf),
                     MonitorSampleExtras{});
        }
    }

    if (summary_mode && callback) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const double bw = (elapsed > 0.0) ? (total_rd_gb + total_wr_gb) / elapsed : 0.0;
        callback(elapsed, bw,
                 gbps_to_cas(total_rd_gb / std::max(1.0, elapsed), elapsed, line, sf),
                 gbps_to_cas(total_wr_gb / std::max(1.0, elapsed), elapsed, line, sf),
                 MonitorSampleExtras{});
    }
    return true;
}
