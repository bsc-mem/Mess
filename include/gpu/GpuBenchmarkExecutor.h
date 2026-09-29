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

#ifndef GPU_BENCHMARK_EXECUTOR_H
#define GPU_BENCHMARK_EXECUTOR_H

#include "BenchmarkConfig.h"
#include "SystemDetection.h"
#include "Measurement.h"
#include "CurveTracer.h"
#include "utils/ProgressTracker.h"
#include "gpu/GpuWorker.h"
#include "measurement/AdaptiveSweep.h"
#include <map>
#include <vector>
#include <string>
#include <memory>

class GpuBenchmarkExecutor {
private:
    const BenchmarkConfig& config_;
    const CPUCapabilities& caps_;
    const GPUCapabilities& gpu_caps_;

    std::vector<BenchmarkResult> results_;
    std::vector<ExecutionMode> modes_;
    std::map<ExecutionMode, std::unique_ptr<MeasurementStorage>> storage_;
    std::unique_ptr<ProgressTracker> progress_tracker_;
    std::unique_ptr<GpuWorker> worker_;
    std::vector<std::string> metrics_;

    bool build_worker(const std::vector<int>& ratios, const std::string& directory) const;
    std::string compute_architecture() const;
    bool measure_sample(ExecutionMode mode, int ratio_pct, int pause, BenchmarkResult& result);
    measurement::MeasuredPoint adaptive_probe(ExecutionMode mode, int ratio_pct, int pause, int& total_global_measurements, double bw_peak, bool count_progress = true, bool replace_existing = false);
    bool run_adaptive_for_ratio(int ratio_pct, int& total_global_measurements);

public:
    GpuBenchmarkExecutor(const BenchmarkConfig& config, const system_info& sys_info, const CPUCapabilities& caps, const GPUCapabilities& gpu_caps);
    ~GpuBenchmarkExecutor() = default;

    GpuBenchmarkExecutor(const GpuBenchmarkExecutor&) = delete;
    GpuBenchmarkExecutor& operator=(const GpuBenchmarkExecutor&) = delete;

    static bool check_environment();
    bool run();
    const std::vector<BenchmarkResult>& get_results() const { return results_; }
    double get_total_runtime() const { return progress_tracker_->total_runtime(); }
    const std::vector<double>& get_iteration_times() const { return progress_tracker_->iteration_times(); }
};

#endif
