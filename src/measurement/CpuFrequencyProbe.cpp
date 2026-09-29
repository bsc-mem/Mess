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

#include "measurement/CpuFrequencyProbe.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cerrno>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

CpuFrequencyProbeResult summarize_cpu_frequency_samples(
    int cpu, const std::vector<CpuFrequencySample>& samples) {
    CpuFrequencyProbeResult result;
    result.cpu = cpu;
    std::vector<double> frequencies;
    for (const auto& sample : samples) {
        if (sample.cpu != cpu || sample.cycles == 0 ||
            !std::isfinite(sample.thread_seconds) || sample.thread_seconds < 0.01 ||
            sample.time_enabled_ns == 0 ||
            sample.time_running_ns != sample.time_enabled_ns) {
            continue;
        }
        double ghz = static_cast<double>(sample.cycles) / sample.thread_seconds / 1e9;
        if (std::isfinite(ghz) && ghz > 0.0) frequencies.push_back(ghz);
    }
    result.accepted_samples = frequencies.size();
    if (frequencies.size() < 5) {
        result.error = "fewer than five fully counted frequency samples";
        return result;
    }
    std::sort(frequencies.begin(), frequencies.end());
    const size_t middle = frequencies.size() / 2;
    const double median = frequencies[middle];
    // One transient at either end is allowed; the central samples must agree.
    result.spread_pct = 100.0 * (frequencies[frequencies.size() - 2] - frequencies[1]) / median;
    if (result.spread_pct > 3.0) {
        result.error = "CPU frequency did not settle during the frequency probe";
        return result;
    }
    result.frequency_ghz = median;
    return result;
}

#ifdef __linux__
namespace {
struct Counter {
    int fd = -1;
    ~Counter() { if (fd >= 0) close(fd); }
};

struct Affinity {
    cpu_set_t original{};
    bool changed = false;
    bool restore() {
        if (!changed) return true;
        if (sched_setaffinity(0, sizeof(original), &original) != 0) return false;
        changed = false;
        return true;
    }
    ~Affinity() { restore(); }
};

struct CounterReading {
    uint64_t cycles = 0;
    uint64_t enabled = 0;
    uint64_t running = 0;
};

double clock_seconds(clockid_t clock) {
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) return -1.0;
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

bool run_integer_work(double seconds) {
    const double start = clock_seconds(CLOCK_MONOTONIC);
    if (start < 0.0) return false;
    uint64_t value = 1;
    double elapsed = 0.0;
    do {
        for (int i = 0; i < 65536; ++i) {
            value = value * 1664525ULL + 1013904223ULL;
            asm volatile("" : "+r"(value));
        }
        const double end = clock_seconds(CLOCK_MONOTONIC);
        if (end < 0.0) return false;
        elapsed = end - start;
    } while (elapsed < seconds);
    return true;
}
}
#endif

CpuFrequencyProbeResult probe_cpu_frequency(int cpu) {
    CpuFrequencyProbeResult result;
    result.cpu = cpu;
#ifdef __linux__
    Affinity affinity;
    if (cpu < 0 || cpu >= CPU_SETSIZE ||
        sched_getaffinity(0, sizeof(affinity.original), &affinity.original) != 0) {
        result.error = "cannot read CPU affinity for the startup frequency probe";
        return result;
    }
    if (!CPU_ISSET(cpu, &affinity.original)) {
        result.error = "pointer-chase CPU " + std::to_string(cpu) + " is outside the allowed CPU set";
        return result;
    }
    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(cpu, &target);
    if (sched_setaffinity(0, sizeof(target), &target) != 0) {
        result.error = "cannot pin the startup frequency probe: " + std::string(std::strerror(errno));
        return result;
    }
    affinity.changed = true;

    // One task-local counter, with the same user-mode scope as ptr_chase.
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    Counter counter;
    counter.fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
    if (counter.fd < 0) {
        result.error = "cannot open the CPU cycles counter: " + std::string(std::strerror(errno));
        return result;
    }

    if (!run_integer_work(0.1)) {
        result.error = "cannot read CLOCK_MONOTONIC";
        return result;
    }
    std::vector<CpuFrequencySample> samples;
    for (int attempt = 0; attempt < 7; ++attempt) {
        CounterReading before{}, after{};
        // RESET resets counts, but not enabled/running time; subtract those below.
        if (ioctl(counter.fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
            read(counter.fd, &before, sizeof(before)) != sizeof(before) ||
            ioctl(counter.fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
            result.error = "cannot start the CPU frequency counter";
            break;
        }
        const double start = clock_seconds(CLOCK_THREAD_CPUTIME_ID);
        const bool worked = run_integer_work(0.1);
        const double end = clock_seconds(CLOCK_THREAD_CPUTIME_ID);
        if (ioctl(counter.fd, PERF_EVENT_IOC_DISABLE, 0) != 0 ||
            read(counter.fd, &after, sizeof(after)) != sizeof(after) || !worked ||
            start < 0.0 || end <= start ||
            after.enabled < before.enabled || after.running < before.running) {
            result.error = "cannot read the CPU frequency sample";
            break;
        }
        samples.push_back({after.cycles, after.enabled - before.enabled,
                           after.running - before.running, end - start, sched_getcpu()});
        if (samples.size() >= 5) {
            result = summarize_cpu_frequency_samples(cpu, samples);
            if (result.ok()) break;
        }
    }
    if (result.error.empty() && !result.ok()) result = summarize_cpu_frequency_samples(cpu, samples);
    if (!affinity.restore()) {
        result.frequency_ghz = 0.0;
        result.error = "cannot restore CPU affinity after the frequency probe";
    }
#else
    result.error = "CPU cycle probing is only available on Linux";
#endif
    return result;
}
