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

#include "measurement/bw_measurers/PerfBandwidthMeasurer.h"
#include "platform/Console.h"   // platform::glyph
#include "measurement/BandwidthSampleAggregation.h"
#include "measurement/BandwidthStabilizer.h"
#include "measurement/PerfStatCsv.h"
#include "architecture/ArchitectureRegistry.h"
#include "architecture/BandwidthCounterStrategy.h"
#include "SystemDetection.h"
#include "Utils.h"
#include "utils/CpuTopology.h"
#include "utils/SubprocessCapture.h"
#include "utils/StringUtils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <deque>
#include <iomanip>
#include <chrono>
#include <map>
#include <cerrno>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

namespace {
bool use_per_socket_aggregation(const BandwidthCounterSelection& selection) {
    if (selection.type != CounterType::CAS_COUNT &&
        selection.type != CounterType::NVIDIA_GRACE &&
        selection.type != CounterType::TOTAL_BYTES) {
        return true;
    }
    return !selection.cas.uses_read_subtract_formula;
}

std::pair<std::string, std::string> detect_unc_m_aggregate_events() {
    static std::pair<std::string, std::string> cached;
    static bool initialized = false;
    if (initialized) {
        return cached;
    }
    initialized = true;

    // perf list can be hundreds of KiB on big-iron — cap accordingly.
    subprocess::Options opts;
    opts.timeout          = std::chrono::seconds(10);
    opts.max_output_bytes = 262144;
    const std::string output = subprocess::run_shell("perf list 2>/dev/null", opts);

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream ls(line);
        std::string token;
        ls >> token;
        if (token.empty()) {
            continue;
        }
        if (string_utils::to_lower(token) == "unc_m_cas_count.rd") {
            cached.first = "unc_m_cas_count.rd";
        } else if (string_utils::to_lower(token) == "unc_m_cas_count.wr") {
            cached.second = "unc_m_cas_count.wr";
        }
    }

    return cached;
}

std::string build_preferred_perf_events(const BandwidthCounterSelection& selection) {
    std::string events = selection.get_all_events_string();

    if (selection.type != CounterType::CAS_COUNT || selection.cas.uses_read_subtract_formula) {
        return events;
    }

    if (events.find("uncore_type_14_") != std::string::npos ||
        events.find("uncore_hbm") != std::string::npos) {
        return events;
    }

    const auto agg = detect_unc_m_aggregate_events();
    if (agg.first.empty() || agg.second.empty()) {
        return events;
    }

    std::string preferred = agg.first + "," + agg.second;
    for (const auto& extra : selection.extra_counters) {
        if (!extra.empty()) {
            preferred += ",";
            preferred += extra;
        }
    }
    return preferred;
}

std::vector<std::string> counted_socket_scopes(const BandwidthCounterSelection& selection, const std::vector<int>& mem_nodes) {
    std::vector<std::string> scopes;
    if (selection.type != CounterType::CAS_COUNT) return scopes;
    for (int node : mem_nodes) {
        const int socket = cpu_topology::socket_of_memory_node(node);
        if (socket < 0) return {};
        const std::string scope = "S" + std::to_string(socket);
        if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) scopes.push_back(scope);
    }
    return scopes;
}

bool skip_socket_row(const std::string& scope, const std::vector<std::string>& counted_scopes) {
    if (counted_scopes.empty()) return scope == "S1";
    return !scope.empty() && std::find(counted_scopes.begin(), counted_scopes.end(), scope) == counted_scopes.end();
}

bool use_pid_attach_mode(const BandwidthCounterSelection& selection) {
    return selection.cas.uses_read_subtract_formula;
}

std::string join_pid_list(const std::vector<platform::ProcessId>& pids) {
    std::string joined;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i > 0) {
            joined += ",";
        }
        joined += std::to_string(pids[i]);
    }
    return joined;
}

bool is_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

std::string build_perf_scope_args(const BandwidthCounterSelection& selection,
                                  const TrafficGenProcessManager* traffic_gen_manager) {
    if (!use_pid_attach_mode(selection)) {
        return use_per_socket_aggregation(selection) ? "-a --per-socket" : "-a";
    }

    if (!traffic_gen_manager) {
        return "";
    }

    const std::vector<platform::ProcessId> worker_pids = traffic_gen_manager->traffic_gen_worker_pids();
    if (worker_pids.empty()) {
        return "";
    }

    return "-p " + join_pid_list(worker_pids);
}

void apply_bandwidth_formula(const BandwidthCounterSelection& selection,
                             long long raw_rd,
                             long long raw_wr,
                             const std::map<std::string, long long>& extras,
                             long long& effective_rd,
                             long long& effective_wr) {
    effective_rd = raw_rd;
    effective_wr = raw_wr;

    if (!selection.cas.uses_read_subtract_formula) {
        return;
    }

    long long subtract = 0;
    for (const auto& event : selection.cas.read_subtract_events) {
        auto it = extras.find(event);
        if (it != extras.end()) {
            subtract += it->second;
        }
    }

    effective_rd = std::max(0LL, raw_rd - subtract);
}
}

bool PerfBandwidthMeasurer::sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed, const std::vector<int>& mem_nodes) const {
    return sample_with_popen(cas_rd, cas_wr, elapsed, mem_nodes);
}

bool PerfBandwidthMeasurer::sample_with_popen(long long& cas_rd, long long& cas_wr, double& elapsed, const std::vector<int>& mem_nodes) const {
    const bool verbose4 = config_.verbosity >= 4;
    extra_perf_values_.clear();
    const int cache_line_size = std::max(1, cached_cache_line_size_);

    int interval = static_cast<int>(sampling_interval_ms_);

    const BandwidthCounterSelection& selection = counter_selection_;

    if (!selection.is_valid()) {
        std::cerr << "ERROR: Counter selection not initialized. "
                  << "BandwidthCounterStrategy must be initialized first." << std::endl;
        if (!selection.failure_reason.empty()) {
            std::cerr << "Reason: " << selection.failure_reason << std::endl;
        }
        return false;
    }

    std::string events_str = build_preferred_perf_events(selection);
    const std::vector<std::string> counted_scopes = counted_socket_scopes(selection, mem_nodes);
    const bool attach_mode = use_pid_attach_mode(selection);
    const std::string scope_args = build_perf_scope_args(selection, traffic_gen_manager_);
    if (scope_args.empty()) {
        if (attach_mode) {
            std::cerr << "ERROR: No TrafficGen worker PIDs available for A64FX perf sampling." << std::endl;
        }
        return false;
    }

    ensure_a64fx_pmu_available();

    std::string cmd = std::string("cd ") + string_utils::shell_quote(storage_->bandwidth_dir()) + " && export LC_ALL=C && echo \"__PID__$$\" && exec stdbuf -oL -eL ";
    // For system-wide perf stat (-a/--per-socket), avoid numactl wrapping.
    // Some Emerald Rapids/CXL setups can hang with:
    //   numactl ... perf stat -a --per-socket ...
    cmd += "perf stat -I " + std::to_string(interval) +
           " -x, " + scope_args + " -e " + events_str + " 2>&1";

    if(verbose4){std::cout << "[SAMPLING CMD] " << cmd << '\n';}

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return false;
    }

    setbuf(pipe, NULL);

    pid_t perf_stat_pid = -1;
    char pid_buf[256];
    if (fgets(pid_buf, sizeof(pid_buf), pipe)) {
        const char* marker = strstr(pid_buf, "__PID__");
        if (marker) {
            perf_stat_pid = static_cast<pid_t>(atol(marker + 7));
        }
    }

    if (verbose4) {
        std::cout << "      [PERF DEBUG] attach_mode=" << (attach_mode ? "yes" : "no")
                  << ", scope=\"" << scope_args << "\""
                  << ", perf_pid=" << perf_stat_pid << '\n';
        std::cout << "      [PERF DEBUG] command: " << cmd << '\n';
    }

    char buffer[4096];
    long long current_sample_rd = 0, current_sample_wr = 0, current_sample_combined = 0;
    std::map<std::string, long long> current_extra;
    std::map<std::string, long long> sum_extra;
    long long sum_rd = 0, sum_wr = 0, sum_combined = 0;
    double last_timestamp = -1.0;
    double previous_interval_end = 0.0;
    double accumulated_time = 0.0;
    int samples_read = 0;
    int total_samples_seen = 0;
    const int samples_to_read = 2;
    const int warmup_samples = 1;
    bool using_combined = selection.cas.has_combined_counter && !selection.cas.has_read_write;

    while (samples_read < samples_to_read && fgets(buffer, sizeof(buffer), pipe)) {
        if (buffer[0] == '#' || strstr(buffer, "time") || strlen(buffer) < 5) {
            continue;
        }

        perf_stat_csv::Row row;
        if (perf_stat_csv::parse_row(buffer, true, row)) {
            if (skip_socket_row(row.scope, counted_scopes)) {
                continue;
            }
            double timestamp = row.timestamp;
            long long value = perf_stat_csv::normalize_row_value(row, cache_line_size);
            const std::string& event_name = row.event_name;

            if (last_timestamp >= 0 && timestamp != last_timestamp) {
                long long effective_rd = 0;
                long long effective_wr = 0;
                apply_bandwidth_formula(selection, current_sample_rd, current_sample_wr, current_extra, effective_rd, effective_wr);
                double duration = last_timestamp - previous_interval_end;
                previous_interval_end = last_timestamp;
                if (duration < 0.000001) duration = interval / 1000.0;

                total_samples_seen++;
                bool sample_valid = false;
                if (using_combined) {
                    sample_valid = (current_sample_combined > 0);
                } else {
                    sample_valid = (effective_rd + effective_wr > 0);
                }
                if (sample_valid && total_samples_seen > warmup_samples) {
                    sum_rd += effective_rd;
                    sum_wr += effective_wr;
                    sum_combined += current_sample_combined;
                    for (const auto& kv : current_extra) {
                        sum_extra[kv.first] += kv.second;
                    }    
                    accumulated_time += duration;
                    samples_read++;
                }
                current_sample_rd = 0;
                current_sample_wr = 0;
                current_sample_combined = 0;
                EventClassification::reset_extra_values(current_extra);
                if (samples_read >= samples_to_read) {
                    break;
                }
            }

            EventClassification ec = EventClassification::classify(event_name, selection);

            if (verbose4) {
                std::cout << "      [SAMPPLING EVENT] " << event_name << " -> rd:" << ec.is_read 
                          << " wr:" << ec.is_write << " comb:" << ec.is_combined << " extra: " << ec.is_extra
                          << " val:" << value << '\n';
            }

            if (ec.is_combined) {
                current_sample_combined += value;
            } else if (ec.is_read) {
                current_sample_rd += value;
            } else if (ec.is_write) {
                current_sample_wr += value;
            } else if (ec.is_extra) {
                EventClassification::accumulate_extra(current_extra, ec.extra_key, value);
            }
            last_timestamp = timestamp;
        }
    }

    long long effective_rd = 0;
    long long effective_wr = 0;
    apply_bandwidth_formula(selection, current_sample_rd, current_sample_wr, current_extra, effective_rd, effective_wr);
    bool sample_valid = using_combined 
        ? (current_sample_combined > 0) 
        : (effective_rd + effective_wr > 0);
    if (samples_read < samples_to_read && sample_valid) {
        sum_rd += effective_rd;
        sum_wr += effective_wr;
        sum_combined += current_sample_combined;
        for (const auto& kv : current_extra) {
            sum_extra[kv.first] += kv.second;
        }
        samples_read++;
    }

    if (perf_stat_pid > 0) {
        kill(perf_stat_pid, SIGTERM);
    }
    pclose(pipe);

    bool success = false;
    if (using_combined) {
        success = (samples_read > 0 && sum_combined > 0);
    } else {
        success = (samples_read > 0 && sum_rd > 0 && sum_wr > 0);
    }

    if (success) {
        if (using_combined) {
            cas_rd = sum_combined;
            cas_wr = 0;
        } else {
            cas_rd = sum_rd;
            cas_wr = sum_wr;
        }
        elapsed = accumulated_time;
        for (const auto& kv : sum_extra) {
            extra_perf_values_[kv.first] = kv.second;
        }
        return true;
    }

    return false;
}

bool PerfBandwidthMeasurer::wait_for_stabilization(int& samples_taken, long long& last_cas_rd, long long& last_cas_wr, double& last_elapsed, int pause, int ratio, bool fast_resume, std::function<void()> on_sample) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    extra_perf_values_.clear();
    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;
    const bool verbose4 = config_.verbosity >= 4;

    int interval = static_cast<int>(sampling_interval_ms_);
    std::vector<int> mem_nodes = numa_resolver_ ? numa_resolver_() : std::vector<int>{0};

    int relaunch_attempts = 0;
    if (!ensure_traffic_gen_running(ratio, pause, relaunch_attempts)) {
        return false;
    }

    bool is_remote = (counter_selection_.type == CounterType::UPI_FLITS);
    double theoretical_peak_gb_s = achievable_peak_gbps(is_remote);

    if (verbose3) {
        double full_peak = TheoreticalPeakCalculator::calculate_from_capabilities(caps_, is_remote);
        auto fmt_gb = [](double v) -> std::string {
            if (v <= 0.0) return std::string("unknown");
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << v << " GB/s";
            return oss.str();
        };
        std::cout << "      [Stabilizer] Theoretical peak: " << fmt_gb(full_peak)
                  << ", Achievable (" << get_traffic_gen_cores() << " cores): " << fmt_gb(theoretical_peak_gb_s)
                  << ", Noise floor: "
                  << std::fixed << std::setprecision(2)
                  << TheoreticalPeakCalculator::get_noise_floor(theoretical_peak_gb_s)
                  << " GB/s" << '\n';
    }
    const TrafficGenHealthChecker health_checker = traffic_gen_health();

    const BandwidthCounterSelection& selection = counter_selection_;
    if (!selection.is_valid()) {
        std::cerr << "ERROR: Counter selection not initialized. "
                  << "BandwidthCounterStrategy must be initialized first." << std::endl;
        return false;
    }
    std::string events_str = build_preferred_perf_events(selection);
    const std::vector<std::string> counted_scopes = counted_socket_scopes(selection, mem_nodes);
    const bool attach_mode = use_pid_attach_mode(selection);
    const std::string scope_args = build_perf_scope_args(selection, traffic_gen_manager_);
    if (scope_args.empty()) {
        if (attach_mode) {
            std::cerr << "ERROR: No TrafficGen worker PIDs available for A64FX perf sampling." << std::endl;
        }
        return false;
    }

    ensure_a64fx_pmu_available();

    std::string cmd = std::string("cd ") + string_utils::shell_quote(storage_->bandwidth_dir()) + " && export LC_ALL=C && echo \"__PID__$$\" && exec stdbuf -oL -eL ";
    // For system-wide perf stat (-a/--per-socket), avoid numactl wrapping.
    // Some Emerald Rapids/CXL setups can hang with:
    //   numactl ... perf stat -a --per-socket ...
    cmd += "perf stat -I " + std::to_string(interval) +
           " -x, " + scope_args + " -e " + events_str + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        if (verbose2) {
            std::cout << "    Warning: perf stat failed, falling back to configured wait time" << '\n';
        }
        return false;
    }

    setbuf(pipe, NULL);
    int pipe_fd = fileno(pipe);

    pid_t perf_stat_pid = -1;
    char pid_buf[256];
    if (fgets(pid_buf, sizeof(pid_buf), pipe)) {
        const char* marker = strstr(pid_buf, "__PID__");
        if (marker) {
            perf_stat_pid = static_cast<pid_t>(atol(marker + 7));
        }
    }

    auto kill_and_pclose = [&]() {
        if (perf_stat_pid > 0) {
            kill(perf_stat_pid, SIGTERM);
        }
        pclose(pipe);
        pipe = nullptr;
    };

    BandwidthStabilizer stabilizer = make_stabilizer(theoretical_peak_gb_s, pause, fast_resume ? 5 : 7);
    
    int total_samples = 0;
    const int OVERALL_TIMEOUT_SECONDS = 60;
    int max_samples = static_cast<int>((OVERALL_TIMEOUT_SECONDS * 1000.0) / sampling_interval_ms_);
    
    char buffer[4096];
    bool success = false;
    
    long long sample_cas_rd = 0, sample_cas_wr = 0;
    std::map<std::string, long long> sample_extra;
    std::map<std::string, long long> aggregated_extra;
    std::map<std::string, long long> final_extra;
    double last_timestamp = -1.0;
    double previous_interval_end = 0.0;

    long long final_cas_rd = 0, final_cas_wr = 0;
    double final_elapsed = sampling_interval_ms_ / 1000.0;

    auto loop_start = std::chrono::steady_clock::now();
    const int READ_TIMEOUT_SECONDS = 10;
    int consecutive_timeouts = 0;
    long long last_heartbeat_s = -1;
    int parse_failures = 0;

    while (total_samples < max_samples) {
        auto elapsed_total = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - loop_start).count();
        if (verbose4 && (last_heartbeat_s < 0 || elapsed_total - last_heartbeat_s >= 5)) {
            last_heartbeat_s = elapsed_total;
            std::cout << "      [PERF DEBUG] heartbeat t=" << elapsed_total
                      << "s, samples=" << total_samples
                      << ", timeouts=" << consecutive_timeouts
                      << ", perf_alive=" << (is_pid_alive(perf_stat_pid) ? "yes" : "no")
                      << ", tg_alive="
                      << (traffic_gen_manager_->is_traffic_gen_running(traffic_gen_manager_->active_traffic_gen_pid()) ? "yes" : "no")
                      << '\n';
        }
        
        if (elapsed_total > OVERALL_TIMEOUT_SECONDS) {
            std::cerr << "\n    ERROR: Perf stabilization timed out after " << OVERALL_TIMEOUT_SECONDS << " seconds." << '\n';
            if (verbose3) {
                std::cerr << "    Possible causes:\n"
                          << "      - perf stat command hung\n"
                          << "      - Performance counters unavailable\n"
                          << "      - System overloaded\n"
                          << "    Command: " << cmd << '\n';
            }
            kill_and_pclose();
            traffic_gen_manager_->kill_all_traffic_gen();
            return false;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(pipe_fd, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = READ_TIMEOUT_SECONDS;
        timeout.tv_usec = 0;
        
        int ready = select(pipe_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready < 0) {
            std::cerr << "    ERROR: select() failed on perf pipe" << '\n';
            kill_and_pclose();
            return false;
        }
        
        if (ready == 0) {
            consecutive_timeouts++;
            if (verbose2) {
                std::cout << "    Warning: No perf output for " << READ_TIMEOUT_SECONDS << " seconds (attempt " 
                          << consecutive_timeouts << "/3)" << '\n';
            }
            
            if (consecutive_timeouts >= 3) {
                std::cerr << "\n    ERROR: Perf not producing output." << '\n';
                if (verbose3) {
                    std::cerr << "    Possible causes:\n"
                              << "      - perf_event_paranoid too restrictive\n"
                              << "      - Uncore counters require root access\n"
                              << "      - perf stat command syntax error\n"
                              << "    Command: " << cmd << '\n';
                }
                kill_and_pclose();
                traffic_gen_manager_->kill_all_traffic_gen();
                return false;
            }
            continue;
        }
        
        consecutive_timeouts = 0;
        
        if (!fgets(buffer, sizeof(buffer), pipe)) {
            if (verbose4) {
                std::cout << "      [PERF DEBUG] fgets returned EOF/NULL from perf pipe" << '\n';
            }
            break;
        }
        if (buffer[0] == '#' || strstr(buffer, "time") || strlen(buffer) < 5) {
            if (verbose4) {
                std::string line(buffer);
                if (line.find("not supported") != std::string::npos ||
                    line.find("No permission") != std::string::npos ||
                    line.find("failed") != std::string::npos ||
                    line.find("Error") != std::string::npos) {
                    std::cout << "      [PERF DEBUG] diagnostic line: " << line;
                }
            }
            continue;
        }

        perf_stat_csv::Row row;
        const bool parsed = perf_stat_csv::parse_row(buffer, true, row);
        
        if (verbose4) {
            std::cout << "      [PERF RAW] parsed=" << parsed << " line: " << buffer;
        }
        if (!parsed && verbose4 && parse_failures < 20) {
            ++parse_failures;
            std::cout << "      [PERF DEBUG] parse-fail[" << parse_failures << "]: " << buffer;
        }
        
        if (parsed) {
            if (skip_socket_row(row.scope, counted_scopes)) {
                continue;
            }
            double timestamp = row.timestamp;
            long long value = perf_stat_csv::normalize_row_value(row, std::max(1, cached_cache_line_size_));
            const std::string& event_name = row.event_name;

            if (last_timestamp >= 0 && timestamp != last_timestamp) {
                long long effective_rd = 0;
                long long effective_wr = 0;
                apply_bandwidth_formula(selection, sample_cas_rd, sample_cas_wr, sample_extra, effective_rd, effective_wr);
                double duration = last_timestamp - previous_interval_end;
                previous_interval_end = last_timestamp;
                double target_duration = sampling_interval_ms_ / 1000.0;
                double normalization_factor = 1.0;
                
                if (duration > 0.000001) {
                    normalization_factor = target_duration / duration;
                }

                if (effective_rd + effective_wr > 0) {
                    long long norm_rd = static_cast<long long>(effective_rd * normalization_factor);
                    long long norm_wr = static_cast<long long>(effective_wr * normalization_factor);
                    long long total_cas = norm_rd + norm_wr;
                    long long total_cas_reg = effective_rd + effective_wr;
                    
                    total_samples++;

                    if (on_sample) {
                        on_sample();
                    }

                    if (total_samples % 20 == 0 &&
                        !traffic_gen_manager_->is_traffic_gen_running(traffic_gen_manager_->active_traffic_gen_pid())) {
                        if (relaunch_attempts < 2) {
                            if (verbose2) {
                                std::cout << "    TrafficGen died during stabilization, relaunching..." << '\n';
                            }
                            if (relaunch_traffic_gen(ratio, pause)) {
                                relaunch_attempts++;
                                stabilizer.reset();
                                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                continue;
                            } else {
                                kill_and_pclose();
                                return false;
                            }
                        } else {
                            kill_and_pclose();
                            return false;
                        }
                    }

                    if (total_samples > max_samples) {
                        if (config_.verbosity >= 1) {
                            std::cout << "\n    " << platform::glyph::kWarning << " WARNING: BW did not stabilize after " << OVERALL_TIMEOUT_SECONDS << "s" << '\n';
                            std::cout << "    Killing TrafficGen and triggering retry..." << '\n';
                        }
                        kill_and_pclose();
                        traffic_gen_manager_->kill_all_traffic_gen();
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));

                        samples_taken = total_samples;
                        return false;
                    }

                    final_cas_rd = norm_rd;
                    final_cas_wr = norm_wr;
                    final_elapsed = target_duration;
                    final_extra = sample_extra;
                    for (const auto& kv : sample_extra) {
                        aggregated_extra[kv.first] += kv.second;
                    }

                    double bw_gb_s = bandwidth_gbps(norm_rd, norm_wr, target_duration);
                    StabilizationResult result = stabilizer.add_sample(norm_rd, norm_wr, bw_gb_s);

                    if (result == StabilizationResult::ZOMBIE_DETECTED) {
                        if (verbose2) {
                            std::cout << "    [ZOMBIE] TrafficGen died, aborting stabilization" << '\n';
                        }
                        kill_and_pclose();
                        samples_taken = total_samples;
                        return false;
                    }

                    if (verbose3) {
                        double read_ratio = 0.0;
                        if (selection.type == CounterType::NVIDIA_GRACE) {
                            double read_bytes = static_cast<double>(norm_rd) * 32.0;
                            double write_bytes = static_cast<double>(norm_wr);
                            double total_bytes = read_bytes + write_bytes;
                            if (total_bytes > 0) {
                                read_ratio = read_bytes / total_bytes;
                            }
                        } else {
                            if (total_cas_reg > 0) {
                                read_ratio = static_cast<double>(effective_rd) / static_cast<double>(total_cas_reg);
                            }
                        }
                        
                        std::string counter_label = "CAS";
                        if (selection.type == CounterType::UPI_FLITS) counter_label = "UPI";
                        else if (selection.type == CounterType::NVIDIA_GRACE) counter_label = "GRACE";
                        else if (selection.type == CounterType::TOTAL_BYTES) counter_label = "BYTES";
                        else if (selection.cas.uses_read_subtract_formula) counter_label = "A64FX";
                        
                        bool has_distinct_rw = selection.cas.has_read_write;
                        std::cout << "      [Sample " << total_samples << "] " << counter_label << ": " << total_cas;
                        stabilizer.print_status(read_ratio, bw_gb_s, has_distinct_rw);
                    }

                    if (stabilizer.is_stable()) {
                        success = true;
                        break;
                    }

                    if (total_samples > 3 && total_cas == 0) {
                        if (!health_checker.is_healthy()) {
                            if (config_.verbosity >= 2) {
                                std::cout << "    [ZOMBIE] Zero CAS + unhealthy traffic gen detected" << '\n';
                            }
                            kill_and_pclose();
                            samples_taken = total_samples;
                            return false;
                        }
                    }
                }
                sample_cas_rd = 0;
                sample_cas_wr = 0;
                EventClassification::reset_extra_values(sample_extra);
            }

            EventClassification ec = EventClassification::classify(event_name, selection);

            if (verbose4) {
                std::cout << "      [EVENT] " << event_name << " -> rd:" << ec.is_read 
                          << " wr:" << ec.is_write << " comb:" << ec.is_combined 
                          << " val:" << value << '\n';
            }

            if (ec.is_extra) {
                EventClassification::accumulate_extra(sample_extra, ec.extra_key, value);
            } else if (ec.is_combined || ec.is_read) {
                sample_cas_rd += value;
            } else if (ec.is_write) {
                sample_cas_wr += value;
            }

            last_timestamp = timestamp;
        }
    }

    kill_and_pclose();

    if (success) {
        const auto& samples = stabilizer.get_samples();
        const auto totals = measurement::aggregate_cas_samples(samples);
        last_cas_rd = totals.read;
        last_cas_wr = totals.write;
        last_elapsed = samples.size() * (sampling_interval_ms_ / 1000.0);

        if (verbose3) {
             std::cout << "      [BW AGGREGATE] Using " << samples.size()
                       << " stable samples (" << last_elapsed << "s total)" << '\n';
        }
        extra_perf_values_ = aggregated_extra;
        
        samples_taken = total_samples;
        return true;
    } else {
        last_cas_rd = final_cas_rd;
        last_cas_wr = final_cas_wr;
        last_elapsed = final_elapsed;
        extra_perf_values_ = final_extra;
        samples_taken = total_samples;
        return false;
    }
}

bool PerfBandwidthMeasurer::monitor_command(const std::string& command, 
                                            MonitorCallback callback,
                                            bool summary_mode) {
    extra_perf_values_.clear();
    
    std::vector<int> mem_nodes = numa_resolver_ ? numa_resolver_() : std::vector<int>{0};

    const BandwidthCounterSelection& selection = counter_selection_;
    if (!selection.is_valid()) {
        std::cerr << "ERROR: Counter selection not initialized. "
                  << "BandwidthCounterStrategy must be initialized first." << std::endl;
        return false;
    }

    std::string events_str = build_preferred_perf_events(selection);
    const std::vector<std::string> counted_scopes = counted_socket_scopes(selection, mem_nodes);

    std::string trimmed_command = command;
    const auto first_non_ws = trimmed_command.find_first_not_of(" \t\r\n");
    if (first_non_ws == std::string::npos) {
        trimmed_command.clear();
    } else {
        trimmed_command.erase(0, first_non_ws);
    }
    const auto last_non_ws = trimmed_command.find_last_not_of(" \t\r\n");
    if (!trimmed_command.empty() && last_non_ws != std::string::npos) {
        trimmed_command.erase(last_non_ws + 1);
    }
    const bool pid_attach_mode =
        (trimmed_command.rfind("-p ", 0) == 0) ||
        (trimmed_command.rfind("--pid ", 0) == 0);

    std::stringstream perf_cmd;
    perf_cmd << "export LC_ALL=C && perf stat -x, ";
    if (!pid_attach_mode) {
        perf_cmd << "-a ";
        if (use_per_socket_aggregation(selection)) {
            perf_cmd << "--per-socket ";
        }
    }
    
    if (!summary_mode) {
        int interval_ms = static_cast<int>(sampling_interval_ms_);
        perf_cmd << "-I " << interval_ms << " ";
    }
    
    perf_cmd << "-e " << events_str << " ";
    perf_cmd << trimmed_command;
    perf_cmd << " 2>&1";

    ensure_a64fx_pmu_available();

    auto start_time = std::chrono::steady_clock::now();
    FILE* pipe = popen(perf_cmd.str().c_str(), "r");
    if (!pipe) {
        return false;
    }

    char buffer[4096];
    
    double current_timestamp = -1.0;
    long long current_rd = 0;
    long long current_wr = 0;
    std::map<std::string, long long> current_extra;
    std::map<std::string, long long> total_extra;
    bool has_data = false;


    auto process_aggregation = [&](double timestamp, double duration_s) {
        long long effective_rd = 0;
        long long effective_wr = 0;
        apply_bandwidth_formula(selection, current_rd, current_wr, current_extra, effective_rd, effective_wr);
        double bw = calculate_bandwidth_gbps(effective_rd, effective_wr, duration_s,
                                             selection.type, cached_cache_line_size_, cached_scaling_factor_);
        if (callback) {
            callback(timestamp, bw, effective_rd, effective_wr, current_extra);
        }
    };

    while (fgets(buffer, sizeof(buffer), pipe)) {
        if (buffer[0] == '#' || strstr(buffer, "time") || strlen(buffer) < 5) {
            continue;
        }

        perf_stat_csv::Row row;

        if (!summary_mode) {
            if (!perf_stat_csv::parse_row(buffer, true, row)) {
                continue;
            }
            if (skip_socket_row(row.scope, counted_scopes)) {
                continue;
            }
        } else {
            if (!perf_stat_csv::parse_row(buffer, false, row)) {
                continue;
            }
            if (skip_socket_row(row.scope, counted_scopes)) {
                continue;
            }
            row.timestamp = 0.0;
        }

        if (!summary_mode) {
            float timestamp = static_cast<float>(row.timestamp);
            if (std::abs(static_cast<double>(timestamp) - current_timestamp) > 0.0001) {
                if (has_data) {
                    double duration = sampling_interval_ms_ / 1000.0;
                    process_aggregation(current_timestamp, duration);
                }
                current_timestamp = static_cast<double>(timestamp);
                current_rd = 0;
                current_wr = 0;
                EventClassification::reset_extra_values(current_extra);
                has_data = true;
            }
        } else {
            has_data = true;
        }

        EventClassification ec = EventClassification::classify(row.event_name, selection);
        const long long row_value = perf_stat_csv::normalize_row_value(row, std::max(1, cached_cache_line_size_));

        if (ec.is_extra) {
            EventClassification::accumulate_extra(current_extra, ec.extra_key, row_value);
            EventClassification::accumulate_extra(total_extra, ec.extra_key, row_value);
        } else if (ec.is_read || ec.is_combined) {
            current_rd += row_value;
        } else if (ec.is_write) {
            current_wr += row_value;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();

    if (has_data) {
        if (!summary_mode) {
             double duration = sampling_interval_ms_ / 1000.0;
             process_aggregation(current_timestamp, duration);
        } else {
             process_aggregation(0.0, elapsed_seconds);
        }
    }
    
    pclose(pipe);

    for (const auto& kv : total_extra) {
        extra_perf_values_[kv.first] = kv.second;
    }

    return true;
}
