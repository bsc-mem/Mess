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

#include "gpu/GpuBenchmarkExecutor.h"
#include "Utils.h"
#include "gpu/GpuKernelGenerator.h"
#include "gpu/GpuMeasurement.h"
#include "measurement/PointSampling.h"
#include "measurement/ResultFormatter.h"
#include "platform/Process.h"
#include "platform/Fs.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

static const uint64_t PTR_CHASE_KERNEL_NS = 50000000;
static const uint64_t PTR_CHASE_SETTLE_NS = 2000000;
static const uint64_t PTR_CHASE_MIN_KERNELS = 4;
static const uint64_t PTR_CHASE_MAX_NS = 1500000000;
static const double PTR_CHASE_PRECISION = 0.005;
static const uint64_t WORKLOAD_GENERATOR_ITER_SIZE = 400;
static const int PTX_UNROLL_OPS = 400;

namespace {
const char* worker_mode(ExecutionMode mode) {
    (void)mode;
    return "multiseq";
}

const char* mode_directory(ExecutionMode mode) {
    (void)mode;
    return "multisequential";
}

const char* mode_label(ExecutionMode mode) {
    (void)mode;
    return "MULTISEQ";
}
}

bool GpuBenchmarkExecutor::check_environment() {
    const auto nvcc = platform::which("nvcc");
    const bool compiler = nvcc.has_value();
    const bool make = platform::which("make").has_value();
    if (compiler && make) {
        std::error_code error;
        std::filesystem::path cuda = std::getenv("CUDA_HOME") ? std::filesystem::path(std::getenv("CUDA_HOME"))
                                                               : std::filesystem::canonical(*nvcc, error).parent_path().parent_path();
        const std::filesystem::path cupti = std::getenv("CUPTI_HOME") ? std::filesystem::path(std::getenv("CUPTI_HOME")) : cuda / "extras/CUPTI";
        if (std::filesystem::exists(cupti / "include/cupti_range_profiler.h")) return true;
        std::cerr << "GPU setup is incomplete:\n  CUDA Toolkit 12.6 or newer with CUPTI is required; cupti_range_profiler.h was not found in "
                  << (cupti / "include").string() << ".\n  Load a newer CUDA module, or set CUPTI_HOME if CUPTI is installed elsewhere.\n";
        return false;
    }
    std::cerr << "GPU setup is incomplete:\n";
    if (!compiler) std::cerr << "  nvcc (CUDA compiler) was not found in PATH. GPU kernels and their CUPTI counters are compiled at runtime.\n";
    if (!make) std::cerr << "  make was not found in PATH. Install or load the build tools.\n";
    if (!compiler) {
        std::string module_command;
        if (const char* lmod = std::getenv("LMOD_CMD")) module_command = lmod;
        else if (auto command = platform::which("modulecmd")) module_command = command->string();
        std::string cuda_module;
        if (!module_command.empty()) {
            platform::RunOptions options;
            options.timeout = std::chrono::seconds(3);
            options.max_output_bytes = 32768;
            for (const auto& args : {std::vector<std::string>{"sh", "-t", "--default", "avail", "cuda"},
                                     std::vector<std::string>{"sh", "-t", "avail", "cuda"}}) {
                const auto available = platform::run(module_command, args, options);
                if (available.spawn_failed || available.timed_out || available.exit_status != 0) continue;
                std::istringstream lines(available.output);
                std::string line;
                const std::regex name(R"(^\s*((?:cuda|CUDA)/[0-9][A-Za-z0-9._-]*)(?:\s|\(|$))");
                std::smatch match;
                while (std::getline(lines, line)) {
                    if (std::regex_search(line, match, name)) { cuda_module = match[1]; break; }
                }
                if (!cuda_module.empty()) break;
            }
        }
        if (!cuda_module.empty())
            std::cerr << "An available CUDA module is: " << cuda_module
                      << "\n  module load " << cuda_module << "\nThen retry the command; check that nvcc is in PATH.\n";
        else if (!module_command.empty() || std::getenv("MODULESHOME"))
            std::cerr << "Use 'module avail cuda' to find and load the site's CUDA toolkit (12.6 or newer).\n";
        else
            std::cerr << "Install or activate the NVIDIA CUDA Toolkit 12.6 or newer and add its bin directory to PATH.\n";
    }
    return false;
}

GpuBenchmarkExecutor::GpuBenchmarkExecutor(const BenchmarkConfig& config, const system_info&, const CPUCapabilities& caps, const GPUCapabilities& gpu_caps)
    : config_(config), caps_(caps), gpu_caps_(gpu_caps),
      progress_tracker_(std::make_unique<ProgressTracker>(0)) {
    modes_.push_back(ExecutionMode::MULTISEQUENTIAL);
    for (ExecutionMode mode : modes_)
        storage_[mode] = std::make_unique<MeasurementStorage>(config, 128, config.verbosity, config.output_root + "/gpu/" + mode_directory(mode));
}

std::string GpuBenchmarkExecutor::compute_architecture() const {
    std::vector<int> capabilities;
    for (const auto& gpu : gpu_caps_.gpus) {
        std::string digits;
        for (char c : gpu.compute_capability) {
            if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
            else if (c != '.') { digits.clear(); break; }
        }
        if (digits.size() >= 2 && std::find(capabilities.begin(), capabilities.end(), std::stoi(digits)) == capabilities.end())
            capabilities.push_back(std::stoi(digits));
    }
    if (capabilities.empty()) return "-arch=native";

    auto supported = [](const std::string& option, const std::string& prefix) {
        platform::RunOptions options;
        options.timeout = std::chrono::seconds(30);
        std::vector<int> versions;
        std::istringstream listed(platform::run("nvcc", {option}, options).output);
        for (std::string entry; listed >> entry;) {
            const std::string version = entry.substr(std::min(prefix.size(), entry.size()));
            if (entry.rfind(prefix, 0) == 0 && !version.empty() && version.find_first_not_of("0123456789") == std::string::npos)
                versions.push_back(std::stoi(version));
        }
        return versions;
    };
    const std::vector<int> native = supported("--list-gpu-code", "sm_");
    const std::vector<int> portable = supported("--list-gpu-arch", "compute_");

    std::string gencode;
    for (int capability : capabilities) {
        const std::string exact = std::to_string(capability);
        if (std::find(native.begin(), native.end(), capability) != native.end()) {
            gencode += " -gencode arch=compute_" + exact + ",code=sm_" + exact;
            continue;
        }
        int newest = 0;
        for (int version : portable) if (version <= capability) newest = std::max(newest, version);
        if (!newest) return "-arch=native";
        const std::string ptx = std::to_string(newest);
        gencode += " -gencode arch=compute_" + ptx + ",code=compute_" + ptx;
    }
    return gencode.substr(1);
}

bool GpuBenchmarkExecutor::build_worker(const std::vector<int>& ratios, const std::string& directory) const {
    std::error_code copy_error;
    std::filesystem::copy(get_project_root() / "templates/gpu", directory,
                          std::filesystem::copy_options::recursive, copy_error);
    if (copy_error) {
        std::cerr << "Failed to copy GPU templates: " << copy_error.message() << '\n';
        return false;
    }
    auto write = [&](const std::string& name, const std::string& content) {
        std::ofstream file(directory + "/" + name);
        file << content;
        file.close();
        if (!file) std::cerr << "Failed to write GPU source " << name << ".\n";
        return static_cast<bool>(file);
    };
    if (!write("loopGPU.h", GpuKernelGenerator::generate_pointer_chase()) ||
        !write("workloads.cuh", GpuKernelGenerator::generate_workload_table(PTX_UNROLL_OPS, ratios, modes_)))
        return false;
    for (ExecutionMode mode : modes_) {
        for (int ratio : ratios) {
            if (mode == ExecutionMode::MULTISEQUENTIAL &&
                !write(GpuKernelGenerator::workload_file(ratio), GpuKernelGenerator::generate_workload(PTX_UNROLL_OPS, ratio)))
                return false;
        }
    }

    const unsigned jobs = std::max(1u, std::min<unsigned>(ratios.size() + 1, std::thread::hardware_concurrency()));
    platform::RunOptions options;
    options.working_dir = directory;
    options.timeout = std::chrono::minutes(10);
    options.max_output_bytes = 1 << 20;
    const auto build = platform::run("make", {"-j" + std::to_string(jobs), "NVCC_GENCODE=" + compute_architecture()}, options);
    if (build.spawn_failed || build.timed_out || build.exit_status != 0) {
        std::cerr << "Failed to compile GPU template code: " << build.error << '\n' << build.output;
        return false;
    }
    return true;
}

bool GpuBenchmarkExecutor::measure_sample(ExecutionMode mode, int ratio_pct, int pause, BenchmarkResult& result) {
    std::string line, error;
    if (!worker_->sample(worker_mode(mode), ratio_pct, pause, line, error) || !gpu::parse_sample(line, metrics_, result, error)) {
        std::cerr << "\nGPU measurement failed: " << error << '\n';
        return false;
    }
    if (result.ratio_pct != ratio_pct || result.pause != pause) {
        std::cerr << "\nGPU worker answered a different point: " << line << '\n';
        return false;
    }
    result.mode = mode;
    return true;
}

measurement::MeasuredPoint GpuBenchmarkExecutor::adaptive_probe(ExecutionMode mode, int ratio_pct, int pause, int& total_global_measurements, double bw_peak, bool count_progress, bool replace_existing) {
    MeasurementStorage& storage = *storage_.at(mode);
    measurement::MeasuredPoint measured;
    std::vector<BenchmarkResult> samples;
    OutlierDetector outlier_detector(config_);
    auto measure = [&](int rep, BenchmarkResult& result) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            progress_tracker_->start_iteration();
            const bool success = measure_sample(mode, ratio_pct, pause, result);
            const double elapsed = progress_tracker_->finish_iteration();
            if (success) {
                result.repetition = rep;
                result.iteration_time_s = elapsed;
                return true;
            }
        }
        return false;
    };
    auto find_outliers = [&](const std::vector<BenchmarkResult>& values) {
        double max_bw = 0.0;
        for (const auto& value : values) max_bw = std::max(max_bw, value.bandwidth_mbps / 1000.0);
        if (values.size() < 2 || (bw_peak > 1e-12 && max_bw < 0.02 * bw_peak))
            return std::vector<int>{};
        return outlier_detector.find_outliers(values, 10.0);
    };
    measurement::PointSamplingPolicy policy;
    policy.repetitions = config_.point_reps;
    policy.max_point_attempts = 2;
    if (!measurement::resolve_point(policy, measure, find_outliers, [] {}, samples)) return measured;

    if (replace_existing) {
        results_.erase(std::remove_if(results_.begin(), results_.end(), [&](const BenchmarkResult& value) {
            return value.mode == mode && value.ratio_pct == ratio_pct && value.pause == pause;
        }), results_.end());
    }
    storage.cleanup_measurement_files(ratio_pct, pause);
    BandwidthCounterSelection counters;
    counters.type = CounterType::TOTAL_BYTES;
    counters.cas.read_events = {metrics_[0]};
    counters.cas.write_events = {metrics_[1]};
    for (const auto& sample : samples) {
        results_.push_back(sample);
        if (count_progress) {
            progress_tracker_->record_iteration_time(sample.iteration_time_s);
            progress_tracker_->increment_completed_iterations();
        }
        storage.append_to_file_with_lock(storage.bw_file_path(ratio_pct, pause), measurement::format_bandwidth_output(sample, counters, 1));
        storage.append_to_file_with_lock(storage.lat_file_path(ratio_pct, pause), measurement::format_latency_output(sample, 0, false));
    }
    measured.stats = measurement::summarize_point(pause, samples);
    measured.results = samples;
    progress_tracker_->update_ratio_stats(ratio_pct, measured.stats.bw * 1000.0, measured.stats.lat);
    std::cout << "\r" << progress_tracker_->format_progress(
        progress_tracker_->completed_iterations(), total_global_measurements,
        std::max(0, total_global_measurements - progress_tracker_->completed_iterations())) << std::flush;
    return measured;
}

bool GpuBenchmarkExecutor::run_adaptive_for_ratio(int ratio_pct, int& total_global_measurements) {
    auto measure = [&](size_t index, int pause, double peak, bool count, bool replace) {
        auto point = adaptive_probe(modes_[index], ratio_pct, pause, total_global_measurements, peak, count, replace);
        if (point.stats.n_reps == 0) throw std::runtime_error("GPU point could not be measured reliably");
        return point;
    };
    auto account = [&](const std::vector<BenchmarkResult>& samples) {
        for (const auto& sample : samples) {
            progress_tracker_->record_iteration_time(sample.iteration_time_s);
            progress_tracker_->increment_completed_iterations();
        }
    };
    const auto tier = CurveTracer::resolve_tier(config_.adaptive_tier, config_.adaptive_point_count);
    std::vector<std::string> labels;
    for (ExecutionMode mode : modes_) labels.push_back(mode_label(mode));
    return measurement::run_adaptive_sweep(config_, labels, *progress_tracker_,
        total_global_measurements, measure, account, std::max(1, tier.max_budget / 3));
}

bool GpuBenchmarkExecutor::run() try {
  if (!check_environment()) return false;
  if (config_.point_reps <= 0) {
      std::cerr << "GPU measurements require at least one repetition.\n";
      return false;
  }
  results_.clear();

  std::vector<int> ratios;
  for (double ratio : config_.ratios_pct) ratios.push_back(static_cast<int>(std::lround(ratio)));
  if (ratios.empty())
      for (int ratio = 100; ratio >= 0; ratio -= 2) ratios.push_back(ratio);

  const bool adaptive_mode = config_.pauses.empty();
  int total_measurements = 0;
  if (!adaptive_mode) {
      total_measurements = ratios.size() * config_.pauses.size() * config_.point_reps * modes_.size();
  } else {
      const auto tier = CurveTracer::resolve_tier(config_.adaptive_tier, config_.adaptive_point_count);
      total_measurements = ratios.size() * tier.max_budget * config_.point_reps * modes_.size();
  }
  progress_tracker_->start_benchmark(total_measurements);
  if (adaptive_mode) {
      const auto tier = CurveTracer::resolve_tier(config_.adaptive_tier, config_.adaptive_point_count);
      progress_tracker_->set_adaptive_info(ratios.size() * modes_.size(), config_.point_reps, tier.max_budget, modes_.size());
  }

  struct TemporaryDirectory {
      std::filesystem::path path;
      ~TemporaryDirectory() {
          std::error_code error;
          std::filesystem::remove_all(path, error);
      }
  };
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  TemporaryDirectory temporary;
  for (int attempt = 0; attempt < 100; ++attempt) {
      auto candidate = std::filesystem::temp_directory_path() /
          ("mess_gpu_" + std::to_string(stamp) + "_" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate)) {
          temporary.path = candidate;
          break;
      }
  }
  if (temporary.path.empty()) {
      std::cerr << "Unable to create GPU temporary directory.\n";
      return false;
  }
  const std::string build_dir = temporary.path.string();
  if (!build_worker(ratios, build_dir)) return false;

  metrics_ = gpu::measurement_metrics(config_.add_counters);
  std::string metric_list;
  for (const auto& metric : metrics_) metric_list += (metric_list.empty() ? "" : ",") + metric;
  for (const auto& [mode, storage] : storage_) storage->ensure_directories_exist();
  worker_ = std::make_unique<GpuWorker>(
      (std::filesystem::path(build_dir) / "main").string(),
      std::vector<std::string>{"-w", std::to_string(WORKLOAD_GENERATOR_ITER_SIZE),
                               "-t", std::to_string(PTR_CHASE_KERNEL_NS), "-d", std::to_string(PTR_CHASE_SETTLE_NS),
                               "-k", std::to_string(PTR_CHASE_MIN_KERNELS), "-T", std::to_string(PTR_CHASE_MAX_NS),
                               "-e", std::to_string(PTR_CHASE_PRECISION),
                               "-m", metric_list},
      build_dir, storage_.at(modes_.front())->logs_dir() + "/gpu_worker.txt");
  std::string error;
  if (!worker_->start(error)) {
      std::cerr << "GPU worker failed to start: " << error << '\n';
      return false;
  }
  GPUCapabilities measured_gpu = gpu_caps_;
  if (measured_gpu.gpus.empty()) measured_gpu.gpus.emplace_back();
  if (gpu::parse_memory(worker_->ready(), measured_gpu.gpus.front())) measured_gpu.has_gpu = true;
  for (const auto& [mode, storage] : storage_) storage->write_plotter_file(caps_, "local", 0.0, 128, -1.0, -1.0, &measured_gpu);

  int current_ratio = 0;
  for (int ratio_pct : ratios) {
    current_ratio++;
    progress_tracker_->set_current_ratio(ratio_pct, current_ratio, ratios.size());
    if (config_.verbosity >= 2) {
      std::cout << "\n\n=== GPU " << ratio_pct << "% loads (" << current_ratio
                << "/" << ratios.size() << " load ratios) ===" << std::endl;
    }
    if (adaptive_mode) {
        if (!run_adaptive_for_ratio(ratio_pct, total_measurements)) return false;
    } else {
        for (int pause : config_.pauses) {
            for (ExecutionMode mode : modes_) {
                if (adaptive_probe(mode, ratio_pct, pause, total_measurements, 0.0).stats.n_reps == 0)
                    return false;
            }
        }
    }
  }
  worker_.reset();

  std::cout << "\nGPU Benchmark complete." << std::endl;
  return true;
} catch (const std::exception& error) {
  std::cerr << "GPU benchmark failed: " << error.what() << '\n';
  return false;
}
