#include "measurement/BandwidthRecheck.h"
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

#include "BenchmarkExecutor.h"
#include "measurement/PointSampling.h"
#include "measurement/AdaptiveSweep.h"
#include "platform/Console.h"   // platform::glyph
#include "measurement/CpuFrequencyProbe.h"
#include "measurement/PointerChaseTiming.h"
#include <array>
#include "Measurement.h"
#include "ProcessManager.h"
#include "architecture/ArchitectureRegistry.h"
#include "Utils.h"
#include "utils/StringUtils.h"
#include "platform/Ipc.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <random>
#include <future>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <set>
#include <iomanip>
#include <vector>
#include <utility>
#include <csignal>
#include <deque>
#include <stdexcept>
#include <system_error>
#include <semaphore.h>
#include <cctype>
#include <cerrno>
#ifdef __linux__
#include <linux/perf_event.h>
#endif
#ifdef __linux__
#include <asm/unistd.h>
#endif
#include "PtrchasePerfHelper.h"
#include "architecture/BandwidthCounterStrategy.h"
#include "measurement/InstructionSamplerFactory.h"
#include "measurement/ResultFormatter.h"

double BenchmarkExecutor::cached_tlb_hit_latency_ns_ = 10.0;

BenchmarkExecutor::BenchmarkExecutor(const BenchmarkConfig& config, const system_info& sys_info, const CPUCapabilities& caps,
                                   double tlb_latency_ns, int cache_line_size)
    : config_(config),
      sys_info_(sys_info),
      caps_(caps),
      bw_bytes_per_cas_(cache_line_size),
      counter_strategy_(&BandwidthCounterStrategy::instance()),
      traffic_gen_manager_(std::make_unique<TrafficGenProcessManager>(config, sys_info)),
      ptrchase_manager_(std::make_unique<PtrChaseProcessManager>(config, sys_info)),
      measurement_storage_multiseq_(config.generate_multiseq
          ? std::make_unique<MeasurementStorage>(config, cache_line_size, config.verbosity, config.output_root + "/multisequential")
          : nullptr),
      bandwidth_measurer_multiseq_(nullptr),
      latency_measurer_multiseq_(config.generate_multiseq
          ? std::make_unique<LatencyMeasurer>(config, measurement_storage_multiseq_.get(), ptrchase_manager_.get(), [this]() { return get_memory_binding_nodes(); }, cache_line_size)
          : nullptr),
      outlier_detector_multiseq_(config.generate_multiseq
          ? std::make_unique<OutlierDetector>(config)
          : nullptr),
      progress_tracker_(std::make_unique<ProgressTracker>(0)) {

    if (config_.instruction_sampling) {
        instruction_sampler_ = InstructionSamplerFactory::create(
            SamplerBackend::AUTO, 50, 0, config_.verbosity);
        if (!instruction_sampler_ && config_.verbosity >= 1) {
            std::cerr << "    Warning: No instruction sampler backend available (PEBS/SPE)\n";
        }
    }

    if (config_.generate_multiseq) {
        bandwidth_measurer_multiseq_ = create_bandwidth_measurer(
            config, sys_info, caps,
            measurement_storage_multiseq_.get(),
            traffic_gen_manager_.get(),
            [this]() { return get_memory_binding_nodes(); },
            ExecutionMode::MULTISEQUENTIAL);
        if (!bandwidth_measurer_multiseq_) {
            throw std::runtime_error("Failed to create MultiSequential bandwidth measurer");
        }
    }

    
    cached_tlb_hit_latency_ns_ = tlb_latency_ns;

    uint64_t tlb1_raw = 0, tlb2_raw = 0;
    bool use_tlb1 = false, use_tlb2 = false;
    ptrchase_perf::select_tlb_events_for_ptrchase(sys_info, tlb1_raw, tlb2_raw, use_tlb1, use_tlb2);
    
    auto arch_for_fmt = ArchitectureRegistry::instance().getArchitecture(caps);
    auto fmt_strategy = arch_for_fmt ? arch_for_fmt->createCounterStrategy(caps) : nullptr;
    const BandwidthCounterStrategy* fmt = fmt_strategy ? fmt_strategy.get() : counter_strategy_;
    std::string tlb1_str = use_tlb1 ? fmt->formatTlbEventName(0, tlb1_raw) : "";
    std::string tlb2_str = use_tlb2 ? fmt->formatTlbEventName(1, tlb2_raw) : "";

    if (latency_measurer_multiseq_) {
        latency_measurer_multiseq_->setExpectedTlbEvents(tlb1_str, tlb2_str);
    }


    double upi_scaling_factor = 0.0;
    auto arch = ArchitectureRegistry::instance().getArchitecture(caps);
    if (arch) {
        double factor = arch->getUpiScalingFactor(caps);
        if (factor > 0) {
            upi_scaling_factor = 1.0 / factor;
        }
    }

    discover_bandwidth_counters_if_needed();
    if (cached_bw_counters_.cas.bytes_per_cas > 0) {
        bw_bytes_per_cas_ = cached_bw_counters_.cas.bytes_per_cas;
    }
    const std::string binding_type = cached_bw_counters_.cross_socket ? "remote" : "local";
    CPUCapabilities plotter_caps = caps;
    if (binding_type == "local") {
        const auto numa_memory = detect_numa_memory_info(caps.model_name, caps.hasOnPackageHBM());
        const std::string target_memory_type = summarize_numa_memory_type_for_nodes(
            numa_memory,
            get_memory_binding_nodes(),
            caps.memory_type);
        if (memory_type_matches(target_memory_type, "HBM")) {
            plotter_caps.memory_type = target_memory_type;
            if (plotter_caps.memory_channels <= 0 || plotter_caps.memory_channels == 2) {
                plotter_caps.memory_channels = 4;
            }
            plotter_caps.bus_width = 1024;
        }
    }
    if (measurement_storage_multiseq_) {
        measurement_storage_multiseq_->set_bytes_per_cas(bw_bytes_per_cas_);
    }

    const double page_walk_lat_ns = fmt ? fmt->getPageWalkLatencyNs() : -1.0;
    if (measurement_storage_multiseq_) {
        measurement_storage_multiseq_->write_plotter_file(plotter_caps, binding_type, tlb_latency_ns, cache_line_size, upi_scaling_factor, page_walk_lat_ns);
    }


    discover_bandwidth_counters_if_needed();
}

BenchmarkExecutor::~BenchmarkExecutor() {
    force_cleanup();
}

void BenchmarkExecutor::discover_bandwidth_counters_if_needed() const {
    if (bw_counters_discovered_) {
        return;
    }
    bw_counters_discovered_ = true;

    auto& strategy = BandwidthCounterStrategy::instance();
    if (strategy.is_initialized()) {
        cached_bw_counters_ = strategy.get_selection();
        return;
    }

    const auto& cache = SystemToolsCache::instance();
    int src_cpu = cache.initialized ? cache.ptr_chase_cpu : 0;
    if (!config_.traffic_gen_explicit_cores.empty()) {
        int parsed_src_cpu = 0;
        if (string_utils::parse_int_strict(config_.traffic_gen_explicit_cores[0], parsed_src_cpu)) {
            src_cpu = parsed_src_cpu;
        } else if (config_.verbosity >= 1) {
            std::cerr << "Warning: could not parse traffic-gen core '"
                      << config_.traffic_gen_explicit_cores[0]
                      << "'; using default core selection." << std::endl;
        }
    }

    std::vector<int> monitored_nodes = resolve_monitored_nodes(src_cpu);

    strategy.set_measurer_type(string_to_measurer_type(config_.measurer));
    strategy.set_extra_counters(config_.add_counters);
    strategy.set_memory_type(caps_.memory_type);
    strategy.initialize(src_cpu, monitored_nodes, caps_);
    cached_bw_counters_ = strategy.get_selection();
}

void BenchmarkExecutor::force_cleanup() {
    if (ptrchase_manager_) {
        ptrchase_manager_->cleanup();
    }
    if (traffic_gen_manager_) {
        traffic_gen_manager_->kill_all_traffic_gen();
    }
}

double BenchmarkExecutor::get_upi_scaling_factor() const {
    auto arch = ArchitectureRegistry::instance().getArchitecture(caps_);
    if (arch) {
        return arch->getUpiScalingFactor(caps_);
    }
    return 1.0 / 9.0;
}

/**
 * @brief Determines which physical NUMA nodes or sockets should be monitored by PMU counters.
 * 
 * Hardware bandwidth counters are often tied to specific memory controllers (IMCs), which are
 * associated with NUMA nodes. This function analyzes the benchmark's execution binding
 * (e.g., local, remote, or specific NUMA node) and the CPU executing the traffic generator
 * (`src_cpu`) to resolve the list of NUMA nodes whose IMCs must be monitored.
 * 
 * For instance, on AMD systems, memory channels are scoped tightly per node; on Intel, it may
 * be per socket.
 * 
 * @param src_cpu The CPU core ID where the primary traffic generator thread is pinned.
 * @return std::vector<int> A list of NUMA node IDs to attach performance counters to.
 */
std::vector<int> BenchmarkExecutor::resolve_monitored_nodes(int src_cpu) const {
    std::vector<int> monitored_nodes = get_memory_binding_nodes();
    if (monitored_nodes.empty()) {
        const auto& cpu_topo = SystemToolsCache::instance().cpu_topology;
        if (cpu_topo.count(src_cpu)) {
            int socket = cpu_topo.at(src_cpu).socket;
            std::map<int, std::set<int>> socket_nodes = get_socket_to_nodes_map(cpu_topo);
            if (socket_nodes.count(socket)) {
                for (int node : socket_nodes[socket]) {
                    monitored_nodes.push_back(node);
                }
            }
        }
        if (monitored_nodes.empty() && sys_info_.numa_node_count > 0) {
            monitored_nodes.push_back(sys_info_.numa_nodes[0].id);
        }
    }
    return monitored_nodes;
}

void BenchmarkExecutor::cleanup_measurement_files(double ratio_pct, int pause) const {

    MeasurementStorage* storage = measurement_storage_multiseq_.get();
    storage->cleanup_measurement_files(ratio_pct, pause);
}

std::string BenchmarkExecutor::mode_label(ExecutionMode mode) const {
    (void)mode;
    return "MULTISEQ";
}

MeasurementStorage* BenchmarkExecutor::storage_for_mode(ExecutionMode mode) const {
    (void)mode;
    return measurement_storage_multiseq_.get();
}

OutlierDetector* BenchmarkExecutor::outlier_detector_for_mode(ExecutionMode mode) const {
    (void)mode;
    return outlier_detector_multiseq_.get();
}

BandwidthMeasurer* BenchmarkExecutor::bandwidth_measurer_for_mode(ExecutionMode mode) const {
    (void)mode;
    return bandwidth_measurer_multiseq_.get();
}

std::string BenchmarkExecutor::format_bandwidth_output(const BenchmarkResult& result, const BandwidthCounterSelection& selection) const {
    return measurement::format_bandwidth_output(result, selection, bw_bytes_per_cas_);
}

std::string BenchmarkExecutor::format_latency_output(const BenchmarkResult& result, pid_t ptrchase_pid) const {
    return measurement::format_latency_output(result, ptrchase_pid, config_.instruction_sampling);
}


void BenchmarkExecutor::flush_point_results(double ratio_pct,
                                            int pause,
                                            ExecutionMode mode,
                                            const std::vector<BenchmarkResult>& final_results) {
    MeasurementStorage* storage = storage_for_mode(mode);
    BandwidthMeasurer* bandwidth_measurer = bandwidth_measurer_for_mode(mode);

    std::stringstream bw_buffer;
    std::stringstream lat_buffer;
    std::vector<uint64_t> sampler_cycles;
    const auto& selection = bandwidth_measurer->get_counter_selection();

    for (const auto& result : final_results) {
        bw_buffer << format_bandwidth_output(result, selection);
        lat_buffer << format_latency_output(result, ptrchase_manager_->pid());
        sampler_cycles.insert(sampler_cycles.end(),
                              result.sampler_raw_latencies.begin(),
                              result.sampler_raw_latencies.end());
    }

    const std::string bw_file = storage->bw_file_path(ratio_pct, pause);
    storage->append_to_file_with_lock(bw_file, bw_buffer.str());

    const std::string lat_file = storage->lat_file_path(ratio_pct, pause);
    if (!storage->append_to_file_with_lock(lat_file, lat_buffer.str())) {
        std::cerr << "  Failed to write latency data: " << lat_file << std::endl;
    }

    if (!sampler_cycles.empty()) {
        storage->append_sampler_samples(ratio_pct, pause, sampler_cycles);
    }

}

void BenchmarkExecutor::remove_results_for_point(double ratio_pct,
                                                 int pause,
                                                 ExecutionMode mode) {
    results_.erase(
        std::remove_if(results_.begin(), results_.end(),
                       [&](const BenchmarkResult& result) {
                           return std::abs(result.ratio_pct - ratio_pct) < 1e-9 &&
                                  result.pause == pause &&
                                  result.mode == mode;
                       }),
        results_.end());
}

void BenchmarkExecutor::account_measured_point_progress(
    const std::vector<BenchmarkResult>& final_results) {
    for (const auto& result : final_results) {
        progress_tracker_->record_iteration_time(result.iteration_time_s);
        progress_tracker_->increment_completed_iterations();
    }
}

ProbeStats BenchmarkExecutor::summarize_probe_stats(
    int pause, const std::vector<BenchmarkResult>& final_results) const {
    return measurement::summarize_point(pause, final_results);
}

bool BenchmarkExecutor::resolve_point_measurements(
    double ratio_pct,
    int pause,
    ExecutionMode mode,
    int& total_global_measurements,
    std::vector<BenchmarkResult>& final_results,
    double bw_peak,
    bool skip_outliers_for_low_bw,
    bool count_progress,
    bool replace_existing) {
    OutlierDetector* outlier_detector = outlier_detector_for_mode(mode);
    const std::string mode_str = mode_label(mode);

    auto refresh_progress_line = [&]() {
        int remaining = total_global_measurements - progress_tracker_->completed_iterations();
        std::string progress_line = progress_tracker_->format_progress(
            progress_tracker_->completed_iterations(), total_global_measurements, remaining);
        std::cout << "\r" << progress_line << std::flush;
    };

    auto print_progress_checkpoint = [&]() {
        refresh_progress_line();
        if (config_.verbosity >= 2) {
            std::cout << std::endl << std::endl;
        }
    };

    auto print_point_measurements = [&](const std::vector<BenchmarkResult>& point_results,
                                        const std::string& header) {
        if (config_.verbosity < 2) {
            return;
        }

        std::cout << header << std::endl;
        std::cout << "    BW measurements: ";
        for (size_t i = 0; i < point_results.size(); ++i) {
            std::cout << std::fixed << std::setprecision(2)
                      << point_results[i].bandwidth_mbps / 1000.0 << " GB/s";
            if (i + 1 < point_results.size()) {
                std::cout << ", ";
            }
        }
        std::cout << std::endl << "    Latency measurements: ";
        for (size_t i = 0; i < point_results.size(); ++i) {
            std::cout << std::fixed << std::setprecision(1)
                      << point_results[i].latency_ns << " ns";
            if (i + 1 < point_results.size()) {
                std::cout << ", ";
            }
        }
        std::cout << std::endl;
    };

    int successful_measurements = 0;
    auto measure_rep = [&](int rep, BenchmarkResult& result) -> bool {
        int consecutive_failures = 0;
        bool reuse_traffic_gen = false;

        while (true) {
            result = BenchmarkResult{};
            result.ratio_pct = ratio_pct;
            result.pause = pause;
            result.repetition = rep;
            result.iteration_time_s = 0.0;

            progress_tracker_->start_iteration();

            if (config_.verbosity >= 2) {
                std::cout << platform::clear_line();
                std::cout << " Issued Load Ratio: " << std::fixed << std::setprecision(0)
                          << ratio_pct << "\n Pause: " << pause
                          << "\n Execution Mode: " << mode_str << std::endl;
            }

            BenchmarkStatus status =
                run_single_benchmark(ratio_pct, pause, rep, result, mode, reuse_traffic_gen);
            if (status == BenchmarkStatus::SUCCESS) {
                double iteration_time = progress_tracker_->finish_iteration();
                result.iteration_time_s = iteration_time;
                if (count_progress) {
                    progress_tracker_->record_iteration_time(iteration_time);
                    progress_tracker_->increment_completed_iterations();
                    successful_measurements++;
                }
                if (count_progress && config_.verbosity < 2) {
                    refresh_progress_line();
                }
                return true;
            }

            progress_tracker_->finish_iteration();

            reuse_traffic_gen = status == BenchmarkStatus::RETRY_BANDWIDTH;
            consecutive_failures++;
            if (consecutive_failures >= 5) {
                std::cerr << "\nERROR: Too many consecutive failures ("
                          << consecutive_failures
                          << "). Restarting point." << std::endl;
                traffic_gen_manager_->kill_all_traffic_gen();
                return false;
            }

            if (config_.verbosity >= 2) {
                std::cout << "    Retrying measurement (failure "
                          << consecutive_failures << "/5)...\n";
            }

            if (!reuse_traffic_gen && traffic_gen_manager_) {
                traffic_gen_manager_->kill_all_traffic_gen();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    };
    auto begin_attempt = [&]() {
        if (count_progress && successful_measurements > 0) {
            progress_tracker_->reset_completed_iterations(
                progress_tracker_->completed_iterations() - successful_measurements);
        }
        successful_measurements = 0;
        cleanup_measurement_files(ratio_pct, pause);
    };
    auto find_outliers = [&](const std::vector<BenchmarkResult>& samples) {
        double max_bw = 0.0;
        for (const auto& sample : samples)
            max_bw = std::max(max_bw, sample.bandwidth_mbps / 1000.0);
        if (config_.point_reps <= 1 || (skip_outliers_for_low_bw && bw_peak > 1e-12 &&
                                      max_bw < 0.02 * bw_peak)) return std::vector<int>{};
        print_point_measurements(samples, "  Point quality: " + mode_str);
        if (config_.verbosity >= 2) std::cout << std::endl;
        return outlier_detector->find_outliers(samples, 10.0);
    };
    measurement::PointSamplingPolicy policy;
    policy.repetitions = config_.point_reps;
    if (!measurement::resolve_point(policy, measure_rep, find_outliers, begin_attempt, final_results)) {
        begin_attempt();
        std::cerr << "\nERROR: Failed to resolve point within the retry budget.\n";
        return false;
    }
    if (count_progress && successful_measurements > static_cast<int>(final_results.size())) {
        progress_tracker_->reset_completed_iterations(progress_tracker_->completed_iterations() -
            successful_measurements + static_cast<int>(final_results.size()));
    }
    if (replace_existing) remove_results_for_point(ratio_pct, pause, mode);
    flush_point_results(ratio_pct, pause, mode, final_results);
    results_.insert(results_.end(), final_results.begin(), final_results.end());
    const ProbeStats summary = summarize_probe_stats(pause, final_results);
    progress_tracker_->update_ratio_stats(ratio_pct, summary.bw * 1000.0, summary.lat);
    if (count_progress) print_progress_checkpoint();
    return true;
}

BenchmarkExecutor::AdaptiveMeasuredPoint BenchmarkExecutor::adaptive_probe(
    double ratio_pct,
    int pause,
    ExecutionMode mode,
    int& total_global_measurements,
    double bw_peak,
    bool count_progress,
    bool replace_existing) {
    std::vector<BenchmarkResult> final_results;
    if (!resolve_point_measurements(ratio_pct, pause, mode, total_global_measurements,
                                    final_results, bw_peak, true,
                                    count_progress, replace_existing)) {
        throw std::runtime_error("Failed to resolve adaptive point measurements");
    }
    return {summarize_probe_stats(pause, final_results), std::move(final_results)};
}

bool BenchmarkExecutor::run_adaptive_for_ratio(double ratio_pct,
                                                const std::vector<ExecutionMode>& modes,
                                                int& total_global_measurements) {
    std::vector<std::string> labels;
    for (auto mode : modes) labels.push_back(mode_label(mode));
    auto measure = [&](size_t index, int pause, double peak, bool count, bool replace) {
        auto point = adaptive_probe(ratio_pct, pause, modes[index], total_global_measurements,
                                    peak, count, replace);
        return measurement::MeasuredPoint{point.stats, std::move(point.results)};
    };
    return measurement::run_adaptive_sweep(config_, labels, *progress_tracker_, total_global_measurements,
        measure, [&](const std::vector<BenchmarkResult>& results) { account_measured_point_progress(results); });
}

bool BenchmarkExecutor::run() {

    results_.clear();

    std::vector<double> ratios = config_.ratios_pct;
    if (ratios.empty()) {
        for (int i = 100; i >= 0; i -= 2) {
            ratios.push_back(static_cast<double>(i));
        }
    }

    std::vector<ExecutionMode> modes;
    bool do_multiseq = config_.generate_multiseq;
    if (do_multiseq) modes.push_back(ExecutionMode::MULTISEQUENTIAL);


    if (!ptrchase_manager_->ensure_running()) {
        std::cerr << "ERROR: Failed to launch persistent pointer chase process" << std::endl;
        return false;
    }

    int total_ratios = static_cast<int>(ratios.size());
    int current_ratio = 0;

    if (config_.pauses.empty()) {
        AdaptiveTier tier = CurveTracer::resolve_tier(config_.adaptive_tier, config_.adaptive_point_count);
        int num_modes = static_cast<int>(modes.size());
        int estimated_total = total_ratios * tier.max_budget * config_.point_reps * num_modes;
        int total_global_measurements = estimated_total;
        progress_tracker_->start_benchmark(total_global_measurements);
        progress_tracker_->set_adaptive_info(
            total_ratios * num_modes,
            config_.point_reps,
            tier.max_budget,
            num_modes);

        for (double ratio_pct : ratios) {
            current_ratio++;
            progress_tracker_->set_current_ratio(ratio_pct, current_ratio, total_ratios);
            if (config_.verbosity >= 2) {
                std::cout << platform::clear_line();
                std::cout << "\n\n=== " << ratio_pct << "% loads ("
                          << current_ratio << "/" << total_ratios << " load ratios) ===" << std::endl;
            }

            if (!run_adaptive_for_ratio(ratio_pct, modes, total_global_measurements)) {
                return false;
            }
        }
    } else {
        std::vector<int> pauses = config_.pauses;

        int total_global_measurements = ratios.size() * pauses.size() * config_.point_reps * modes.size();
        progress_tracker_->start_benchmark(total_global_measurements);

        for (double ratio_pct : ratios) {
            current_ratio++;
            progress_tracker_->set_current_ratio(ratio_pct, current_ratio, total_ratios);
            if (config_.verbosity >= 2) {
                std::cout << "\n\n=== " << ratio_pct << "% loads (" 
                          << current_ratio << "/" << total_ratios << " load ratios) ===" << std::endl;
            }

            std::map<ExecutionMode, std::vector<double>> bw_history;
            std::set<int> backtracked_pauses;

            for (int pause_idx = 0; pause_idx < static_cast<int>(pauses.size()); ++pause_idx) {
                int pause = pauses[pause_idx];
                    bool backtrack_triggered = false;

                    for (ExecutionMode mode : modes) {
                        std::vector<BenchmarkResult> current_point_results;
                        if (!resolve_point_measurements(ratio_pct, pause, mode,
                                                        total_global_measurements,
                                                        current_point_results)) {
                            return false;
                        }

                        if (pause_idx > 0 && pauses[pause_idx] > pauses[pause_idx - 1]) {
                            double total_bw = 0;
                            for (const auto& res : current_point_results) {
                                total_bw += res.bandwidth_mbps;
                            }
                            double avg_bw = total_bw / current_point_results.size();

                            if (bw_history[mode].size() <= static_cast<size_t>(pause_idx)) {
                                bw_history[mode].resize(pause_idx + 1, 0.0);
                            }
                            bw_history[mode][pause_idx] = avg_bw;

                            double prev_bw = bw_history[mode][pause_idx - 1];
                            
                            if (prev_bw > 0.1 && avg_bw > prev_bw * 1.5) {
                                if (backtracked_pauses.find(pause) == backtracked_pauses.end()) {
                                    if (config_.verbosity >= 1) {
                                        std::cout << "    " << platform::glyph::kWarning << " Anomaly detected: Higher pause (" << pause 
                                                << ") has significantly higher BW (" << std::fixed << std::setprecision(2) << avg_bw/1000.0 
                                                << " GB/s) than previous pause (" << pauses[pause_idx-1] 
                                                << ", " << prev_bw/1000.0 << " GB/s). (+50% threshold exceeded)\n";
                                        std::cout << "    " << platform::glyph::kRetry << " Backtracking to repeat pause " << pauses[pause_idx-1] << "...\n";
                                    }
                                    
                                    backtracked_pauses.insert(pause);
                                    pause_idx -= 2;
                                    backtrack_triggered = true;
                                    break; 
                                } else {
                                    if (config_.verbosity >= 1) {
                                        std::cout << "    " << platform::glyph::kWarning << " Anomaly persisted after retry: Higher pause (" << pause 
                                                << ") still has higher BW. Ignoring and continuing...\n";
                                    }
                                }
                            }
                        } else {
                            double total_bw = 0;
                            for (const auto& res : current_point_results) {
                                total_bw += res.bandwidth_mbps;
                            }
                            double avg_bw = total_bw / current_point_results.size();
                            
                            if (bw_history[mode].size() <= static_cast<size_t>(pause_idx)) {
                                bw_history[mode].resize(pause_idx + 1, 0.0);
                            }
                            bw_history[mode][pause_idx] = avg_bw;
                        }

                        if (backtrack_triggered) break;
                    }
                }
            }
        }

    if (config_.verbosity >= 2) {
        std::cout << "\n\n=== Final Cleanup ===" << std::endl;
        std::cout << "Terminating processes..." << std::endl;
    }
        
    if (ptrchase_manager_) {
        ptrchase_manager_->cleanup();
    }
    
    if (traffic_gen_manager_) {
        traffic_gen_manager_->kill_all_traffic_gen();
    }
    
    if (config_.verbosity >= 2) {
        std::cout << "Cleanup complete" << std::endl;
    }

    return true;
}

std::vector<int> BenchmarkExecutor::get_memory_binding_nodes() const {
    return config_.memory_bind_nodes;
}



/**
 * @brief Executes a single iteration of the benchmark suite for a specific traffic mix and mode.
 * 
 * 
 * @param ratio_pct The current mix of reads/writes being tested (0.0 to 100.0).
 * @param pause The configured pause or delay between memory operations.
 * @param rep The current repetition number.
 * @param result An output struct where the collected metrics are appended.
 * @param mode The execution mode.
 * @return BenchmarkStatus Indicates whether the run succeeded or requires a retry due to outliers.
 */
BenchmarkExecutor::BenchmarkStatus BenchmarkExecutor::run_single_benchmark(double ratio_pct, int pause, int rep, BenchmarkResult& result, ExecutionMode mode, bool reuse_traffic_gen) {
    MeasurementStorage* measurement_storage_ = measurement_storage_multiseq_.get();
    BandwidthMeasurer* bandwidth_measurer_ = bandwidth_measurer_multiseq_.get();
    LatencyMeasurer* latency_measurer_ = latency_measurer_multiseq_.get();

    result.repetitions = config_.point_reps;
    result.mode = mode;

    measurement_storage_->ensure_directories_exist();

    if (config_.instruction_sampling && instruction_sampler_) {
        instruction_sampler_->cleanup();
    }
    
    if (ratio_pct < 0 || ratio_pct > 100) {
        std::cerr << "ERROR: Invalid ratio_pct value: " << ratio_pct << std::endl;
        return BenchmarkStatus::FAILURE;
    }
    
    if (pause < 0) {
        std::cerr << "ERROR: Invalid pause value: " << pause << std::endl;
        return BenchmarkStatus::FAILURE;
    }
    
    std::string traffic_gen_log_file = measurement_storage_->traffic_gen_log_file_path(ratio_pct, pause);
    
    int traffic_gen_cores = config_.instruction_sampling
        ? sys_info_.sockets[0].core_count
        : get_default_traffic_gen_cores(sys_info_);
    static bool warned_cross_socket_total_cores = false;
    if (config_.traffic_gen_cores > sys_info_.sockets[0].core_count && !warned_cross_socket_total_cores) {
        std::cerr << "WARNING: --total-cores is larger than one socket. Mess profiles single-socket curves; spreading traffic across sockets affects the measurements and is not the expected way of running." << std::endl;
        warned_cross_socket_total_cores = true;
    }
    if (config_.traffic_gen_cores > 0 && config_.traffic_gen_cores <= sys_info_.sockets[0].core_count) {
        traffic_gen_cores = config_.traffic_gen_cores;
    }
    
    if (traffic_gen_cores <= 0 || traffic_gen_cores > 1024) {
        std::cerr << "ERROR: Invalid traffic_gen_cores value: " << traffic_gen_cores << std::endl;
        return BenchmarkStatus::FAILURE;
    }

    std::vector<int> traffic_gen_mem_nodes = get_memory_binding_nodes();

    TrafficGenPreparation traffic_gen_prep;
    if (!traffic_gen_manager_->prepare_traffic_gen(ratio_pct,
                                         pause,
                                         traffic_gen_cores,
                                         traffic_gen_mem_nodes,
                                         traffic_gen_log_file,
                                         traffic_gen_prep,
                                         mode,
                                         reuse_traffic_gen)) {
        return BenchmarkStatus::FAILURE;
    }

    bool reuse_existing_traffic_gen = traffic_gen_prep.reused_existing;
    int traffic_gen_pid = traffic_gen_prep.pid;

    if (!reuse_existing_traffic_gen) {
        if (!traffic_gen_manager_->wait_for_traffic_gen_ready(120)) {
            if (!traffic_gen_manager_->is_traffic_gen_running(traffic_gen_pid)) {
                std::cerr << "    ERROR: TrafficGen failed during initialization" << std::endl;
                return BenchmarkStatus::FAILURE;
            }
            if (config_.verbosity >= 1) {
                std::cerr << "    Warning: TrafficGen ready wait failed, proceeding anyway" << std::endl;
            }
        }
    }

    discover_bandwidth_counters_if_needed();
    BandwidthCounterSelection bw_counters = cached_bw_counters_;
    for (const auto& counter : config_.add_counters) {
        if (std::find(bw_counters.extra_counters.begin(), bw_counters.extra_counters.end(), counter) ==
            bw_counters.extra_counters.end()) {
            bw_counters.extra_counters.push_back(counter);
        }
    }
    bandwidth_measurer_->set_counter_selection(bw_counters);

    const bool verbose1 = config_.verbosity >= 1;
    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;


    if (verbose2) {
        std::cout << "    Current measurement is " << (rep + 1) << "/" << config_.point_reps << '\n';
    }
    
    if (!config_.instruction_sampling) {
        if (latency_measurer_->is_burst_running() || latency_measurer_->burst_failed()) {
            ptrchase_manager_->cleanup();
            latency_measurer_->reset();
        }
        if (!ptrchase_manager_->ensure_running()) {
            std::cerr << "ERROR: Cannot ensure ptr_chase is running" << std::endl;
            return BenchmarkStatus::FAILURE;
        }
                
        if (verbose3) {
            std::cout << "      Still active with pid: " << ptrchase_manager_->pid() << '\n';
        }
    }

    // Sampling weights are cycles, with no pointer-chase thread clock available.
    // Calibrate only when this backend is used, before any measurement window.
    if (config_.instruction_sampling) {
        if (sampler_frequency_ghz_ <= 0.0) {
            const auto frequency = probe_cpu_frequency(SystemToolsCache::instance().ptr_chase_cpu);
            if (!frequency.ok()) {
                std::cerr << "Instruction sampler frequency calibration failed: " << frequency.error << '\n';
                traffic_gen_manager_->terminate_traffic_gen(traffic_gen_pid);
                return BenchmarkStatus::FAILURE;
            }
            sampler_frequency_ghz_ = frequency.frequency_ghz;
        }
        result.point_cpu_freq_ghz = sampler_frequency_ghz_;
    }

    
    if (traffic_gen_pid > 0) {
        int traffic_gen_samples = 0;
        
        if (verbose2) {
            std::cout << "    Waiting for TrafficGen to stabilize...\n";
        }

        long long bw_cas_rd = 0, bw_cas_wr = 0;
        double bw_elapsed = 0;
        bool traffic_gen_stabilized = false;
        const double sampling_interval_ms = bandwidth_measurer_->sampling_interval_ms();
        
        const double stabilization_timeout_samples = (60.0 * 1000.0) / sampling_interval_ms;

        std::deque<PerfBurstCounters> latency_samples;
        
        auto print_burst_stats = [&](const PerfBurstCounters& b, int current_count) {
            if (verbose3) {
                const double latency_ns = calculate_burst_latency(b);
                
                std::cout << "      [Latency Burst " << current_count << "] "
                          << "cycles: " << static_cast<long long>(b.cycles) << " | "
                          << "instr: " << static_cast<long long>(b.instructions) << " | "
                          << "tlb1: " << static_cast<long long>(b.tlb1miss) << " | "
                          << "tlb2: " << static_cast<long long>(b.tlb2miss) << " | "
                          << "dur: " << std::fixed << std::setprecision(3) << b.duration_s << " s";
                // Reports the allocation, not the privilege. MEM_LARGE_PAGES needs a contiguous
                // physical reservation and falls back to 4 KB without saying so.
                std::cout << " | hp: " << (b.using_hugepages ? "yes" : "no");
                std::cout << " | lat: " << std::fixed << std::setprecision(1)
                          << latency_ns << " ns" << '\n';
            }
        };

        bool sampler_started = false;
        int sampler_callback_count = 0;
        std::function<void()> on_sample_callback;
        if (config_.instruction_sampling && instruction_sampler_) {
            on_sample_callback = [&]() {
                ++sampler_callback_count;
                if (!sampler_started && sampler_callback_count >= 3) {
                    std::vector<int> cores;
                    if (config_.traffic_gen_explicit_cores.empty()) {
                        cores = select_worker_cpus(traffic_gen_cores, -1);
                    } else {
                        for (const auto& c : config_.traffic_gen_explicit_cores) {
                            cores.push_back(std::stoi(c));
                        }
                    }
                    std::vector<platform::ProcessId> pids = traffic_gen_manager_->traffic_gen_worker_pids();
                    if (instruction_sampler_->start_async(cores, pids)) {
                        sampler_started = true;
                    }
                }
            };
        }

        traffic_gen_stabilized = bandwidth_measurer_->wait_for_stabilization(
            traffic_gen_samples, bw_cas_rd, bw_cas_wr, bw_elapsed,
            pause, ratio_pct, reuse_existing_traffic_gen, on_sample_callback);

        // If BW didn't stabilize (60s timeout), abort this measurement and retry
        if (!traffic_gen_stabilized && traffic_gen_samples > stabilization_timeout_samples) {
            if (verbose1) {
                std::cout << "    " << platform::glyph::kWarning << " Aborting measurement due to BW instability (Time limit)\n";
            }
            if (config_.instruction_sampling && instruction_sampler_) {
                instruction_sampler_->stop_and_process(0.0);
            }
            return BenchmarkStatus::FAILURE;
        }

        if (verbose2) {
            if (traffic_gen_stabilized) {
                double wait_time = (sampling_interval_ms * traffic_gen_samples + 100.0);
                const auto& stab_selection = bandwidth_measurer_->get_counter_selection();
                const bool is_pcm_counter_mode =
                    (!stab_selection.cas.read_events.empty() &&
                     stab_selection.cas.read_events[0].find("pcm_cxl/") != std::string::npos) ||
                    (!stab_selection.cas.write_events.empty() &&
                     stab_selection.cas.write_events[0].find("pcm_cxl/") != std::string::npos);
                if (is_pcm_counter_mode && bw_elapsed > 0.0) {
                    wait_time = bw_elapsed * 1000.0;
                }
                std::cout << "    Bandwidth stabilized after " << traffic_gen_samples << " samples";
                if (verbose3 && config_.measurer != "vtune") {
                    std::string time_str = (wait_time >= 1000.0)
                        ? (std::to_string(static_cast<int>(std::round(wait_time/1000.0))) + "s")
                        : (std::to_string(static_cast<int>(std::round(wait_time))) + "ms");
                    std::cout << " ( ~" << time_str << " )";
                }
                std::cout << '\n';
            } else {
                std::cout << "    Timeout, measuring last value\n";
            }
        }

        if (!traffic_gen_stabilized && traffic_gen_samples == 0) {
            std::cerr << "    Error: Failed to collect any bandwidth samples for this point." << std::endl;
            return BenchmarkStatus::FAILURE;
        }
        

        PerfBurstCounters burst_totals{};
        double total_accesses = 0.0;
        int bursts_aggregated = 0;
        InstructionSamplerStats sampler_stats;
        
        const int min_bursts = 3;
        const int max_bursts = 20;
        const double latency_stability_cv_threshold = 0.03;
        
        auto are_last_3_stable = [&](const std::deque<PerfBurstCounters>& samples) -> bool {
            if (samples.size() < 3) return false;

            std::array<double, 3> latencies{};
            double sum = 0.0;
            int count = 0;
            for (auto it = samples.rbegin(); it != samples.rend() && count < 3; ++it) {
                const double lat = calculate_burst_latency(*it);
                if (lat <= 0.0) return false;
                latencies[count] = lat;
                sum += lat;
                count++;
            }

            if (count < 3) {
                return false;
            }

            double mean = sum / 3.0;
            double sq_sum = 0.0;
            for (double lat : latencies) {
                sq_sum += (lat - mean) * (lat - mean);
            }
            double std_dev = std::sqrt(sq_sum / 3.0);
            double cv = (mean > 0) ? (std_dev / mean) : 1.0;
            return cv < latency_stability_cv_threshold;
        };

        int latency_retries = 0;
        const int max_latency_retries = 3;
        
        bool latency_success = false;

        if (config_.instruction_sampling) {
            double freq_ghz = result.point_cpu_freq_ghz;
            sampler_stats = instruction_sampler_->stop_and_process(freq_ghz);

            if (sampler_stats.samples > 0) {
                latency_success = true;
                if (verbose2) {
                    std::cout << "    Sampler collected " << sampler_stats.samples << " latency samples"
                              << " | mean=" << std::fixed << std::setprecision(1) << sampler_stats.mean_ns << " ns"
                              << " | median=" << sampler_stats.median_ns << " ns"
                              << " | p99=" << sampler_stats.p99_ns << " ns" << std::endl;
                }
            } else {
                if (verbose1) {
                    std::cout << "    Warning: No latency samples collected during BW stabilization window" << std::endl;
                }
            }

        } else {
            while (latency_retries < max_latency_retries && !latency_success) {
                if (latency_retries > 0) {
                    latency_samples.clear();
                } else {
                    while (latency_samples.size() >= max_bursts) {
                        latency_samples.pop_front();
                    }
                }
                
                if (!ptrchase_manager_->ensure_running()) {
                    std::cerr << "ERROR: Cannot ensure ptr_chase is running" << std::endl;
                    latency_retries++;
                    continue;
                }

                if (latency_retries > 0) {
                     if (verbose1) {
                        std::cout << "    Retrying latency measurement (attempt " << (latency_retries + 1) << "/" << max_latency_retries << ")...\n";
                     }
                }

                if (verbose2) {
                    std::cout << "    Waiting for stable latency bursts...\n";
                }
                 
                while (latency_samples.size() < max_bursts) {
                     if (latency_measurer_->burst_failed()) {
                         std::cerr << "    Pointer-chase burst failed: "
                                   << latency_measurer_->burst_error() << '\n';
                         break;
                     }
                     PerfBurstCounters burst_result;
                     if (latency_measurer_->is_burst_running()) {
                         if (latency_measurer_->try_collect_burst_async(burst_result)) {
                            latency_samples.push_back(burst_result);
                            
                            print_burst_stats(burst_result, latency_samples.size());
                            if (latency_samples.size() >= min_bursts && are_last_3_stable(latency_samples)) {
                                if (verbose2) {
                                    std::cout << "    Latency bursts stabilized\n";
                                }
                                latency_success = true;
                                break;
                            }

                             if (latency_samples.size() < max_bursts) {
                                 latency_measurer_->start_burst_async();
                             }
                         } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                         }
                     } else {
                         latency_measurer_->start_burst_async();
                     }
                }

                if (latency_success) {
                    break;
                }

                std::cerr << "    Warning: Failed to collect valid latency bursts." << std::endl;
                
                if (ptrchase_manager_) {
                    std::cerr << "    Forcing ptr_chase restart..." << std::endl;
                    ptrchase_manager_->cleanup();
                    latency_measurer_->reset();
                }
                latency_retries++;
            }
        }

        if (!latency_success) {
            std::cerr << "    Error: Failed to measure latency after " << max_latency_retries << " attempts." << std::endl;
            return BenchmarkStatus::FAILURE;
        }


        if (!config_.instruction_sampling) {
            int count = 0;
            for (auto it = latency_samples.rbegin(); it != latency_samples.rend() && count < 3; ++it) {
                burst_totals.cycles += it->cycles;
                burst_totals.instructions += it->instructions;
                burst_totals.tlb1miss += it->tlb1miss;
                burst_totals.tlb2miss += it->tlb2miss;
                burst_totals.duration_s += it->duration_s;
                burst_totals.pointer_chase_thread_ns += it->pointer_chase_thread_ns;
                total_accesses += it->accesses;
                burst_totals.accesses += it->accesses;
                if (it->pointer_chase_thread_ns <= 0.0 || it->accesses <= 0.0) {
                    std::cerr << "Missing pointer-chase thread CPU timing; regenerate the pointer-chase binary.\n";
                    return BenchmarkStatus::FAILURE;
                }
                
                if (count == 0) {
                    burst_totals.tlb1_event_name = it->tlb1_event_name;
                    burst_totals.tlb2_event_name = it->tlb2_event_name;
                }
                count++;
            }
            bursts_aggregated = count;
            

            if (verbose3) {
                std::cout << "      averaged last " << bursts_aggregated << " bursts\n";
            }
        }

        
        
        
        long long final_bw_cas_rd = 0, final_bw_cas_wr = 0;
        double final_bw_elapsed = 0;

        const auto& final_selection = bandwidth_measurer_->get_counter_selection();
        const bool is_pcm_counter_mode =
            (!final_selection.cas.read_events.empty() &&
             final_selection.cas.read_events[0].find("pcm_cxl/") != std::string::npos) ||
            (!final_selection.cas.write_events.empty() &&
             final_selection.cas.write_events[0].find("pcm_cxl/") != std::string::npos);
        const bool skip_final_bw_recheck = (config_.measurer == "vtune") || is_pcm_counter_mode;
        if (skip_final_bw_recheck) {
            if (verbose3) {
                if (is_pcm_counter_mode) {
                    std::cout << "    BW re-check skipped for PCM (using stabilized streaming sample)\n";
                } else {
                    std::cout << "    BW re-check skipped for VTune to avoid extra collection overhead\n";
                }
            }
        } else if (bandwidth_measurer_->sample_bandwidth(final_bw_cas_rd,
                                                    final_bw_cas_wr,
                                                    final_bw_elapsed,
                                                    traffic_gen_mem_nodes)) {
            CounterType ctype = bandwidth_measurer_->get_counter_selection().type;
            double scaling_factor = (ctype == CounterType::UPI_FLITS) ? get_upi_scaling_factor() : 1.0;

            double initial_bw = BandwidthMeasurer::calculate_bandwidth_gbps(
                bw_cas_rd, bw_cas_wr, bw_elapsed, ctype, bw_bytes_per_cas_, scaling_factor);
            double final_bw = BandwidthMeasurer::calculate_bandwidth_gbps(
                final_bw_cas_rd, final_bw_cas_wr, final_bw_elapsed, ctype, bw_bytes_per_cas_, scaling_factor);
            const auto recheck = measurement::confirm_bandwidth(
                initial_bw,
                measurement::BandwidthReading{final_bw_cas_rd, final_bw_cas_wr, final_bw_elapsed},
                [&](measurement::BandwidthReading& next) {
                    return bandwidth_measurer_->sample_bandwidth(
                        next.read, next.write, next.seconds, traffic_gen_mem_nodes);
                },
                [&](const measurement::BandwidthReading& reading) {
                    return BandwidthMeasurer::calculate_bandwidth_gbps(
                        reading.read, reading.write, reading.seconds,
                        ctype, bw_bytes_per_cas_, scaling_factor);
                }, bandwidth_measurer_->stability_noise_floor_gbps());

            if (recheck.status != measurement::BandwidthRecheckStatus::Accepted) {
                if (verbose1) {
                    if (recheck.status == measurement::BandwidthRecheckStatus::Changed) {
                        std::cout << "    BW changed from " << initial_bw << " to "
                                  << recheck.bandwidth << " GB/s over " << recheck.samples
                                  << " confirmation samples; retrying this repetition.\n";
                    } else {
                        std::cout << "    Could not confirm the bandwidth recheck; retrying this repetition.\n";
                    }
                }
                return recheck.status == measurement::BandwidthRecheckStatus::Changed
                    ? BenchmarkStatus::RETRY_BANDWIDTH : BenchmarkStatus::FAILURE;
            }

            bw_cas_rd = recheck.reading.read;
            bw_cas_wr = recheck.reading.write;
            bw_elapsed = recheck.reading.seconds;
            if (verbose3) {
                std::cout << "    BW re-check: " << final_bw << " GB/s; accepted "
                          << recheck.samples << "-sample mean " << recheck.bandwidth << " GB/s\n";
            }
        } else if (!skip_final_bw_recheck && bw_elapsed <= 0.0 && bw_cas_rd == 0 && bw_cas_wr == 0) {
            std::cerr << "    Error: Final bandwidth sampling failed and no valid bandwidth measurement is available." << std::endl;
            return BenchmarkStatus::FAILURE;
        }
        
        if (verbose2) {
            std::cout << "    Both BW and latency stabilized\n";
        }
        
        double total_cycles = burst_totals.cycles;
        double total_instructions = burst_totals.instructions;
        double total_tlb1miss = burst_totals.tlb1miss;
        double total_tlb2miss = burst_totals.tlb2miss;
        double aggregate_duration = burst_totals.duration_s;
        bool using_hugepages = burst_totals.using_hugepages;
        
        double measured_ratio_pct = ratio_pct;
        if (bw_cas_rd + bw_cas_wr > 0) {
            measured_ratio_pct = (double)bw_cas_rd / (double)(bw_cas_rd + bw_cas_wr) * 100.0;
        }
        
        
        result.measured_ratio_pct = measured_ratio_pct;
        result.cycles = total_cycles;
        result.instructions = total_instructions;
        result.accesses = total_accesses;
        result.tlb1miss = total_tlb1miss;
        result.tlb2miss = total_tlb2miss;
        result.duration_s = aggregate_duration;
        result.pointer_chase_thread_ns = burst_totals.pointer_chase_thread_ns;
        if (!config_.instruction_sampling) {
            result.point_cpu_freq_ghz = resolve_pointer_chase_timing(
                total_cycles, result.pointer_chase_thread_ns).frequency_ghz;
            result.latency_direct_ns = calculate_burst_latency(burst_totals);
        }
        result.using_hugepages = using_hugepages;
        result.bw_cas_rd = bw_cas_rd;
        result.bw_cas_wr = bw_cas_wr;
        result.bw_elapsed = bw_elapsed;
        result.traffic_gen_samples = traffic_gen_samples;
        
        result.tlb1_event_name = burst_totals.tlb1_event_name;
        result.tlb2_event_name = burst_totals.tlb2_event_name;

        result.extra_perf_values = bandwidth_measurer_->get_extra_perf_values();

                
        double measured_bw_gb_s = 0.0;
        if (bw_elapsed > 0) {
            CounterType counter_type = bandwidth_measurer_->get_counter_selection().type;
            double scaling_factor = (counter_type == CounterType::UPI_FLITS) ? get_upi_scaling_factor() : 1.0;
            measured_bw_gb_s = BandwidthMeasurer::calculate_bandwidth_gbps(
                bw_cas_rd, bw_cas_wr, bw_elapsed, counter_type, bw_bytes_per_cas_, scaling_factor);
            result.bandwidth_mbps = measured_bw_gb_s * 1000.0;
        }
        
        if (verbose2) {
            std::cout << "      Measured Bandwidth: " << std::fixed << std::setprecision(2) 
                      << measured_bw_gb_s << " GB/s\n";
            if (verbose3) {
                const auto& selection = bandwidth_measurer_->get_counter_selection();
                bool has_distinct_rw = selection.cas.has_read_write;
                if (has_distinct_rw) {
                    std::string rd_label = "rd", wr_label = "wr";
                    if (selection.type == CounterType::UPI_FLITS) {
                        rd_label = "RXL"; wr_label = "TXL";
                    } else if (selection.type == CounterType::NVIDIA_GRACE) {
                        rd_label = "rd_data"; wr_label = "wr_bytes";
                    } else if (selection.type == CounterType::TOTAL_BYTES) {
                        rd_label = "local_dram_byte_rd"; wr_label = "local_dram_byte_wr";
                    } else if (selection.cas.uses_read_subtract_formula) {
                        rd_label = "r17-(r325+r326)";
                        wr_label = "r18";
                    }
                    std::cout << "         (" << rd_label << ": " << bw_cas_rd << ", " << wr_label << ": " << bw_cas_wr 
                          << ", elapsed: " << std::fixed << std::setprecision(2) << bw_elapsed << "s)\n";
                } else {
                    std::cout << "         (DRAM count: " << (bw_cas_rd + bw_cas_wr)
                          << ", elapsed: " << std::fixed << std::setprecision(2) << bw_elapsed << "s)\n";
                }
            }
        }
    
        if (!config_.instruction_sampling && result.latency_direct_ns > 0.0) {
            result.latency_ns = result.latency_direct_ns;
            
            if (verbose2) {
                std::cout << "      Measured Latency:   " << std::fixed << std::setprecision(2)
                          << result.latency_ns << " ns\n";
                if (verbose3) {
                        std::cout << "         (aggregated over " << LAT_REQUIRED_STABLE_COUNT
                          << " bursts = " << std::fixed << std::setprecision(2)
                          << aggregate_duration << "s, cycles: "
                          << std::fixed << std::setprecision(0) << total_cycles
                          << ", instr: " << total_instructions
                          << ", tlb1miss: " << total_tlb1miss
                          << ", tlb2miss: " << total_tlb2miss
                          << ", Freq: " << std::fixed << std::setprecision(2)
                          << result.point_cpu_freq_ghz << " GHz)\n";
                }
            }
        } else if (config_.instruction_sampling && sampler_stats.samples > 0) {
            result.latency_ns = sampler_stats.iqr_mean_ns > 0.0
                ? sampler_stats.iqr_mean_ns : sampler_stats.mean_ns;

            result.sampler_samples = sampler_stats.samples;
            result.sampler_mean_ns = sampler_stats.mean_ns;
            result.sampler_iqr_mean_ns = sampler_stats.iqr_mean_ns;
            result.sampler_min_ns = sampler_stats.min_ns;
            result.sampler_median_ns = sampler_stats.median_ns;
            result.sampler_p90_ns = sampler_stats.p90_ns;
            result.sampler_p95_ns = sampler_stats.p95_ns;
            result.sampler_p99_ns = sampler_stats.p99_ns;
            result.sampler_p99_9_ns = sampler_stats.p99_9_ns;
            result.sampler_max_ns = sampler_stats.max_ns;

            result.sampler_mean_cycles = sampler_stats.mean_cycles;
            result.sampler_iqr_mean_cycles = sampler_stats.iqr_mean_cycles;
            result.sampler_min_cycles = sampler_stats.min_cycles;
            result.sampler_median_cycles = sampler_stats.median_cycles;
            result.sampler_p90_cycles = sampler_stats.p90_cycles;
            result.sampler_p95_cycles = sampler_stats.p95_cycles;
            result.sampler_p99_cycles = sampler_stats.p99_cycles;
            result.sampler_p99_9_cycles = sampler_stats.p99_9_cycles;
            result.sampler_max_cycles = sampler_stats.max_cycles;

            result.sampler_raw_latencies = std::move(sampler_stats.raw_latencies);

            if (verbose2) {
                double freq_ghz = result.point_cpu_freq_ghz;
                std::cout << "      Sampler Latency Stats (freq=" << std::fixed << std::setprecision(2) << freq_ghz << " GHz):\n";
                std::cout << "        Samples : " << sampler_stats.samples << "\n";
                std::cout << "        Mean    : " << std::fixed << std::setprecision(1) << sampler_stats.mean_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.mean_ns << " ns)\n";
                std::cout << "        IQR Mean: " << std::fixed << std::setprecision(1) << sampler_stats.iqr_mean_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.iqr_mean_ns << " ns)\n";
                std::cout << "        Min     : " << std::fixed << std::setprecision(1) << sampler_stats.min_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.min_ns << " ns)\n";
                std::cout << "        Median  : " << std::fixed << std::setprecision(1) << sampler_stats.median_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.median_ns << " ns)\n";
                std::cout << "        P90     : " << std::fixed << std::setprecision(1) << sampler_stats.p90_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.p90_ns << " ns)\n";
                std::cout << "        P95     : " << std::fixed << std::setprecision(1) << sampler_stats.p95_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.p95_ns << " ns)\n";
                std::cout << "        P99     : " << std::fixed << std::setprecision(1) << sampler_stats.p99_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.p99_ns << " ns)\n";
                std::cout << "        P99.9   : " << std::fixed << std::setprecision(1) << sampler_stats.p99_9_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.p99_9_ns << " ns)\n";
                std::cout << "        Max     : " << std::fixed << std::setprecision(1) << sampler_stats.max_cycles << " cycles (" << std::fixed << std::setprecision(2) << sampler_stats.max_ns << " ns)\n";
            }
        } else {
            std::cerr << "Warning: No valid latency measurements found" << std::endl;
            result.latency_ns = 0.0;
            if (ptrchase_manager_) {
                std::cerr << "    Forcing ptr_chase restart due to missing latency data" << std::endl;
                ptrchase_manager_->cleanup();
            }
            std::cerr << "    Returning false to trigger retry..." << std::endl;
            return BenchmarkStatus::FAILURE;
        }
        
        if (verbose2) {
            std::cout << "    Cleaning up processes...";
            std::cout << "\n\n";
        }
        traffic_gen_manager_->terminate_traffic_gen(traffic_gen_pid);
        
        int cleanup_wait = 0;
        while (traffic_gen_manager_->is_traffic_gen_running(traffic_gen_pid) && cleanup_wait < 20) {
            traffic_gen_manager_->terminate_traffic_gen(traffic_gen_pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cleanup_wait++;
        }
        
        if (cleanup_wait >= 20) {
            std::cerr << "    ERROR: Failed to kill TrafficGen processes after 4 seconds" << std::endl;
        }
        if (!config_.instruction_sampling && result.latency_direct_ns <= 0.0) {
            std::cerr << "Nonpositive direct latency after TLB correction; rejecting repetition.\n";
            return BenchmarkStatus::FAILURE;
        }
    } else {
        std::cerr << "Warning: Failed to launch TrafficGen stress for ratio " << ratio_pct << "%, pause " << pause << std::endl;
    }

    return BenchmarkStatus::SUCCESS;
}


double BenchmarkExecutor::calculate_burst_latency(const PerfBurstCounters& burst) const {
    const auto timing = resolve_pointer_chase_timing(burst.cycles, burst.pointer_chase_thread_ns);
    // Cycle-valued TLB counters cannot be converted without valid cycle timing.
    if (timing.thread_ns <= 0.0 || (!caps_.isAppleSilicon() && timing.frequency_ghz <= 0.0)) return 0.0;
    const double tlb_overhead_ns = counter_strategy_->computeTlbOverheadNs(
        burst.tlb1miss, burst.tlb2miss, timing.frequency_ghz, cached_tlb_hit_latency_ns_);
    return timing.latencyNs(burst.accesses, tlb_overhead_ns);
}

