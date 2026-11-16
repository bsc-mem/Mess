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

#include "measurement/CasSizeCalibrator.h"
#include "measurement/PerfStatCsv.h"
#include "utils/CpuTopology.h"
#include "utils/SubprocessCapture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

namespace {

constexpr int kCandidateSizes[] = {16, 32, 64, 128};
constexpr double kSnapTolerance = 0.15;
constexpr double kMinimumBytesPerPhase = 4e9;

}

int snap_bytes_per_cas(double bytes_per_cas) {
    if (!(bytes_per_cas > 0.0)) return 0;
    int nearest = 0;
    double nearest_distance = 0.0;
    for (int candidate : kCandidateSizes) {
        const double distance = std::fabs(std::log2(bytes_per_cas / candidate));
        if (nearest == 0 || distance < nearest_distance) {
            nearest = candidate;
            nearest_distance = distance;
        }
    }
    return std::fabs(bytes_per_cas / nearest - 1.0) <= kSnapTolerance ? nearest : 0;
}

CasSizeCalibration summarize_cas_size(double read_bytes, long long read_cas, double write_bytes, long long write_cas) {
    CasSizeCalibration result;
    result.bytes_moved = read_bytes + write_bytes;
    if (read_bytes < kMinimumBytesPerPhase || write_bytes < kMinimumBytesPerPhase) {
        result.error = "calibration traffic too small";
        return result;
    }
    if (read_cas <= 0 || write_cas <= 0) {
        result.error = "no CAS commands counted on the target sockets";
        return result;
    }
    result.read_bytes_per_cas = read_bytes / static_cast<double>(read_cas);
    result.write_bytes_per_cas = write_bytes / static_cast<double>(write_cas);
    const int read_size = snap_bytes_per_cas(result.read_bytes_per_cas);
    const int write_size = snap_bytes_per_cas(result.write_bytes_per_cas);
    std::ostringstream measured;
    measured.precision(3);
    measured << "read " << result.read_bytes_per_cas << " B/CAS, write " << result.write_bytes_per_cas << " B/CAS";
    if (read_size == 0 || write_size == 0) {
        result.error = measured.str() + " is not close to a power of two";
        return result;
    }
    if (read_size != write_size) {
        result.error = measured.str() + " disagree";
        return result;
    }
    result.bytes_per_cas = read_size;
    return result;
}

CasSizeCalibration calibrate_strategy_cas_size(BandwidthCounterStrategy& strategy, const std::vector<int>& cpus, const std::vector<int>& mem_nodes) {
    if (!strategy.needs_cas_size_calibration()) return CasSizeCalibration{};
    CasSizeCalibration calibration = calibrate_cas_size(strategy.cas_size_calibration_counters(), cpus, mem_nodes);
    calibration.attempted = true;
    if (calibration.ok()) strategy.set_bytes_per_cas(calibration.bytes_per_cas);
    return calibration;
}

std::string describe_cas_size_calibration(const CasSizeCalibration& calibration, int bytes_per_cas) {
    std::ostringstream text;
    if (calibration.ok()) {
        text.setf(std::ios::fixed);
        text.precision(1);
        text << "HBM bytes per CAS: " << bytes_per_cas << " (measured: read " << calibration.read_bytes_per_cas
             << ", write " << calibration.write_bytes_per_cas << ")";
    } else {
        text << "Warning: could not measure HBM bytes per CAS (" << calibration.error << "); using default " << bytes_per_cas << " bytes.";
    }
    return text.str();
}

#ifdef __linux__

namespace {

constexpr size_t kBufferBytes = size_t(2) << 30;
constexpr size_t kChunkBytes = size_t(4) << 20;
constexpr auto kPerfStartup = std::chrono::milliseconds(400);
constexpr auto kPhaseDuration = std::chrono::milliseconds(600);
constexpr auto kPerfWindow = std::chrono::milliseconds(2000);
constexpr int kMaxAttempts = 3;
constexpr long kMpolBind = 2;

enum Phase : int { kIdle, kRead, kWrite, kStop };

struct TrafficTeam {
    std::atomic<int> phase{kIdle};
    std::atomic<int> active{0};
    std::atomic<int> ready{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> sink{0};
};

struct WindowCounts {
    double read_bytes = 0.0;
    double write_bytes = 0.0;
    long long read_cas = 0;
    long long write_cas = 0;
};

bool bind_to_nodes(void* buffer, size_t bytes, const std::vector<int>& nodes) {
    const int max_node = *std::max_element(nodes.begin(), nodes.end());
    const size_t bits = sizeof(unsigned long) * 8;
    std::vector<unsigned long> mask(static_cast<size_t>(max_node) / bits + 1, 0UL);
    for (int node : nodes) mask[static_cast<size_t>(node) / bits] |= 1UL << (static_cast<size_t>(node) % bits);
    return syscall(SYS_mbind, buffer, bytes, kMpolBind, mask.data(), static_cast<unsigned long>(max_node) + 2, 0) == 0;
}

void read_chunk(const uint64_t* data, size_t words, TrafficTeam& team) {
    uint64_t sum = 0;
    for (size_t i = 0; i < words; ++i) sum += data[i];
    team.sink.fetch_xor(sum, std::memory_order_relaxed);
}

void write_chunk(uint64_t* data, size_t words, uint64_t value) {
#if defined(__x86_64__) || defined(__i386__)
    const __m128i v = _mm_set1_epi64x(static_cast<long long>(value));
    for (size_t i = 0; i < words; i += 2) _mm_stream_si128(reinterpret_cast<__m128i*>(data + i), v);
    _mm_sfence();
#else
    std::fill(data, data + words, value);
#endif
}

void run_worker(int cpu, uint64_t* slice, size_t slice_bytes, TrafficTeam& team) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    std::memset(slice, 1, slice_bytes);
    team.ready.fetch_add(1);

    size_t offset = 0;
    uint64_t pass = 0;
    while (true) {
        team.active.fetch_add(1);
        const int phase = team.phase.load();
        if (phase == kStop) {
            team.active.fetch_sub(1);
            return;
        }
        if (phase == kIdle) {
            team.active.fetch_sub(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        const size_t chunk = std::min(kChunkBytes, slice_bytes - offset);
        uint64_t* data = slice + offset / sizeof(uint64_t);
        if (phase == kRead) {
            read_chunk(data, chunk / sizeof(uint64_t), team);
        } else {
            write_chunk(data, chunk / sizeof(uint64_t), ++pass);
        }
        team.bytes.fetch_add(chunk);
        team.active.fetch_sub(1);
        offset = (offset + chunk) % slice_bytes;
    }
}

std::vector<std::string> target_socket_scopes(const std::vector<int>& mem_nodes) {
    std::vector<std::string> scopes;
    for (int node : mem_nodes) {
        const int socket = cpu_topology::socket_of_memory_node(node);
        if (socket < 0) return {};
        const std::string scope = "S" + std::to_string(socket);
        if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) scopes.push_back(scope);
    }
    return scopes;
}

bool count_cas(const std::string& output, const BandwidthCounterSelection& selection, const std::vector<std::string>& scopes, WindowCounts& counts) {
    std::istringstream lines(output);
    std::string line;
    bool any = false;
    while (std::getline(lines, line)) {
        perf_stat_csv::Row row;
        if (!perf_stat_csv::parse_row(line, false, row)) continue;
        if (!scopes.empty() && !row.scope.empty() && std::find(scopes.begin(), scopes.end(), row.scope) == scopes.end()) continue;
        const EventClassification event = EventClassification::classify(row.event_name, selection);
        if (event.is_read) counts.read_cas += row.value;
        else if (event.is_write) counts.write_cas += row.value;
        else continue;
        any = true;
    }
    return any;
}

double run_phase(TrafficTeam& team, Phase phase) {
    team.bytes.store(0);
    team.phase.store(phase);
    std::this_thread::sleep_for(kPhaseDuration);
    team.phase.store(kIdle);
    while (team.active.load() != 0) std::this_thread::yield();
    return static_cast<double>(team.bytes.load());
}

bool measure_window(TrafficTeam& team, const std::string& events, const BandwidthCounterSelection& selection, const std::vector<std::string>& scopes, WindowCounts& counts, std::string& error) {
    const double window_s = std::chrono::duration<double>(kPerfWindow).count();
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        subprocess::Options opts;
        opts.timeout = kPerfWindow + std::chrono::seconds(10);
        opts.max_output_bytes = size_t(1) << 20;
        subprocess::Result perf;
        const auto launched = std::chrono::steady_clock::now();
        std::thread perf_thread([&]() {
            perf = subprocess::capture("perf", {"stat", "-a", "--per-socket", "-x,", "-e", events, "--", "sleep", std::to_string(window_s)}, opts);
        });
        std::this_thread::sleep_for(kPerfStartup);
        counts = WindowCounts{};
        counts.read_bytes = run_phase(team, kRead);
        counts.write_bytes = run_phase(team, kWrite);
        perf_thread.join();
        const double perf_overhead_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - launched).count() - window_s;

        if (!perf.ok()) {
            error = "perf stat failed while counting HBM CAS commands";
            return false;
        }
        if (perf_overhead_s > 0.8 * std::chrono::duration<double>(kPerfStartup).count()) continue;
        if (!count_cas(perf.output, selection, scopes, counts)) {
            error = "perf output contained no HBM CAS counts";
            return false;
        }
        return true;
    }
    error = "perf did not start counting before the calibration traffic";
    return false;
}

}

CasSizeCalibration calibrate_cas_size(const CasCounterSelection& counters, const std::vector<int>& cpus, const std::vector<int>& mem_nodes) {
    CasSizeCalibration result;
    if (!counters.has_read_write || counters.read_events.empty() || counters.write_events.empty()) {
        result.error = counters.failure_reason.empty() ? "no HBM CAS read/write counters available" : counters.failure_reason;
        return result;
    }
    if (cpus.empty() || mem_nodes.empty()) {
        result.error = "no CPUs or memory nodes to calibrate on";
        return result;
    }

    const size_t slice_bytes = (kBufferBytes / cpus.size()) / kChunkBytes * kChunkBytes;
    if (slice_bytes == 0) {
        result.error = "too many CPUs for the calibration buffer";
        return result;
    }
    const size_t buffer_bytes = slice_bytes * cpus.size();
    void* buffer = mmap(nullptr, buffer_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer == MAP_FAILED) {
        result.error = "could not allocate the calibration buffer";
        return result;
    }
    if (!bind_to_nodes(buffer, buffer_bytes, mem_nodes)) {
        munmap(buffer, buffer_bytes);
        result.error = "could not bind the calibration buffer to the target memory nodes";
        return result;
    }

    TrafficTeam team;
    std::vector<std::thread> workers;
    workers.reserve(cpus.size());
    for (size_t i = 0; i < cpus.size(); ++i) {
        uint64_t* slice = static_cast<uint64_t*>(buffer) + i * (slice_bytes / sizeof(uint64_t));
        workers.emplace_back(run_worker, cpus[i], slice, slice_bytes, std::ref(team));
    }
    while (team.ready.load() != static_cast<int>(cpus.size())) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    BandwidthCounterSelection selection;
    selection.type = CounterType::CAS_COUNT;
    selection.cas = counters;
    const std::string events = counters.get_read_events_string() + "," + counters.get_write_events_string();
    const std::vector<std::string> scopes = target_socket_scopes(mem_nodes);

    WindowCounts counts;
    std::string error;
    const bool measured = measure_window(team, events, selection, scopes, counts, error);

    team.phase.store(kStop);
    for (auto& worker : workers) worker.join();
    munmap(buffer, buffer_bytes);

    if (!measured) {
        result.error = error;
        return result;
    }
    return summarize_cas_size(counts.read_bytes, counts.read_cas, counts.write_bytes, counts.write_cas);
}

#else

CasSizeCalibration calibrate_cas_size(const CasCounterSelection&, const std::vector<int>&, const std::vector<int>&) {
    CasSizeCalibration result;
    result.error = "CAS size calibration requires Linux";
    return result;
}

#endif
