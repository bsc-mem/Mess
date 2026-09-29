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

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include "BenchmarkConfig.h"
#include "SystemInfo.h"
#include "PtrchasePerfHelper.h"
#include "Utils.h"
#include "platform/Ipc.h"
#include "platform/Process.h"
#include <string>
#include <vector>

/**
 * @file ProcessManager.h
 * @brief Lifecycle helpers for pointer-chase and traffic-generator subprocesses.
 */

/**
 * @brief Manages the dedicated pointer-chase subprocess used for latency measurements.
 */
/**
 * @brief Manages the lifecycle of the pointer chasing tool used for latency measurements.
 * 
 * If the benchmark is configured to use software pointer chasing instead of hardware instruction 
 * sampling (PEBS/SPE), this class manages the external `ptr_chase` binary. It establishes 
 * named pipes and semaphores to tightly coordinate start/stop boundaries between the traffic 
 * generator load and the latency probe.
 */
class PtrChaseProcessManager {
public:
    /** @brief Creates a pointer-chase manager for the active benchmark configuration. */
    PtrChaseProcessManager(const BenchmarkConfig& config, const system_info& sys_info);
    ~PtrChaseProcessManager();

    bool ensure_running();
    bool wait_for_ready(int timeout_seconds = 30);
    void cleanup();

    void cleanup_perf_counters();

    bool ensure_semaphores();
    void drain_done_sem();
    void drain_start_sem();
    bool wait_for_done(int timeout_ms);

    platform::ProcessId pid() const { return ptr_chase_pid_; }

    platform::Semaphore& start_sem() { return ptrchase_start_sem_; }
    platform::Semaphore& done_sem() { return ptrchase_done_sem_; }
    const std::string& pipe_path() const { return pipe_path_; }

private:
    const BenchmarkConfig& config_;

    platform::ProcessId ptr_chase_pid_;
    platform::Semaphore ptrchase_start_sem_;
    platform::Semaphore ptrchase_done_sem_;
    ptrchase_perf::PtrchasePerfFDs ptrchase_perf_fds_;

    std::string start_sem_name_;
    std::string done_sem_name_;
    std::string ready_flag_path_;
    std::string pipe_path_;
    std::string unique_id_;

#if defined(_WIN32)
    platform::ProcessGroup ptr_chase_group_;
#endif

    void generate_unique_names();
    bool launch_process();
};

/**
 * @brief Captures the outcome of preparing a traffic-generator launch.
 */
struct TrafficGenPreparation {
    platform::ProcessId pid = 0;
    bool reused_existing = false;
};

/**
 * @brief Launches, reuses, and tears down traffic-generator subprocesses.
 */
/**
 * @brief Manages the lifecycle and state of background memory traffic generator processes.
 * 
 * The traffic generators are spawned as independent subprocesses that execute the generated 
 * inline assembly loops to saturate memory bandwidth. This manager handles compiling the kernel, 
 * pinning the threads to the correct cores/NUMA nodes, tracking their PIDs, and ensuring they 
 * remain active (via persistent mode) or are cleanly restarted between benchmark iterations.
 */
class TrafficGenProcessManager {
public:
    TrafficGenProcessManager(const BenchmarkConfig& config, const system_info& sys_info);
    ~TrafficGenProcessManager();

    bool prepare_traffic_gen(double ratio_pct,
                           int pause,
                           int traffic_gen_cores,
                           const std::vector<int>& traffic_gen_mem_nodes,
                           const std::string& traffic_gen_log_file,
                           TrafficGenPreparation& prep,
                           ExecutionMode mode = ExecutionMode::MULTISEQUENTIAL,
                           bool reuse_existing = false);

    bool relaunch_traffic_gen(double ratio_pct,
                         int pause,
                         int traffic_gen_cores,
                         const std::vector<int>& traffic_gen_mem_nodes,
                         const std::string& traffic_gen_log_file,
                         ExecutionMode mode = ExecutionMode::MULTISEQUENTIAL);

    bool is_traffic_gen_running(platform::ProcessId pid) const;
    void terminate_traffic_gen(platform::ProcessId pid);
    void kill_all_traffic_gen();

    bool wait_for_traffic_gen_ready(int timeout_seconds = 120);

    platform::ProcessId current_traffic_gen_pid() const { return current_traffic_gen_pid_; }
    platform::ProcessId active_traffic_gen_pid() const { return active_traffic_gen_pid_; }
    std::vector<platform::ProcessId> traffic_gen_worker_pids() const;

private:
    const BenchmarkConfig& config_;
    const system_info& sys_info_;

    platform::ProcessId current_traffic_gen_pid_;
    platform::ProcessId active_traffic_gen_pid_;

    bool persistent_traffic_gen_valid_;
    double persistent_ratio_pct_;
    int persistent_pause_;
    ExecutionMode persistent_mode_;
    int persistent_traffic_gen_cores_;
    uint64_t persistent_traffic_gen_array_elements_;
    std::vector<int> persistent_traffic_gen_mem_nodes_;

    bool launch_traffic_gen_multiseq(double ratio_pct, int pause, int cores, const std::vector<int>& mem_nodes, const std::string& log_file, platform::ProcessId& out_pid) const;
    

    bool launch_traffic_gen_impl(double ratio_pct, int pause, int cores, const std::vector<int>& mem_nodes, const std::string& log_file, platform::ProcessId& out_pid) const;
    
#if USE_MPI_SPAWN
    bool launch_traffic_gen_mpi(double ratio_pct, int pause, int cores, const std::vector<int>& mem_nodes, const std::string& log_file, platform::ProcessId& out_pid) const;
#endif
    bool read_traffic_gen_pid(const std::string& pid_filename, platform::ProcessId& traffic_gen_pid) const;
    bool cleanup_zombie_traffic_gen() const;
    std::string get_membind_arg(int default_node) const;

    std::string unique_id_;
    std::string ready_fifo_path_;
    mutable int expected_ready_count_;
    mutable std::string last_traffic_gen_log_file_;

    void generate_unique_names();
    void print_recent_traffic_gen_log_excerpt() const;
    uint64_t active_traffic_gen_array_elements() const;

#if defined(_WIN32)
    mutable platform::ProcessGroup traffic_gen_group_;
#endif

    mutable std::vector<std::string> cached_core_list_;
    mutable int cached_traffic_gen_cores_;
    mutable std::vector<std::string> cached_explicit_cores_;
    
    bool resolve_core_list(int traffic_gen_cores, const std::vector<std::string>& explicit_cores, std::vector<std::string>& out_cores) const;
};

#endif
