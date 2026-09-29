/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
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

#include "measurement/bw_measurers/WindowsImcBandwidthMeasurer.h"

#include "measurement/BandwidthStabilizer.h"
#include "measurement/BandwidthSampleAggregation.h"
#include "measurement/TheoreticalPeakCalculator.h"
#include "platform/Process.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>


namespace {

// msr.sys IOCTL interface. Must match pcm/src/WinMSRDriver/msrstruct.h.

constexpr DWORD kMsrDevType = 50000;

#define MESS_MSR_CTL(code) CTL_CODE(kMsrDevType, (code), METHOD_BUFFERED, FILE_ANY_ACCESS)

constexpr DWORD kIoCtlPciCfgRead  = MESS_MSR_CTL(0x802);
constexpr DWORD kIoCtlMmapSupport = MESS_MSR_CTL(0x804);
constexpr DWORD kIoCtlMmap        = MESS_MSR_CTL(0x805);
constexpr DWORD kIoCtlMunmap      = MESS_MSR_CTL(0x806);

struct PciCfgRequest {
    ULONG   bus, dev, func, reg, bytes;
    ULONG64 write_value;
};

struct MmapRequest {
    LARGE_INTEGER address;
    SIZE_T        size;
};

// From pcm/src/bw.cpp (ClientBW + getClientIMCStartAddr).
constexpr ULONG    kMchbarPciOffset  = 0x0048;
constexpr uint32_t kMcEventBase      = 0x5000;
constexpr uint32_t kMcMmapSize       = 0x6000;
constexpr uint32_t kMcDramDataReads  = 0x5050;
constexpr uint32_t kMcDramDataWrites = 0x5054;

std::string format_last_error(DWORD e) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = msg ? msg : "";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s + " (" + std::to_string(e) + ")";
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// A hypervisor traps the MSRs other tools need but not this MMIO path, so the bit is context in
// a failure message, never a reason to stop.
bool hypervisor_present() {
    int regs[4] = {};
    __asm__ __volatile__("cpuid"
                         : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                         : "a"(1), "c"(0));
    return (regs[2] & (1 << 31)) != 0;
}

// Counters wrap at 32 bits, about 8 s at 34 GB/s. The clamp below keeps intervals shorter.
inline uint32_t delta32(uint32_t now, uint32_t prev) { return now - prev; }

inline double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace


class WindowsImcBandwidthMeasurer::Imc {
public:
    ~Imc() {
        if (mapped_ && driver_ != INVALID_HANDLE_VALUE) {
            MmapRequest req{};
            req.address.QuadPart =
                static_cast<LONGLONG>(reinterpret_cast<uintptr_t>(mapped_));
            DWORD got = 0;
            DeviceIoControl(driver_, kIoCtlMunmap, &req, sizeof(req),
                            nullptr, 0, &got, nullptr);
        }
        if (driver_ != INVALID_HANDLE_VALUE) CloseHandle(driver_);
    }

    bool open(std::string& error) {
        driver_ = CreateFileA("\\\\.\\RDMSR", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
        if (driver_ == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            error = "cannot open the msr driver device \\\\.\\RDMSR: " +
                    format_last_error(err);
            if (err == ERROR_ACCESS_DENIED && !process_is_elevated()) {
                error += "\n         The device is administrator-only and this process "
                         "is not elevated.\n"
                         "         Re-run Mess from an Administrator prompt.";
            } else if (err == ERROR_FILE_NOT_FOUND) {
                error += "\n         The msr service is not running. Start it with "
                         "'sc.exe start msr', or\n"
                         "         install it first - see "
                         "the Windows section of README.md.";
            }
            return false;
        }

        uint64_t support = 0;
        if (!query(kIoCtlMmapSupport, &support) || !support) {
            error = "the msr driver reports no physical-memory mapping support, so the "
                    "IMC counters cannot be reached.";
            return false;
        }

        // MCHBAR: bus 0, device 0, function 0, offset 0x48, 64 bits wide.
        uint64_t bar = 0;
        if (!read_pci_cfg(0, 0, 0, kMchbarPciOffset, 8, &bar)) {
            error = "PCI configuration read of MCHBAR failed: " + last_error_;
            return false;
        }
        if (bar == 0) {
            error = "MCHBAR reads as zero. This backend is the CLIENT "
                    "memory-controller path;\n"
                    "         server parts (Xeon) place the IMC counters elsewhere and "
                    "are not supported by it.";
            if (hypervisor_present()) {
                error += "\n         A hypervisor is also present (CPUID.1:ECX[31]), "
                         "which hides some platform registers.";
            }
            return false;
        }

        base_ = bar & ~(4096ULL - 1ULL);  // round down to 4K, as PCM does

        MmapRequest req{};
        req.address.QuadPart = static_cast<LONGLONG>(base_ + kMcEventBase);
        req.size             = kMcMmapSize - kMcEventBase;
        ULONG64 mapped_base = 0;
        DWORD   got = 0;
        if (!DeviceIoControl(driver_, kIoCtlMmap, &req, sizeof(req),
                             &mapped_base, sizeof(mapped_base), &got, nullptr) ||
            mapped_base == 0) {
            error = "mapping MCHBAR failed: " + format_last_error(GetLastError());
            return false;
        }
        // The driver maps into ZwCurrentProcess(), so this address is dereferenceable here.
        mapped_ = reinterpret_cast<volatile void*>(
            static_cast<uintptr_t>(mapped_base));
        return true;
    }

    uint32_t reads()  const { return reg(kMcDramDataReads); }
    uint32_t writes() const { return reg(kMcDramDataWrites); }

private:
    uint32_t reg(uint32_t abs_offset) const {
        const auto* p = reinterpret_cast<volatile const uint32_t*>(
            reinterpret_cast<volatile const uint8_t*>(mapped_) +
            (abs_offset - kMcEventBase));
        return *p;
    }

    bool query(DWORD ioctl_code, uint64_t* out) {
        ULONG64 value = 0;
        DWORD   got = 0;
        if (!DeviceIoControl(driver_, ioctl_code, nullptr, 0, &value, sizeof(value),
                             &got, nullptr)) {
            last_error_ = format_last_error(GetLastError());
            return false;
        }
        *out = value;
        return true;
    }

    bool read_pci_cfg(ULONG bus, ULONG dev, ULONG func, ULONG reg_offset,
                      ULONG bytes, uint64_t* out) {
        PciCfgRequest req{};
        req.bus = bus; req.dev = dev; req.func = func;
        req.reg = reg_offset; req.bytes = bytes;
        ULONG64 value = 0;
        DWORD   got = 0;
        if (!DeviceIoControl(driver_, kIoCtlPciCfgRead, &req, sizeof(req),
                             &value, sizeof(value), &got, nullptr)) {
            last_error_ = format_last_error(GetLastError());
            return false;
        }
        *out = value;
        return true;
    }

    HANDLE         driver_ = INVALID_HANDLE_VALUE;
    volatile void* mapped_ = nullptr;
    uint64_t       base_   = 0;
    std::string    last_error_;
};


WindowsImcBandwidthMeasurer::WindowsImcBandwidthMeasurer(
    const BenchmarkConfig& config,
    const system_info& sys_info,
    const CPUCapabilities& caps,
    MeasurementStorage* storage,
    TrafficGenProcessManager* traffic_gen_manager,
    std::function<std::vector<int>()> numa_resolver,
    ExecutionMode mode)
    : BandwidthMeasurer(config, sys_info, caps, storage, traffic_gen_manager,
                        numa_resolver, mode) {}

WindowsImcBandwidthMeasurer::~WindowsImcBandwidthMeasurer() = default;

bool WindowsImcBandwidthMeasurer::counters_available(std::string& reason) {
    Imc probe;
    reason.clear();
    return probe.open(reason);
}

bool WindowsImcBandwidthMeasurer::init() {
    imc_ = std::make_unique<Imc>();
    if (!imc_->open(error_)) {
        imc_.reset();
        return false;
    }
    return true;
}

bool WindowsImcBandwidthMeasurer::sample_bandwidth(long long& cas_rd,
                                                   long long& cas_wr,
                                                   double& elapsed,
                                                   const std::vector<int>& mem_nodes) const {
    // One controller and one node, so the memory-node list selects nothing here.
    (void)mem_nodes;
    if (!imc_) return false;

    const uint32_t rd0 = imc_->reads();
    const uint32_t wr0 = imc_->writes();
    const double   t0  = now_seconds();

    std::this_thread::sleep_for(std::chrono::microseconds(
        static_cast<long long>(std::max(1.0, sampling_interval_ms_) * 1000.0)));

    const uint32_t rd1 = imc_->reads();
    const uint32_t wr1 = imc_->writes();
    elapsed = now_seconds() - t0;

    cas_rd = static_cast<long long>(delta32(rd1, rd0));
    cas_wr = static_cast<long long>(delta32(wr1, wr0));
    return elapsed > 0.0;
}

bool WindowsImcBandwidthMeasurer::wait_for_stabilization(int& samples_taken,
                                                         long long& last_cas_rd,
                                                         long long& last_cas_wr,
                                                         double& last_elapsed,
                                                         int pause,
                                                         int ratio,
                                                         bool fast_resume,
                                                         std::function<void()> on_sample) {
    extra_perf_values_.clear();
    samples_taken = 0;

    if (!imc_) {
        std::cerr << "ERROR: IMC counters are not open." << std::endl;
        return false;
    }

    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;

    // 32 bits of 64-byte lines wrap after 274 GB, about 8 s saturated. A delta spanning a wrap
    // under-reports silently.
    constexpr double kMaxSafeIntervalMs = 2000.0;
    double interval_ms = sampling_interval_ms_;
    if (interval_ms > kMaxSafeIntervalMs) {
        if (verbose2) {
            std::cout << "    Notice: clamping the sampling interval from " << interval_ms
                      << " ms to " << kMaxSafeIntervalMs
                      << " ms - the IMC counters are 32-bit and would wrap." << '\n';
        }
        interval_ms = kMaxSafeIntervalMs;
    }
    if (interval_ms <= 0.0) interval_ms = 100.0;

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

    const int OVERALL_TIMEOUT_SECONDS = 60;
    const int max_samples = std::max(1, static_cast<int>(
        (OVERALL_TIMEOUT_SECONDS * 1000.0) / interval_ms));

    long long final_cas_rd = 0, final_cas_wr = 0;
    double    final_elapsed = interval_ms / 1000.0;
    bool      success = false;
    int       total_samples = 0;

    uint32_t prev_rd = imc_->reads();
    uint32_t prev_wr = imc_->writes();
    double   prev_t  = now_seconds();

    const auto loop_start = std::chrono::steady_clock::now();

    while (total_samples < max_samples) {
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - loop_start).count() > OVERALL_TIMEOUT_SECONDS) {
            std::cerr << "\n    ERROR: IMC stabilization timed out after "
                      << OVERALL_TIMEOUT_SECONDS << " seconds." << '\n';
            traffic_gen_manager_->kill_all_traffic_gen();
            samples_taken = total_samples;
            return false;
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long long>(interval_ms * 1000.0)));

        const uint32_t rd = imc_->reads();
        const uint32_t wr = imc_->writes();
        const double   t  = now_seconds();
        const double   dt = t - prev_t;

        const long long sample_rd = static_cast<long long>(delta32(rd, prev_rd));
        const long long sample_wr = static_cast<long long>(delta32(wr, prev_wr));
        prev_rd = rd; prev_wr = wr; prev_t = t;

        if (dt <= 0.0) continue;

        // Normalised to the nominal interval: the aggregation below multiplies a sample count
        // by it.
        const double target_duration = interval_ms / 1000.0;
        const double normalization   = target_duration / dt;
        const long long norm_rd = static_cast<long long>(sample_rd * normalization);
        const long long norm_wr = static_cast<long long>(sample_wr * normalization);

        total_samples++;
        if (on_sample) on_sample();

        final_cas_rd  = norm_rd;
        final_cas_wr  = norm_wr;
        final_elapsed = target_duration;

        const double bw_gb_s = static_cast<double>(norm_rd + norm_wr) *
                               cache_line_size * scaling_factor / (target_duration * 1e9);

        if (total_samples % 20 == 0 &&
            !traffic_gen_manager_->is_traffic_gen_running(
                traffic_gen_manager_->active_traffic_gen_pid())) {
            if (relaunch_attempts < 2 && relaunch_current_traffic_gen()) {
                relaunch_attempts++;
                stabilizer.reset();
                prev_rd = imc_->reads();
                prev_wr = imc_->writes();
                prev_t  = now_seconds();
                continue;
            }
            samples_taken = total_samples;
            return false;
        }

        const StabilizationResult result = stabilizer.add_sample(norm_rd, norm_wr, bw_gb_s);

        if (result == StabilizationResult::ZOMBIE_DETECTED) {
            if (verbose2) {
                std::cout << "    [ZOMBIE] TrafficGen died, aborting stabilization" << '\n';
            }
            samples_taken = total_samples;
            return false;
        }

        if (verbose3) {
            const long long total_cas = norm_rd + norm_wr;
            const double read_ratio = (total_cas > 0)
                ? static_cast<double>(norm_rd) / static_cast<double>(total_cas)
                : 0.0;
            std::cout << "      [Sample " << total_samples << "] CAS: " << total_cas;
            stabilizer.print_status(read_ratio, bw_gb_s, true);
        }

        if (stabilizer.is_stable()) {
            success = true;
            break;
        }
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

    last_cas_rd   = final_cas_rd;
    last_cas_wr   = final_cas_wr;
    last_elapsed  = final_elapsed;
    samples_taken = total_samples;
    return false;
}

bool WindowsImcBandwidthMeasurer::monitor_command(const std::string& command,
                                                  MonitorCallback callback,
                                                  bool summary_mode) {
    if (!imc_) return false;

    auto child = platform::spawn_shell(command);
    if (!child) return false;

    const double target = std::max(1.0, sampling_interval_ms_) / 1000.0;

    uint32_t prev_rd = imc_->reads();
    uint32_t prev_wr = imc_->writes();
    double   prev_t  = now_seconds();
    const double start_t = prev_t;

    long long total_rd = 0, total_wr = 0;

    while (child->running()) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long long>(target * 1e6)));

        const uint32_t rd = imc_->reads();
        const uint32_t wr = imc_->writes();
        const double   t  = now_seconds();
        const double   dt = t - prev_t;

        const long long d_rd = static_cast<long long>(delta32(rd, prev_rd));
        const long long d_wr = static_cast<long long>(delta32(wr, prev_wr));
        prev_rd = rd; prev_wr = wr; prev_t = t;

        total_rd += d_rd;
        total_wr += d_wr;

        if (!summary_mode && callback && dt > 0.0) {
            const double bw = static_cast<double>(d_rd + d_wr) * 64.0 / (dt * 1e9);
            callback(t - start_t, bw, d_rd, d_wr, MonitorSampleExtras{});
        }
    }

    if (summary_mode && callback) {
        const double elapsed = now_seconds() - start_t;
        const double bw = (elapsed > 0.0)
            ? static_cast<double>(total_rd + total_wr) * 64.0 / (elapsed * 1e9)
            : 0.0;
        callback(elapsed, bw, total_rd, total_wr, MonitorSampleExtras{});
    }
    return true;
}
