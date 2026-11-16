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

#include "measurement/BandwidthRecheck.h"
#include "BenchmarkExecutor.h"
#include "ExecutionModes.h"
#include "measurement/PointSampling.h"
#include "measurement/AdaptiveSweep.h"
#include "platform/Console.h"   // platform::glyph
#include "measurement/CpuFrequencyProbe.h"
#include "measurement/PointerChaseTiming.h"
#include <array>
#include "Measurement.h"
#include "measurement/WorkerReadiness.h"
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
volatile std::sig_atomic_t BenchmarkExecutor::stop_requested_ = 0;

BenchmarkExecutor::BenchmarkExecutor(const BenchmarkConfig& config, const system_info& sys_info, const CPUCapabilities& caps,
                                   double tlb_latency_ns, int cache_line_size)
    : config_(config),
      sys_info_(sys_info),
      caps_(caps),
      bw_bytes_per_cas_(cache_line_size),
      cache_line_size_(cache_line_size),
      tlb_latency_ns_(tlb_latency_ns),
      plotter_caps_(caps),
      counter_strategy_(&BandwidthCounterStrategy::instance()),
      traffic_gen_manager_(std::make_unique<TrafficGenProcessManager>(config, sys_info)),
      ptrchase_manager_(std::make_unique<PtrChaseProcessManager>(config, sys_info)),
      progress_tracker_(std::make_unique<ProgressTracker>(0)) {

    if (config_.instruction_sampling) {
        instruction_sampler_ = InstructionSamplerFactory::create(
            SamplerBackend::AUTO, 50, 0, config_.verbosity);
        if (!instruction_sampler_ && config_.verbosity >= 1) {
            std::cerr << "    Warning: No instruction sampler backend available (PEBS/SPE)\n";
        }
    }

    for (ExecutionMode mode : enabled_modes(config_)) {
        initialize_mode_resources(mode, config_.output_root + "/" + mode_directory(mode));
    }

    cached_tlb_hit_latency_ns_ = tlb_latency_ns;

    uint64_t tlb1_raw = 0, tlb2_raw = 0;
    bool use_tlb1 = false, use_tlb2 = false;
    ptrchase_perf::select_tlb_events_for_ptrchase(sys_info, tlb1_raw, tlb2_raw, use_tlb1, use_tlb2);
    
    auto arch = ArchitectureRegistry::instance().getArchitecture(caps);
    auto fmt_strategy = arch ? arch->createCounterStrategy(caps) : nullptr;
    const BandwidthCounterStrategy* fmt = fmt_strategy ? fmt_strategy.get() : counter_strategy_;
    std::string tlb1_str = use_tlb1 ? fmt->formatTlbEventName(0, tlb1_raw) : "";
    std::string tlb2_str = use_tlb2 ? fmt->formatTlbEventName(1, tlb2_raw) : "";
    expected_tlb1_event_ = tlb1_str;
    expected_tlb2_event_ = tlb2_str;

    double upi_scaling_factor = 0.0;
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
    plotter_caps_ = plotter_caps;
    plotter_binding_type_ = binding_type;
    plotter_upi_scaling_factor_ = upi_scaling_factor;
    plotter_page_walk_lat_ns_ = fmt ? fmt->getPageWalkLatencyNs() : -1.0;
    for (const auto& entry : mode_resources_) write_mode_metadata(entry.first);
}

BenchmarkExecutor::~BenchmarkExecutor() {
    force_cleanup();
    shutdown_measurement_backends();
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

bool BenchmarkExecutor::shutdown_measurement_backends() {
    if (measurement_backends_shutdown_) {
        return measurement_backends_shutdown_ok_;
    }

    bool shutdown_ok = measurement_backends_shutdown_ok_;
    for (auto& entry : mode_resources_) {
        if (entry.second.bandwidth && !entry.second.bandwidth->shutdown()) {
            shutdown_ok = false;
        }
    }

    measurement_backends_shutdown_ok_ = shutdown_ok;
    measurement_backends_shutdown_ = true;
    return measurement_backends_shutdown_ok_;
}

void BenchmarkExecutor::force_cleanup() {
    if (ptrchase_manager_) {
        ptrchase_manager_->cleanup();
    }
    if (traffic_gen_manager_) {
        traffic_gen_manager_->kill_all_traffic_gen();
    }
}

double BenchmarkExecutor::point_bandwidth_gbps(CounterType type, long long rd, long long wr, double seconds) const {
    double scaling_factor = 1.0;
    if (type == CounterType::UPI_FLITS) {
        auto arch = ArchitectureRegistry::instance().getArchitecture(caps_);
        scaling_factor = arch ? arch->getUpiScalingFactor(caps_) : 1.0 / 9.0;
    }
    return BandwidthMeasurer::calculate_bandwidth_gbps(rd, wr, seconds, type, bw_bytes_per_cas_, scaling_factor);
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

std::string BenchmarkExecutor::mode_label(ExecutionMode mode) const {
    (void)mode;
    return "MULTISEQ";
}

const BenchmarkExecutor::ModeResources* BenchmarkExecutor::resources_for_mode(ExecutionMode mode) const {
    const auto found = mode_resources_.find(mode);
    return found == mode_resources_.end() ? nullptr : &found->second;
}

MeasurementStorage* BenchmarkExecutor::storage_for_mode(ExecutionMode mode) const {
    const auto* resources = resources_for_mode(mode);
    return resources ? resources->storage.get() : nullptr;
}

OutlierDetector* BenchmarkExecutor::outlier_detector_for_mode(ExecutionMode mode) const {
    const auto* resources = resources_for_mode(mode);
    return resources ? resources->outliers.get() : nullptr;
}

BandwidthMeasurer* BenchmarkExecutor::bandwidth_measurer_for_mode(ExecutionMode mode) const {
    const auto* resources = resources_for_mode(mode);
    return resources ? resources->bandwidth.get() : nullptr;
}


void BenchmarkExecutor::initialize_mode_resources(ExecutionMode mode, const std::string& root) {
    ModeResources resources;
    resources.storage = std::make_unique<MeasurementStorage>(config_, cache_line_size_, config_.verbosity, root);
    resources.latency = std::make_unique<LatencyMeasurer>(config_, resources.storage.get(), ptrchase_manager_.get(),
        [this]() { return get_memory_binding_nodes(); }, cache_line_size_);
    resources.outliers = std::make_unique<OutlierDetector>(config_);
    resources.bandwidth = create_bandwidth_measurer(config_, sys_info_, caps_, resources.storage.get(),
        traffic_gen_manager_.get(), [this]() { return get_memory_binding_nodes(); }, mode);
    if (!resources.bandwidth) throw std::runtime_error("Failed to create " + mode_label(mode) + " bandwidth measurer");
    resources.bandwidth->set_cancellation_check([]() { return BenchmarkExecutor::stop_requested(); });
    // Destroy borrowers before storage when changing a mode's output directory.
    const auto previous = mode_resources_.find(mode);
    if (previous != mode_resources_.end()) {
        if (!previous->second.bandwidth->shutdown()) measurement_backends_shutdown_ok_ = false;
        mode_resources_.erase(previous);
    }
    mode_resources_.emplace(mode, std::move(resources));
}

void BenchmarkExecutor::write_mode_metadata(ExecutionMode mode) {
    const auto* resources = resources_for_mode(mode);
    if (!resources) return;
    resources->storage->set_bytes_per_cas(bw_bytes_per_cas_);
    resources->latency->setExpectedTlbEvents(expected_tlb1_event_, expected_tlb2_event_);
    const int workers = resolved_traffic_gen_cores();
    int worker_cores = workers;
    resources->storage->write_plotter_file(plotter_caps_, plotter_binding_type_, tlb_latency_ns_, cache_line_size_,
        worker_cores,
        plotter_upi_scaling_factor_, plotter_page_walk_lat_ns_);
}

int BenchmarkExecutor::resolved_traffic_gen_cores() const {
    return get_traffic_gen_worker_count(config_, sys_info_);
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
    results_.erase(std::remove_if(results_.begin(), results_.end(), [&](const BenchmarkResult& result) {
        return same_point(result, ratio_pct, pause, mode);
    }), results_.end());
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
            if (stop_requested()) return false;
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
            if (stop_requested()) {
                progress_tracker_->finish_iteration();
                return false;
            }
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
        storage_for_mode(mode)->cleanup_measurement_files(ratio_pct, pause);
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
        if (stop_requested()) throw std::runtime_error("Benchmark interrupted");
        auto point = adaptive_probe(ratio_pct, pause, modes[index], total_global_measurements,
                                    peak, count, replace);
        return measurement::MeasuredPoint{point.stats, std::move(point.results)};
    };
    return measurement::run_adaptive_sweep(config_, labels, *progress_tracker_, total_global_measurements,
        measure, [&](const std::vector<BenchmarkResult>& results) { account_measured_point_progress(results); });
}

bool BenchmarkExecutor::run_fixed_pauses_for_ratio(double ratio_pct, const std::vector<ExecutionMode>& modes, int& total_global_measurements) {
    const std::vector<int>& pauses = config_.pauses;
    std::map<ExecutionMode, std::vector<double>> bw_history;
    std::set<int> backtracked_pauses;

    for (int pause_idx = 0; pause_idx < static_cast<int>(pauses.size()); ++pause_idx) {
        const int pause = pauses[pause_idx];
        for (ExecutionMode mode : modes) {
            std::vector<double>& history = bw_history[mode];
            const bool revisit = history.size() > static_cast<size_t>(pause_idx);
            std::vector<BenchmarkResult> current_point_results;
            if (!resolve_point_measurements(ratio_pct, pause, mode, total_global_measurements,
                                            current_point_results, 0.0, false, !revisit, revisit)) {
                return false;
            }

            double total_bw = 0;
            for (const auto& res : current_point_results) {
                total_bw += res.bandwidth_mbps;
            }
            const double avg_bw = total_bw / current_point_results.size();
            if (!revisit) history.resize(pause_idx + 1, 0.0);
            history[pause_idx] = avg_bw;

            if (pause_idx == 0 || pause <= pauses[pause_idx - 1]) continue;
            const double prev_bw = history[pause_idx - 1];
            if (!(prev_bw > 0.1 && avg_bw > prev_bw * 1.5)) continue;

            if (backtracked_pauses.count(pause)) {
                if (config_.verbosity >= 1) {
                    std::cout << "    " << platform::glyph::kWarning << " Anomaly persisted after retry: Higher pause (" << pause
                              << ") still has higher BW. Ignoring and continuing...\n";
                }
                continue;
            }
            if (config_.verbosity >= 1) {
                std::cout << "    " << platform::glyph::kWarning << " Anomaly detected: Higher pause (" << pause
                          << ") has significantly higher BW (" << std::fixed << std::setprecision(2) << avg_bw/1000.0
                          << " GB/s) than previous pause (" << pauses[pause_idx-1]
                          << ", " << prev_bw/1000.0 << " GB/s). (+50% threshold exceeded)\n";
                std::cout << "    " << platform::glyph::kRetry << " Backtracking to repeat pause " << pauses[pause_idx-1] << "...\n";
            }
            backtracked_pauses.insert(pause);
            pause_idx -= 2;
            break;
        }
    }
    return true;
}

bool BenchmarkExecutor::run() {

    results_.clear();
    if (stop_requested()) return false;

    std::vector<double> ratios = config_.ratios_pct;
    if (ratios.empty()) {
        for (int i = 100; i >= 0; i -= 2) {
            ratios.push_back(static_cast<double>(i));
        }
    }

    const std::vector<ExecutionMode> modes = enabled_modes(config_);
    const int total_runs = static_cast<int>(ratios.size());

    if (!ptrchase_manager_->ensure_running()) {
        std::cerr << "ERROR: Failed to launch persistent pointer chase process" << std::endl;
        return false;
    }

    const bool adaptive = config_.pauses.empty();
    const int num_modes = static_cast<int>(modes.size());
    const AdaptiveTier tier = CurveTracer::resolve_tier(config_.adaptive_tier, config_.adaptive_point_count);
    const int points_per_run = adaptive ? tier.max_budget : static_cast<int>(config_.pauses.size());
    int total_global_measurements = total_runs * points_per_run * config_.point_reps * num_modes;
    progress_tracker_->start_benchmark(total_global_measurements);
    if (adaptive) progress_tracker_->set_adaptive_info(total_runs * num_modes, config_.point_reps, tier.max_budget, num_modes);

    int current_ratio = 0;
    auto run_ratios = [&]() {
        for (double ratio_pct : ratios) {
            if (stop_requested()) return false;
            current_ratio++;
            progress_tracker_->set_current_ratio(ratio_pct, current_ratio, total_runs);
            if (config_.verbosity >= 2) {
                if (adaptive) std::cout << platform::clear_line();
                std::cout << "\n\n=== " << ratio_pct << "% loads (" << current_ratio << "/" << total_runs
                          << " load-ratio runs) ===" << std::endl;
            }
            const bool ok = adaptive ? run_adaptive_for_ratio(ratio_pct, modes, total_global_measurements)
                                     : run_fixed_pauses_for_ratio(ratio_pct, modes, total_global_measurements);
            if (!ok) return false;
        }
        return true;
    };

    if (!run_ratios()) return false;

    if (config_.verbosity >= 2) {
        std::cout << "\n\n=== Final Cleanup ===" << std::endl;
        std::cout << "Terminating processes..." << std::endl;
    }
    force_cleanup();
    if (config_.verbosity >= 2) {
        std::cout << "Cleanup complete" << std::endl;
    }

    return true;
}

std::vector<int> BenchmarkExecutor::get_memory_binding_nodes() const {
    return config_.memory_bind_nodes;
}

namespace {

constexpr size_t kMaxLatencyBursts = 20;
constexpr int kMaxLatencyRetries = 3;
constexpr double kLatencyStabilityCv = 0.03;

void print_burst_stats(const PerfBurstCounters& b, size_t index, double latency_ns) {
    std::cout << "      [Latency Burst " << index << "] "
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

bool sum_last_bursts(const std::deque<PerfBurstCounters>& samples, int count, PerfBurstCounters& totals) {
    int summed = 0;
    for (auto it = samples.rbegin(); it != samples.rend() && summed < count; ++it, ++summed) {
        totals.cycles += it->cycles;
        totals.instructions += it->instructions;
        totals.tlb1miss += it->tlb1miss;
        totals.tlb2miss += it->tlb2miss;
        totals.duration_s += it->duration_s;
        totals.pointer_chase_thread_ns += it->pointer_chase_thread_ns;
        totals.accesses += it->accesses;
        if (it->pointer_chase_thread_ns <= 0.0 || it->accesses <= 0.0) return false;
        if (summed == 0) {
            totals.tlb1_event_name = it->tlb1_event_name;
            totals.tlb2_event_name = it->tlb2_event_name;
        }
    }
    return true;
}

void copy_sampler_stats(InstructionSamplerStats stats, BenchmarkResult& result) {
    result.sampler_samples = stats.samples;
    result.sampler_mean_ns = stats.mean_ns;
    result.sampler_iqr_mean_ns = stats.iqr_mean_ns;
    result.sampler_min_ns = stats.min_ns;
    result.sampler_median_ns = stats.median_ns;
    result.sampler_p90_ns = stats.p90_ns;
    result.sampler_p95_ns = stats.p95_ns;
    result.sampler_p99_ns = stats.p99_ns;
    result.sampler_p99_9_ns = stats.p99_9_ns;
    result.sampler_max_ns = stats.max_ns;

    result.sampler_mean_cycles = stats.mean_cycles;
    result.sampler_iqr_mean_cycles = stats.iqr_mean_cycles;
    result.sampler_min_cycles = stats.min_cycles;
    result.sampler_median_cycles = stats.median_cycles;
    result.sampler_p90_cycles = stats.p90_cycles;
    result.sampler_p95_cycles = stats.p95_cycles;
    result.sampler_p99_cycles = stats.p99_cycles;
    result.sampler_p99_9_cycles = stats.p99_9_cycles;
    result.sampler_max_cycles = stats.max_cycles;

    result.sampler_raw_latencies = std::move(stats.raw_latencies);
}

void print_sampler_stats(const InstructionSamplerStats& stats, double freq_ghz) {
    const auto row = [](const char* label, double cycles, double ns) {
        std::cout << "        " << label << ": " << std::fixed << std::setprecision(1) << cycles << " cycles (" << std::fixed << std::setprecision(2) << ns << " ns)\n";
    };
    std::cout << "      Sampler Latency Stats (freq=" << std::fixed << std::setprecision(2) << freq_ghz << " GHz):\n";
    std::cout << "        Samples : " << stats.samples << "\n";
    row("Mean    ", stats.mean_cycles, stats.mean_ns);
    row("IQR Mean", stats.iqr_mean_cycles, stats.iqr_mean_ns);
    row("Min     ", stats.min_cycles, stats.min_ns);
    row("Median  ", stats.median_cycles, stats.median_ns);
    row("P90     ", stats.p90_cycles, stats.p90_ns);
    row("P95     ", stats.p95_cycles, stats.p95_ns);
    row("P99     ", stats.p99_cycles, stats.p99_ns);
    row("P99.9   ", stats.p99_9_cycles, stats.p99_9_ns);
    row("Max     ", stats.max_cycles, stats.max_ns);
}


}

bool BenchmarkExecutor::start_point_traffic(MeasurementStorage& storage, double ratio_pct, int pause, ExecutionMode mode, bool reuse_traffic_gen, TrafficGenPreparation& prep) {
    if (ratio_pct < 0 || ratio_pct > 100) {
        std::cerr << "ERROR: Invalid ratio_pct value: " << ratio_pct << std::endl;
        return false;
    }

    if (pause < 0) {
        std::cerr << "ERROR: Invalid pause value: " << pause << std::endl;
        return false;
    }

    const std::string traffic_gen_log_file = storage.traffic_gen_log_file_path(ratio_pct, pause);

    const int traffic_gen_cores = resolved_traffic_gen_cores();
    static bool warned_cross_socket_total_cores = false;
    if (sys_info_.socket_count > 0 && config_.traffic_gen_cores > sys_info_.sockets[0].core_count && !warned_cross_socket_total_cores) {
        std::cerr << "WARNING: --total-cores is larger than one socket. Mess profiles single-socket curves; spreading traffic across sockets affects the measurements and is not the expected way of running." << std::endl;
        warned_cross_socket_total_cores = true;
    }

    if (traffic_gen_cores <= 0 || traffic_gen_cores > 1024) {
        std::cerr << "ERROR: Invalid traffic_gen_cores value: " << traffic_gen_cores << std::endl;
        return false;
    }

    if (!traffic_gen_manager_->prepare_traffic_gen(ratio_pct, pause, traffic_gen_cores, get_memory_binding_nodes(), traffic_gen_log_file, prep, mode, reuse_traffic_gen)) {
        return false;
    }

    if (prep.pid <= 0) {
        std::cerr << "ERROR: Failed to launch TrafficGen stress for ratio " << ratio_pct << "%, pause " << pause << std::endl;
        return false;
    }

    if (!prep.reused_existing &&
        !measurement::wait_for_ready_workers(*traffic_gen_manager_, 120, []() { return BenchmarkExecutor::stop_requested(); })) {
        if (stop_requested()) return false;
        std::cerr << "ERROR: TrafficGen workers did not initialize completely\n";
        return false;
    }
    return true;
}

bool BenchmarkExecutor::last_bursts_stable(const std::deque<PerfBurstCounters>& samples) const {
    if (samples.size() < static_cast<size_t>(LAT_REQUIRED_STABLE_COUNT)) return false;

    std::array<double, LAT_REQUIRED_STABLE_COUNT> latencies{};
    double sum = 0.0;
    auto it = samples.rbegin();
    for (double& latency : latencies) {
        latency = calculate_burst_latency(*it++);
        if (latency <= 0.0) return false;
        sum += latency;
    }

    const double mean = sum / LAT_REQUIRED_STABLE_COUNT;
    double sq_sum = 0.0;
    for (double latency : latencies) {
        sq_sum += (latency - mean) * (latency - mean);
    }
    const double std_dev = std::sqrt(sq_sum / LAT_REQUIRED_STABLE_COUNT);
    const double cv = (mean > 0) ? (std_dev / mean) : 1.0;
    return cv < kLatencyStabilityCv;
}

bool BenchmarkExecutor::collect_latency_bursts(LatencyMeasurer& latency, std::deque<PerfBurstCounters>& samples) {
    const bool verbose1 = config_.verbosity >= 1;
    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;

    for (int attempt = 0; attempt < kMaxLatencyRetries; ++attempt) {
        if (stop_requested()) return false;
        samples.clear();

        if (!ptrchase_manager_->ensure_running()) {
            std::cerr << "ERROR: Cannot ensure ptr_chase is running" << std::endl;
            continue;
        }

        if (attempt > 0 && verbose1) {
            std::cout << "    Retrying latency measurement (attempt " << (attempt + 1) << "/" << kMaxLatencyRetries << ")...\n";
        }
        if (verbose2) {
            std::cout << "    Waiting for stable latency bursts...\n";
        }

        while (samples.size() < kMaxLatencyBursts) {
            if (stop_requested()) return false;
            if (latency.burst_failed()) {
                std::cerr << "    Pointer-chase burst failed: " << latency.burst_error() << '\n';
                break;
            }
            if (!latency.is_burst_running()) {
                latency.start_burst_async();
                continue;
            }
            PerfBurstCounters burst;
            if (!latency.try_collect_burst_async(burst)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            samples.push_back(burst);
            if (verbose3) print_burst_stats(burst, samples.size(), calculate_burst_latency(burst));
            if (last_bursts_stable(samples)) {
                if (verbose2) {
                    std::cout << "    Latency bursts stabilized\n";
                }
                return true;
            }
            if (samples.size() < kMaxLatencyBursts) latency.start_burst_async();
        }

        std::cerr << "    Warning: Failed to collect valid latency bursts." << std::endl;
        if (ptrchase_manager_) {
            std::cerr << "    Forcing ptr_chase restart..." << std::endl;
            ptrchase_manager_->cleanup();
            latency.reset();
        }
    }
    return false;
}

BenchmarkExecutor::BenchmarkStatus BenchmarkExecutor::recheck_bandwidth(const BandwidthMeasurer& bandwidth, long long& rd, long long& wr, double& seconds, bool pool_stabilized_window) {
    const bool is_pcm = is_pcm_cxl_selection(bandwidth.get_counter_selection());
    if (config_.measurer == "vtune" || is_pcm) {
        if (config_.verbosity >= 3) {
            std::cout << (is_pcm ? "    BW re-check skipped for PCM (using stabilized streaming sample)\n"
                                 : "    BW re-check skipped for VTune to avoid extra collection overhead\n");
        }
        return BenchmarkStatus::SUCCESS;
    }

    const std::vector<int> mem_nodes = get_memory_binding_nodes();
    measurement::BandwidthReading first;
    if (!bandwidth.sample_bandwidth(first.read, first.write, first.seconds, mem_nodes)) {
        if (seconds <= 0.0 && rd == 0 && wr == 0) {
            std::cerr << "    Error: Final bandwidth sampling failed and no valid bandwidth measurement is available." << std::endl;
            return BenchmarkStatus::FAILURE;
        }
        return BenchmarkStatus::SUCCESS;
    }

    const CounterType type = bandwidth.get_counter_selection().type;
    const double initial_bw = point_bandwidth_gbps(type, rd, wr, seconds);
    const double final_bw = point_bandwidth_gbps(type, first.read, first.write, first.seconds);
    const auto recheck = measurement::confirm_bandwidth(
        initial_bw, first,
        [&](measurement::BandwidthReading& next) {
            return bandwidth.sample_bandwidth(next.read, next.write, next.seconds, mem_nodes);
        },
        [&](const measurement::BandwidthReading& reading) {
            return point_bandwidth_gbps(type, reading.read, reading.write, reading.seconds);
        }, bandwidth.stability_noise_floor_gbps());

    if (recheck.status != measurement::BandwidthRecheckStatus::Accepted) {
        if (config_.verbosity >= 1) {
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

    if (pool_stabilized_window) {
        rd += recheck.reading.read;
        wr += recheck.reading.write;
        seconds += recheck.reading.seconds;
    } else {
        rd = recheck.reading.read;
        wr = recheck.reading.write;
        seconds = recheck.reading.seconds;
    }
    if (config_.verbosity >= 3) {
        std::cout << "    BW re-check: " << final_bw << " GB/s; accepted "
                  << recheck.samples << "-sample mean " << recheck.bandwidth << " GB/s";
        if (pool_stabilized_window) {
            std::cout << "; reporting " << point_bandwidth_gbps(type, rd, wr, seconds)
                      << " GB/s over " << seconds << " s";
        }
        std::cout << "\n";
    }
    return BenchmarkStatus::SUCCESS;
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
    if (stop_requested()) return BenchmarkStatus::FAILURE;

    MeasurementStorage* measurement_storage_ = storage_for_mode(ExecutionMode::MULTISEQUENTIAL);
    BandwidthMeasurer* bandwidth_measurer_ = bandwidth_measurer_for_mode(ExecutionMode::MULTISEQUENTIAL);
    LatencyMeasurer* latency_measurer_ = resources_for_mode(ExecutionMode::MULTISEQUENTIAL)->latency.get();

    result.repetitions = config_.point_reps;
    result.mode = mode;

    measurement_storage_->ensure_directories_exist();

    if (config_.instruction_sampling && instruction_sampler_) {
        instruction_sampler_->cleanup();
    }

    TrafficGenPreparation traffic_gen_prep;
    if (!start_point_traffic(*measurement_storage_, ratio_pct, pause, mode, reuse_traffic_gen, traffic_gen_prep)) {
        return BenchmarkStatus::FAILURE;
    }
    const bool reuse_existing_traffic_gen = traffic_gen_prep.reused_existing;
    const platform::ProcessId traffic_gen_pid = traffic_gen_prep.pid;

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


    if (verbose2) {
        std::cout << "    Waiting for TrafficGen to stabilize...\n";
    }

    int traffic_gen_samples = 0;
    long long bw_cas_rd = 0, bw_cas_wr = 0;
    double bw_elapsed = 0;
    const double sampling_interval_ms = bandwidth_measurer_->sampling_interval_ms();
    const double stabilization_timeout_samples = (60.0 * 1000.0) / sampling_interval_ms;

    std::vector<int> sampler_cores;
    bool sampler_started = false;
    int sampler_callback_count = 0;
    std::function<void()> on_sample_callback;
    if (config_.instruction_sampling && instruction_sampler_) {
        std::vector<std::string> core_names;
        resolve_traffic_gen_core_list(resolved_traffic_gen_cores(), config_.traffic_gen_explicit_cores, core_names);
        for (const auto& name : core_names) {
            int cpu = -1;
            if (string_utils::parse_int_strict(name, cpu)) sampler_cores.push_back(cpu);
        }
        on_sample_callback = [&]() {
            if (sampler_started || ++sampler_callback_count < 3) return;
            sampler_started = instruction_sampler_->start_async(sampler_cores, traffic_gen_manager_->traffic_gen_worker_pids());
        };
    }

    const bool traffic_gen_stabilized = bandwidth_measurer_->wait_for_stabilization(
        traffic_gen_samples, bw_cas_rd, bw_cas_wr, bw_elapsed,
        pause, ratio_pct, reuse_existing_traffic_gen, on_sample_callback);

    if (stop_requested()) return BenchmarkStatus::FAILURE;

    // If BW didn't stabilize (60s timeout), abort this measurement and retry
    if (!traffic_gen_stabilized && (traffic_gen_samples > stabilization_timeout_samples || !bandwidth_measurer_->allows_unstable_samples())) {
        if (verbose1) {
            std::cout << "    " << platform::glyph::kWarning << " Aborting measurement due to BW instability (Time limit)\n";
        }
        if (config_.instruction_sampling && instruction_sampler_) {
            instruction_sampler_->stop_and_process(0.0);
        }
        return BenchmarkStatus::FAILURE;
    }

    if (verbose2 && traffic_gen_stabilized) {
        double wait_time = (sampling_interval_ms * traffic_gen_samples + 100.0);
        if (is_pcm_cxl_selection(bandwidth_measurer_->get_counter_selection()) && bw_elapsed > 0.0) {
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
    } else if (verbose2) {
        std::cout << "    Timeout, measuring last value\n";
    }

    if (!traffic_gen_stabilized && traffic_gen_samples == 0) {
        std::cerr << "    Error: Failed to collect any bandwidth samples for this point." << std::endl;
        return BenchmarkStatus::FAILURE;
    }


    std::deque<PerfBurstCounters> latency_samples;
    InstructionSamplerStats sampler_stats;
    bool latency_success = false;
    if (config_.instruction_sampling) {
        sampler_stats = instruction_sampler_->stop_and_process(result.point_cpu_freq_ghz);
        latency_success = sampler_stats.samples > 0;
        if (latency_success && verbose2) {
            std::cout << "    Sampler collected " << sampler_stats.samples << " latency samples"
                      << " | mean=" << std::fixed << std::setprecision(1) << sampler_stats.mean_ns << " ns"
                      << " | median=" << sampler_stats.median_ns << " ns"
                      << " | p99=" << sampler_stats.p99_ns << " ns" << std::endl;
        } else if (!latency_success && verbose1) {
            std::cout << "    Warning: No latency samples collected during BW stabilization window" << std::endl;
        }
    } else {
        latency_success = collect_latency_bursts(*latency_measurer_, latency_samples);
    }

    if (!latency_success) {
        if (!stop_requested()) {
            std::cerr << "    Error: Failed to measure latency after " << kMaxLatencyRetries << " attempts." << std::endl;
        }
        return BenchmarkStatus::FAILURE;
    }


    PerfBurstCounters burst_totals{};
    if (!config_.instruction_sampling) {
        if (!sum_last_bursts(latency_samples, LAT_REQUIRED_STABLE_COUNT, burst_totals)) {
            std::cerr << "Missing pointer-chase thread CPU timing; regenerate the pointer-chase binary.\n";
            return BenchmarkStatus::FAILURE;
        }
        if (verbose3) {
            std::cout << "      averaged last " << LAT_REQUIRED_STABLE_COUNT << " bursts\n";
        }
    }

    const BenchmarkStatus recheck_status = recheck_bandwidth(*bandwidth_measurer_, bw_cas_rd, bw_cas_wr, bw_elapsed,
        traffic_gen_stabilized && bandwidth_measurer_->pools_stabilized_window());
    if (recheck_status != BenchmarkStatus::SUCCESS) return recheck_status;

    if (verbose2) {
        std::cout << "    Both BW and latency stabilized\n";
    }

    double measured_ratio_pct = ratio_pct;
    if (bw_cas_rd + bw_cas_wr > 0) {
        measured_ratio_pct = (double)bw_cas_rd / (double)(bw_cas_rd + bw_cas_wr) * 100.0;
    }

    result.measured_ratio_pct = measured_ratio_pct;
    result.cycles = burst_totals.cycles;
    result.instructions = burst_totals.instructions;
    result.accesses = burst_totals.accesses;
    result.tlb1miss = burst_totals.tlb1miss;
    result.tlb2miss = burst_totals.tlb2miss;
    result.duration_s = burst_totals.duration_s;
    result.pointer_chase_thread_ns = burst_totals.pointer_chase_thread_ns;
    if (!config_.instruction_sampling) {
        result.point_cpu_freq_ghz = resolve_pointer_chase_timing(
            burst_totals.cycles, result.pointer_chase_thread_ns).frequency_ghz;
        result.latency_direct_ns = calculate_burst_latency(burst_totals);
    }
    result.using_hugepages = burst_totals.using_hugepages;
    result.bw_cas_rd = bw_cas_rd;
    result.bw_cas_wr = bw_cas_wr;
    result.bw_elapsed = bw_elapsed;
    result.traffic_gen_samples = traffic_gen_samples;

    result.tlb1_event_name = burst_totals.tlb1_event_name;
    result.tlb2_event_name = burst_totals.tlb2_event_name;

    result.extra_perf_values = bandwidth_measurer_->get_extra_perf_values();

    const auto& selection = bandwidth_measurer_->get_counter_selection();
    double measured_bw_gb_s = 0.0;
    if (bw_elapsed > 0) {
        measured_bw_gb_s = point_bandwidth_gbps(selection.type, bw_cas_rd, bw_cas_wr, bw_elapsed);
        result.bandwidth_mbps = measured_bw_gb_s * 1000.0;
    }

    if (verbose2) {
        std::cout << "      Measured Bandwidth: " << std::fixed << std::setprecision(2)
                  << measured_bw_gb_s << " GB/s\n";
    }
    if (verbose3 && selection.cas.has_read_write) {
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
    } else if (verbose3) {
        std::cout << "         (DRAM count: " << (bw_cas_rd + bw_cas_wr)
                  << ", elapsed: " << std::fixed << std::setprecision(2) << bw_elapsed << "s)\n";
    }

    if (!config_.instruction_sampling && result.latency_direct_ns > 0.0) {
        result.latency_ns = result.latency_direct_ns;
        if (verbose2) {
            std::cout << "      Measured Latency:   " << std::fixed << std::setprecision(2)
                      << result.latency_ns << " ns\n";
        }
        if (verbose3) {
            std::cout << "         (aggregated over " << LAT_REQUIRED_STABLE_COUNT
                      << " bursts = " << std::fixed << std::setprecision(2)
                      << burst_totals.duration_s << "s, cycles: "
                      << std::fixed << std::setprecision(0) << burst_totals.cycles
                      << ", instr: " << burst_totals.instructions
                      << ", tlb1miss: " << burst_totals.tlb1miss
                      << ", tlb2miss: " << burst_totals.tlb2miss
                      << ", Freq: " << std::fixed << std::setprecision(2)
                      << result.point_cpu_freq_ghz << " GHz)\n";
        }
    } else if (config_.instruction_sampling && sampler_stats.samples > 0) {
        result.latency_ns = sampler_stats.iqr_mean_ns > 0.0
            ? sampler_stats.iqr_mean_ns : sampler_stats.mean_ns;
        if (verbose2) print_sampler_stats(sampler_stats, result.point_cpu_freq_ghz);
        copy_sampler_stats(std::move(sampler_stats), result);
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

    if (!traffic_gen_manager_->is_traffic_gen_running(traffic_gen_pid)) {
        traffic_gen_manager_->kill_all_traffic_gen();
        std::cerr << "ERROR: A TrafficGen worker exited during measurement\n";
        return BenchmarkStatus::FAILURE;
    }
    if (!config_.persistent_traffic_gen) {
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

