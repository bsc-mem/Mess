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

#include <iomanip>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <getopt.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include "platform/Console.h"
#include "platform/Fs.h"
#include <sstream>
#include "platform/Process.h"
#include "platform/Sys.h"

#include "SystemDetection.h"
#include "architecture/ArchitectureRegistry.h"
#include "architecture/BandwidthCounterStrategy.h"
#include "BenchmarkConfig.h"
#include "Codegen.h"
#include "Measurement.h"
#include "BenchmarkExecutor.h"
#include "ResultsProcessor.h"
#include "CliParser.h"
#include "gpu/GpuBenchmarkExecutor.h"

#include "Utils.h"
#include "PtrchasePerfHelper.h"
#include "measurement/InstructionSamplerFactory.h"
#if defined(_WIN32)
#include "measurement/bw_measurers/WindowsImcBandwidthMeasurer.h"
#include "measurement/bw_measurers/AMDuProfBandwidthMeasurer.h"
#include "measurement/bw_measurers/VtuneBandwidthMeasurer.h"
#endif
#include "utils/TerminalCursor.h"
#include "utils/StringUtils.h"

#define MESS_VERSION "2.1.0"

namespace {

void cleanup_empty_output_dirs(const BenchmarkConfig& config) {
    namespace fs = std::filesystem;
    
    fs::path root = config.output_root.empty() ? fs::path("measuring") : fs::path(config.output_root);

    bool bw_removed = false;
    bool lat_removed = false;
    std::error_code ec;
    
    fs::path bw_dir = root / "bw";
    if (fs::exists(bw_dir, ec) && fs::is_directory(bw_dir, ec) && fs::is_empty(bw_dir, ec)) {
        fs::remove(bw_dir, ec);
        if (!ec) {
            bw_removed = true;
        }
    }
    
    fs::path lat_dir = root / "lat";
    if (fs::exists(lat_dir, ec) && fs::is_directory(lat_dir, ec) && fs::is_empty(lat_dir, ec)) {
        fs::remove(lat_dir, ec);
        if (!ec) {
            lat_removed = true;
        }
    }
    
    if (bw_removed && lat_removed) {
        fs::path plotter = root / "plotter.txt";
        if (fs::exists(plotter, ec) && fs::is_regular_file(plotter, ec)) {
            fs::remove(plotter, ec);
        }
    }
}

struct PerfAccessProbeResult {
    int paranoid_level = 3;
    bool perf_accessible = false;
    std::string diagnosis;
};

PerfAccessProbeResult probe_perf_access(MeasurerType requested, int verbosity) {
    PerfAccessProbeResult result;
#if defined(_WIN32)
    result.paranoid_level = 0;
    switch (requested) {
        case MeasurerType::VTUNE:
            result.perf_accessible = VtuneBandwidthMeasurer::probe_available(verbosity);
            if (!result.perf_accessible) {
                result.diagnosis = VtuneBandwidthMeasurer::setup_guidance();
                if (result.diagnosis.empty()) {
                    result.diagnosis = "VTune is not installed, or vtune.exe is not on PATH.";
                }
            }
            break;
        case MeasurerType::UPROF:
            result.perf_accessible = AMDuProfBandwidthMeasurer::counters_available(result.diagnosis);
            break;
        case MeasurerType::IMC:
            result.perf_accessible = WindowsImcBandwidthMeasurer::counters_available(result.diagnosis);
            break;
        default: {
            const auto& choice = windows_auto_measurer(verbosity);
            result.perf_accessible = choice.available;
            result.diagnosis = choice.diagnosis;
            break;
        }
    }
#else
    (void)requested;
    (void)verbosity;
    std::ifstream paranoid_file("/proc/sys/kernel/perf_event_paranoid");
    if (paranoid_file.is_open()) {
        paranoid_file >> result.paranoid_level;
    }

    if (result.paranoid_level <= 1) {
        result.perf_accessible = run_command_success("perf stat -e cycles:k true 2>/dev/null >/dev/null");
    }
#endif
    return result;
}

struct KernelBinaryCheckResult {
    bool ok = true;
    std::string error_message;
};

KernelBinaryCheckResult validate_runtime_binaries() {
    KernelBinaryCheckResult result;
    const std::filesystem::path exe_path = get_executable_path();
    if (exe_path.empty()) {
        result.ok = false;
        result.error_message = "ERROR: Could not determine executable path.";
        return result;
    }

    const std::filesystem::path bin_dir = exe_path.parent_path();
    const std::filesystem::path ptr_chase_path = bin_dir / platform::exe_name("ptr_chase");
    const std::filesystem::path traffic_gen_path = bin_dir / platform::exe_name("traffic_gen_multiseq.x");

    if (!std::filesystem::exists(ptr_chase_path)) {
        std::ostringstream oss;
        oss << "ERROR: Kernel binaries not found!" << std::endl;
        oss << "       Expected ptr_chase at: " << ptr_chase_path << std::endl;
        oss << "       Please run generate_code to compile the kernels.";
        result.ok = false;
        result.error_message = oss.str();
        return result;
    }

    if (!std::filesystem::exists(traffic_gen_path)) {
        std::ostringstream oss;
        oss << "ERROR: TrafficGen kernel not found!" << std::endl;
        oss << "       Expected traffic_gen_multiseq.x at: " << traffic_gen_path << std::endl;
        oss << "       Please run generate_code to compile the kernels.";
        result.ok = false;
        result.error_message = oss.str();
    }

    return result;
}

[[maybe_unused]] std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

[[maybe_unused]] std::string run_command_capture(const std::string& command, int& exit_code) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return output;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    const int status = pclose(pipe);
    exit_code = platform::exit_code_of(status);
    return output;
}

uint64_t read_generated_trafficgen_array_elements(const std::string& binary_name) {
    const std::filesystem::path exe_path = get_executable_path();
    if (exe_path.empty()) {
        return 0;
    }

    const std::filesystem::path trafficgen_path = exe_path.parent_path() / binary_name;
    int exit_code = -1;
    std::string output;
#if defined(_WIN32)
    const platform::RunResult probe = platform::run(trafficgen_path.string(), {"--size"});
    output    = probe.output;
    exit_code = probe.exit_status;
#else
    output = run_command_capture(shell_quote(trafficgen_path.string()) + " --size 2>/dev/null", exit_code);
#endif
    if (exit_code != 0) {
        return 0;
    }

    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line); ) {
        const std::string key = "traffic_gen_array_elements=";
        if (line.rfind(key, 0) != 0) {
            continue;
        }
        try {
            return std::stoull(line.substr(key.size()));
        } catch (...) {
            return 0;
        }
    }

    return 0;
}

uint64_t read_generated_trafficgen_array_elements() {
    return read_generated_trafficgen_array_elements(platform::exe_name("traffic_gen_multiseq.x"));
}

#ifdef __linux__
bool is_numeric_pid_dirname(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

bool pid_owned_by_current_user(const std::filesystem::path& proc_dir) {
    std::ifstream status_file(proc_dir / "status");
    if (!status_file.is_open()) return false;

    const uid_t uid = getuid();
    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("Uid:", 0) != 0) continue;
        std::istringstream iss(line.substr(4));
        uid_t real_uid = static_cast<uid_t>(-1);
        iss >> real_uid;
        return real_uid == uid;
    }
    return false;
}

bool process_comm_matches(const std::filesystem::path& proc_dir,
                          const std::vector<std::string>& names) {
    std::ifstream comm_file(proc_dir / "comm");
    if (!comm_file.is_open()) return false;

    std::string comm;
    if (!std::getline(comm_file, comm)) return false;
    for (const auto& name : names) {
        if (comm == name) return true;
    }
    return false;
}
#endif

void signal_processes_by_name(const std::vector<std::string>& names, platform::Stop stop) {
#ifdef __linux__
    const int sig = (stop == platform::Stop::Force) ? SIGKILL : SIGTERM;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        const std::string pid_name = entry.path().filename().string();
        if (!is_numeric_pid_dirname(pid_name)) continue;
        if (!pid_owned_by_current_user(entry.path())) continue;
        if (!process_comm_matches(entry.path(), names)) continue;

        int pid_value = -1;
        if (string_utils::parse_int_strict(pid_name, pid_value) && pid_value > 0) {
            ::kill(static_cast<pid_t>(pid_value), sig);
        }
    }
#else
    (void)names;
    (void)stop;
#endif
}

void unlink_globbed_files(const char* pattern) {
    for (const std::string& path : platform::list_matching(pattern)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

void cleanup_benchmark_processes_and_temp_files() {
    const std::vector<std::string> traffic_names = {
        "traffic_gen_multiseq.x",
        "traffic_gen_rand.x"
    };
    signal_processes_by_name(traffic_names, platform::Stop::Graceful);
    signal_processes_by_name({"ptr_chase", "perf"}, platform::Stop::Graceful);
    signal_processes_by_name(traffic_names, platform::Stop::Force);
    signal_processes_by_name({"ptr_chase"}, platform::Stop::Force);

    const char* patterns[] = {
        "/tmp/traffic_gen_pid_*",
        "/tmp/ptr_chase_perf_*",
        "/tmp/ptr_chase_ready_*.flag",
        "/tmp/ptr_chase_start_*.flag",
        "/tmp/mess_ptrchase_pipe_*",
        "/tmp/mess_tgen_ready_*",
        "/tmp/ptr_chase_*.log",
        "/dev/shm/mem_sampler_perf_*.data",
        "/dev/shm/mem_sampler_perf_*.log"
    };
    for (const char* pattern : patterns) {
        unlink_globbed_files(pattern);
    }
}

bool parse_first_traffic_gen_core(const BenchmarkConfig& config, int& core) {
    if (config.traffic_gen_explicit_cores.empty()) {
        return false;
    }

    const std::string& token = config.traffic_gen_explicit_cores.front();
    if (string_utils::parse_int_strict(token, core)) {
        return true;
    }

    std::cerr << "Warning: could not parse traffic-gen core '" << token
              << "'; using default core selection." << std::endl;
    return false;
}

int resolve_traffic_gen_cores(const BenchmarkConfig& config, const system_info& info) {
    int cores = config.traffic_gen_cores;
    if (cores <= 0) {
        cores = get_default_traffic_gen_cores(info);
    }
    return std::max(cores, 1);
}

}


BenchmarkExecutor* g_executor = nullptr;
volatile sig_atomic_t g_signal_notified = 0;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        terminal::show_cursor_signal_safe();
        if (!g_signal_notified) {
            const char msg[] = "\n\nEnding execution please wait...\n";
            write(STDERR_FILENO, msg, sizeof(msg) - 1);
            g_signal_notified = 1;
        } else {
            const char msg[] = "\nCTRL+C is already being handled, please wait...\n";
            write(STDERR_FILENO, msg, sizeof(msg) - 1);
            return;
        }
        
        if (g_executor) {
            g_executor->force_cleanup();
        }

        cleanup_benchmark_processes_and_temp_files();
        
        std::signal(signal, SIG_DFL);
        raise(signal);
    }
}

void print_enhanced_header(const SystemDetector& detector) {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        ░███     ░███                                         ║\n";  
    std::cout << "║        ░████   ░████                                         ║\n";
    std::cout << "║        ░██░██ ░██░██  ░███████   ░███████   ░███████         ║\n";
    std::cout << "║        ░██ ░████ ░██ ░██    ░██ ░██        ░██               ║\n";
    std::cout << "║        ░██  ░██  ░██ ░█████████  ░███████   ░███████         ║\n";
    std::cout << "║        ░██       ░██ ░██               ░██        ░██        ║\n";
    std::cout << "║        ░██       ░██  ░███████   ░███████   ░███████         ║\n";
    std::cout << "║                                                              ║\n";
    std::cout << "║                   \033[1mMemory Stress Benchmark\033[0m                    ║\n";
    std::cout << "║                                                              ║\n";
    std::cout << "║  ┌─ System Configuration ─────────────────────────────────┐  ║\n";
    std::cout << "║  ├────────────────────────────────────────────────────────┤  ║\n";
    
    std::stringstream ss;
    detector.print(ss, 1);
    
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty()) {
            if (line.find("System Information:") == std::string::npos) {
                std::cout << "║  │ " << std::left << std::setw(55) << line << "│  ║\n";
            }
        }
    }
    
    std::cout << "║  └────────────────────────────────────────────────────────┘  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
}

int main(int argc, char **argv) {
    platform::init_console();

    platform::install_interrupt_handler(signal_handler);

    try {
        TLBMeasurement tlb_measurement;
        bool tlb_ok = false;
        
        auto config = std::make_unique<BenchmarkConfig>();
        CLIParser parser;
        if (!parser.parse(argc, argv, *config)) {
            parser.print_help(argv[0]);
            return EXIT_FAILURE;
        }

        if (!config->profile_output) {
            cleanup_empty_output_dirs(*config);
        }

        if (config->show_version) {
            std::cout << "Mess Benchmark " << MESS_VERSION << std::endl;
            return EXIT_SUCCESS;
        }

        if (config->show_help) {
            parser.print_help(argv[0]);
            return EXIT_SUCCESS;
        }

        SystemDetector detector;
        if (config->run_gpu) {
            if (config->instruction_sampling) {
                std::cerr << "--inst-lat uses CPU PEBS/SPE sampling and is not supported with --gpu\n";
                return EXIT_FAILURE;
            }
            if (!detector.detect()) {
                std::cerr << "Failed to detect system information\n";
                return EXIT_FAILURE;
            }
            print_enhanced_header(detector);
            if (!detector.get_gpu_capabilities().has_gpu) {
                std::cerr << "No NVIDIA GPU detected\n";
                return EXIT_FAILURE;
            }
            if (config->dry_run) return GpuBenchmarkExecutor::check_environment() ? EXIT_SUCCESS : EXIT_FAILURE;
            GpuBenchmarkExecutor executor(*config, detector.get_system_info(),
                detector.get_capabilities(), detector.get_gpu_capabilities());
            if (!executor.run()) return EXIT_FAILURE;
            ResultsProcessor processor(*config, nullptr);
            return processor.process(executor.get_results(), executor.get_total_runtime(),
                                     executor.get_iteration_times()) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        auto detect_future = std::async(std::launch::async, [&detector]() {
            return detector.detect();
        });
        auto cache_future = std::async(std::launch::async, []() {
            SystemToolsCache::instance().init();
        });
        const MeasurerType requested_measurer = string_to_measurer_type(config->measurer);
        const int probe_verbosity = config->verbosity;
        auto perf_probe_future = std::async(std::launch::async, [requested_measurer, probe_verbosity]() {
            return probe_perf_access(requested_measurer, probe_verbosity);
        });

        std::future<KernelBinaryCheckResult> kernel_check_future;
        if (!config->dry_run) {
            kernel_check_future = std::async(std::launch::async, []() {
                return validate_runtime_binaries();
            });
        }

        if (!detect_future.get()) {
            std::cerr << "Failed to detect system information" << std::endl;
            return EXIT_FAILURE;
        }

        print_enhanced_header(detector);

        if (detector.get_capabilities().arch == CPUArchitecture::RISCV64) {
            std::cerr << "\n\033[1;33mWarning: RISC-V support is work-in-progress.\033[0m" << std::endl;
            std::cerr << "Assembly generation, latency measurement and counter detection are available," << std::endl;
            std::cerr << "but bandwidth measurement is not yet implemented for RISC-V platforms." << std::endl;
            std::cerr << "RISC-V does not expose uncore memory controller counters (CAS) via perf;" << std::endl;
            std::cerr << "bandwidth approximation via alternative counters is planned for a future release.\n" << std::endl;
            if (!config->dry_run) {
                std::cerr << "\033[1;31mError: Cannot run the full benchmark on RISC-V yet.\033[0m" << std::endl;
                std::cerr << "Use --dry-run to inspect system detection and counter discovery.\n" << std::endl;
                return EXIT_FAILURE;
            }
        }

        if (kernel_check_future.valid()) {
            KernelBinaryCheckResult kernel_check = kernel_check_future.get();
            if (!kernel_check.ok) {
                std::cerr << kernel_check.error_message << std::endl;
                return EXIT_FAILURE;
            }
        }

        config->traffic_gen_array_elements = read_generated_trafficgen_array_elements();

        const auto perf_probe = perf_probe_future.get();
        const int paranoid_level = perf_probe.paranoid_level;
        const bool perf_accessible = perf_probe.perf_accessible;
        const auto& caps = detector.get_capabilities();

        std::future<TLBMeasurement> tlb_future;
        const std::string status_prefix = "Initial status: ";
        std::cout << status_prefix << "\033[33mChecking...\033[0m" << std::string(10, ' ') << "\r" << std::flush;
        if (perf_accessible && caps.arch != CPUArchitecture::RISCV64) {
            std::cout << status_prefix << "\033[33mMeasuring TLB...\033[0m" << std::string(5, ' ') << "\r" << std::flush;
            tlb_future = std::async(std::launch::async, []() {
                return measure_and_set_tlb_latency();
            });
        } else {
            std::cout << status_prefix << "\033[33mSkipping TLB measurement (performance counters not accessible)\033[0m" << std::string(5, ' ') << "\r" << std::flush;
            tlb_ok = false;
            tlb_measurement.latency_ns = 0.0;
        }

        int src_cpu = 0;
        const bool has_explicit_src_cpu = parse_first_traffic_gen_core(*config, src_cpu);

        std::vector<int> target_nodes = config->memory_bind_nodes;
        if (target_nodes.empty()) {
            target_nodes.push_back(get_numa_node_of_cpu(src_cpu));
        }

        auto& bw_strategy = BandwidthCounterStrategy::instance();
        bw_strategy.set_measurer_type(string_to_measurer_type(config->measurer));
#if defined(_WIN32)
        bw_strategy.set_windows_auto_measurer(windows_auto_measurer().type);
#endif
        bw_strategy.set_extra_counters(config->add_counters);
        bw_strategy.set_memory_type(caps.memory_type);
        std::cout << status_prefix << "\033[33mDiscovering counters...\033[0m" << std::string(5, ' ') << "\r" << std::flush;
        auto counter_discovery_future = std::async(std::launch::async, [&bw_strategy, src_cpu, &target_nodes, &caps]() {
            bw_strategy.initialize(src_cpu, target_nodes, caps);
        });


        if (perf_accessible && tlb_future.valid()) {
            tlb_measurement = tlb_future.get();
            tlb_ok = (tlb_measurement.latency_ns > 0);
        }

        std::cout << status_prefix << "\033[33mWaiting for counter discovery...\033[0m" << std::string(5, ' ') << "\r" << std::flush;
        counter_discovery_future.get();
        cache_future.get();

        if (has_explicit_src_cpu) {
            auto& cache = SystemToolsCache::instance();
            cache.ptr_chase_cpu = src_cpu;
            auto it = cache.cpu_topology.find(src_cpu);
            if (it != cache.cpu_topology.end()) {
                cache.ptr_chase_node = it->second.node;
            } else {
                cache.ptr_chase_node = get_numa_node_of_cpu(src_cpu);
            }
        }

        const std::string sampler_diagnostic = config->instruction_sampling
            ? InstructionSamplerFactory::availability_diagnostic(caps.arch)
            : std::string();
        const bool sampler_ok = sampler_diagnostic.empty();

        const bool apple_silicon = caps.isAppleSilicon();
        bool overall_ok = config->run_gpu || ((perf_accessible || apple_silicon) && (tlb_ok || apple_silicon) && sampler_ok);

        // Initialize default ratios if none specified
        if (config->ratios_pct.empty()) {
            config->ratios_pct.clear();
            for (int ratio = 100; ratio >= 0; ratio -= 2) {
                config->ratios_pct.push_back(static_cast<double>(ratio));
            }
        }

        std::cout << "\r" << std::string(80, ' ') << "\r";
        std::cout << status_prefix;

        if (overall_ok) {
            std::cout << "\033[32mOK\033[0m" << std::endl;
            parser.display_configuration(*config, perf_accessible, paranoid_level, tlb_ok, tlb_measurement.latency_ns);
        } else {
            std::cout << "\033[31mKO\033[0m" << std::endl;

            if (!perf_accessible) {
#if defined(_WIN32)
                std::cout << "  → Bandwidth counters are not available." << std::endl;
                if (!perf_probe.diagnosis.empty()) {
                    std::cout << "    " << perf_probe.diagnosis << std::endl;
                }
                std::cout << "    See the Windows section of README.md." << std::endl;
#else
                std::cout << "  → Performance counter access denied (paranoid level: " << paranoid_level << ")" << std::endl;
                std::cout << "    Required for benchmark execution. Try: echo 0 | sudo tee /proc/sys/kernel/perf_event_paranoid" << std::endl;
#endif
            }
            if (perf_accessible && !tlb_ok) {
                std::cout << "  → TLB measurement not available (using default values)" << std::endl;
            }
            if (!sampler_ok) {
                std::cout << "  → " << sampler_diagnostic << std::endl;
            }

            if (!config->dry_run) {
                std::cerr << "\n\033[1;31mError: System configuration check failed\033[0m" << std::endl;
                std::cerr << "Benchmark cannot run on this system. Please check the configuration and try again.\n" << std::endl;
                return EXIT_FAILURE;
            }
        }

        uint64_t tlb1_raw = 0, tlb2_raw = 0;
        bool use_tlb1 = false, use_tlb2 = false;
        bw_strategy.get_tlb_counters(tlb1_raw, tlb2_raw, use_tlb1, use_tlb2);

#if defined(_WIN32)
        const bool huge_pages_ok = platform::large_pages_available();
        if (!huge_pages_ok) {
            std::cerr << "\nWarning: huge pages are unavailable; pointer chase will use 4 KB pages.\n"
                      << "Grant \"Lock pages in memory\" to this user, then sign in again.\n"
                      << std::endl;
        }
#endif

        if (config->dry_run) {
            std::cout << "\n=== Dry Run Mode ===" << std::endl;
            config->print_summary(std::cout, true);

            auto architecture = ArchitectureRegistry::instance().getArchitecture(caps);

            std::cout << "\n=== Memory Architecture & Counter Discovery ===" << std::endl;
            std::cout << "Detected Architecture: " << (architecture ? architecture->getName() : "Unknown") << std::endl;
            std::cout << "Memory Configuration:\n";
            std::cout << "  Type: " << (!caps.memory_type.empty() ? caps.memory_type : "Unknown");
            if (!caps.memory_frequency.empty()) {
                std::cout << " @ " << caps.memory_frequency;
            }
            if(caps.cpu_inferred) std::cout << " (inferred from CPU model; run with sudo for exact detection)";
            std::cout << "\n";
            std::cout << "  Channels: " << caps.memory_channels;
            if (caps.bus_width > 0) {
                std::cout << " x " << caps.bus_width << "-bit";
            }
            std::cout << "\n";
            std::cout << "  Total Size: " << (caps.total_memory / (1024*1024*1024)) << " GB\n";
            std::cout << "  HBM detected: " << (bw_strategy.is_hbm() ? "yes" : "no") << "\n";
#if defined(_WIN32)
            std::cout << "  Huge pages: " << (huge_pages_ok ? "enabled" : "disabled (pointer chase uses 4 KB pages)") << "\n";
#endif
            std::cout << "\n";

            const int num_cores = resolve_traffic_gen_cores(*config, detector.get_system_info());

            std::cout << "TrafficGen Configuration:\n";
            std::cout << "  Cores: " << num_cores << "\n";
            std::cout << "  Memory Binding: " << config->get_bind_name() << "\n\n";

            std::cout << "=== Counter Discovery Results ===" << std::endl;
            std::cout << "Measurer: " << bw_strategy.get_measurer_name() << std::endl;
            std::cout << "Latency Counters: cycles,instructions" << std::endl;
            bw_strategy.print_counter_info(std::cout);

            if (tlb_ok) {
                std::cout << "TLB Hit Latency: " << tlb_measurement.latency_ns << " ns" << std::endl;
                std::cout << "Cache Line Size: " << tlb_measurement.cache_line_size << " bytes" << std::endl;
            }


            std::cout << "TLB Page Walk Counters:" << std::endl;
            if (use_tlb1) {
                std::cout << "  Page Walk: 0x" << std::hex << tlb1_raw << std::dec << std::endl;
            }
            if (use_tlb2) {
                std::cout << "  STLB Hit / L1D Refill: 0x" << std::hex << tlb2_raw << std::dec << std::endl;
            }
            if (!use_tlb1 && !use_tlb2) {
                std::cout << "  None defined for this architecture" << std::endl;
            }

            std::cout << "\nDry run complete - the benchmark was not executed." << std::endl;
            return EXIT_SUCCESS;
        }

        if (config->verbosity >= 3) {
            std::cout << "\n=== Memory Architecture & Counter Discovery ===" << std::endl;
            std::cout << "Memory Configuration:\n";
            std::cout << "  Type: " << detector.get_capabilities().memory_type;
            if (!detector.get_capabilities().memory_frequency.empty()) {
                std::cout << " @ " << detector.get_capabilities().memory_frequency;
            }
            if(detector.get_capabilities().cpu_inferred) std::cout << " (inferred from CPU model; run with sudo for exact detection)";
            std::cout << "\n";
            
            const int traffic_cores = resolve_traffic_gen_cores(*config, detector.get_system_info());

            std::cout << "TrafficGen Configuration:\n";
            std::cout << "  Cores: " << traffic_cores << "\n";
            if (config->traffic_gen_array_elements > 0) {
                std::cout << "  MultiSeq array size: " << ((config->traffic_gen_array_elements * sizeof(double)) / (1000 * 1000))
                          << " MB total\n";
            }
            std::cout << std::endl;

            std::cout << "Bindings:" << std::endl;
            std::cout << "Detected Architecture: " << ArchitectureRegistry::instance().getArchitecture(detector.get_capabilities())->getName() << std::endl;
            std::cout << "  Memory Binding: " << config->get_bind_name() << std::endl;
            std::cout << std::endl;

        }

        KernelGenerator kernel_gen(config->kernel, detector.get_capabilities());
        
        std::filesystem::path root = get_project_root();
        std::string output_dir = (root / "src/traffic_gen/src").string();

        kernel_gen.generate_kernel(output_dir);


        const auto& capabilities = detector.get_capabilities();
        
        std::vector<BenchmarkResult> final_results;
        double total_runtime = 0.0;
        std::vector<double> iteration_times;

        {
            BenchmarkExecutor executor(*config, detector.get_system_info(), capabilities,
                                     tlb_measurement.latency_ns, tlb_measurement.cache_line_size);
            g_executor = &executor;

            terminal::CursorGuard cursor_guard;

            if (!executor.run()) {
                g_executor = nullptr;
                std::cerr << "\n\nBenchmark execution failed" << std::endl;
                return EXIT_FAILURE;
            }
            g_executor = nullptr;

            final_results = executor.get_results();
            total_runtime = executor.get_total_runtime();
            iteration_times = executor.get_iteration_times();
        }

        ResultsProcessor results_processor(*config, nullptr);
        if (!results_processor.process(final_results, total_runtime, iteration_times)) {
            std::cerr << "Results processing failed" << std::endl;
            return EXIT_FAILURE;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return EXIT_FAILURE;
    }

    

    return EXIT_SUCCESS;
}
