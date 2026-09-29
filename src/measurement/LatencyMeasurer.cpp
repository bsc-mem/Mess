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

#include "Utils.h"
#include "Measurement.h"
#include "measurement/BurstCompletion.h"
#include <algorithm>
#ifdef __linux__
#include <asm/unistd.h>
#endif
#include <chrono>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#ifdef __linux__
#include <linux/perf_event.h>
#endif
#include <sstream>
#include <thread>

LatencyMeasurer::LatencyMeasurer(const BenchmarkConfig& config, MeasurementStorage* storage, PtrChaseProcessManager* ptrchase_manager, std::function<std::vector<int>()> numa_resolver, int cache_line_size)
    : config_(config),
      ptrchase_manager_(ptrchase_manager),
      numa_resolver_(std::move(numa_resolver)) {
    (void)storage;
    (void)cache_line_size;
}

bool LatencyMeasurer::start_burst_async() {
    if (burst_running_ || burst_failed()) {
        return false;
    }
    
    if (ptrchase_manager_->pid() <= 0) {
        return fail_burst("pointer-chase process is not running");
    }
    
    if (!ptrchase_manager_->ensure_semaphores()) {
        return fail_burst("cannot open pointer-chase semaphores");
    }

    ptrchase_manager_->drain_done_sem();

    burst_pipe_ = platform::MessagePipe::open_reader(ptrchase_manager_->pipe_path());
    if (!burst_pipe_.valid()) {
        if (config_.verbosity >= 2) {
            std::cerr << "    [BURST] Failed to open pipe async (" << ptrchase_manager_->pipe_path() << "): " << std::strerror(errno) << std::endl;
        }
        return fail_burst("cannot open pointer-chase result pipe");
    }

    if (!ptrchase_manager_->start_sem().post()) {
        return fail_burst("cannot start pointer-chase burst");
    }

    burst_start_time_ = std::chrono::steady_clock::now();
    burst_running_ = true;
    
    return true;
}

bool LatencyMeasurer::try_collect_burst_async(PerfBurstCounters& out) {
    if (!burst_running_) {
        return false;
    }

    std::string line, error;
    const auto state = measurement::poll_burst_completion(
        ptrchase_manager_->done_sem(), burst_pipe_, ptrchase_manager_->pid(),
        burst_start_time_ + std::chrono::seconds(60), line, error);
    if (state == measurement::BurstCompletion::Pending) return false;
    if (state != measurement::BurstCompletion::Complete) return fail_burst(error);

    burst_pipe_.close();
    burst_running_ = false;

    // Require complete numeric fields. A malformed record must not become a
    // zero-valued sample, or repeatedly restart outside the bounded retry loop.
    std::map<std::string, long long> values;
    std::istringstream fields(line);
    std::string field;
    while (fields >> field) {
        const auto equal = field.find('=');
        if (equal == std::string::npos) return fail_burst("malformed pointer-chase result");
        try {
            size_t consumed = 0;
            const std::string number = field.substr(equal + 1);
            const long long value = std::stoll(number, &consumed);
            if (consumed != number.size()) return fail_burst("malformed pointer-chase counter");
            values[field.substr(0, equal)] = value;
        } catch (...) {
            return fail_burst("malformed pointer-chase counter");
        }
    }
    for (const char* key : {"cycles", "instructions", "tlb1", "tlb2",
                            "duration_ns", "accesses", "pointer_chase_thread_ns", "hugepages"}) {
        if (!values.count(key) || values[key] < 0) return fail_burst("missing or invalid pointer-chase counter");
    }
    if (values["duration_ns"] == 0 || values["accesses"] == 0 || values["pointer_chase_thread_ns"] == 0) {
        return fail_burst("missing pointer-chase timing or accesses; regenerate ptr_chase");
    }
    out = PerfBurstCounters{};
    out.cycles = static_cast<double>(values["cycles"]);
    out.instructions = static_cast<double>(values["instructions"]);
    out.tlb1miss = static_cast<double>(values["tlb1"]);
    out.tlb2miss = static_cast<double>(values["tlb2"]);
    out.using_hugepages = values["hugepages"] != 0;
    out.duration_s = static_cast<double>(values["duration_ns"]) / 1e9;
    out.accesses = static_cast<double>(values["accesses"]);
    out.pointer_chase_thread_ns = static_cast<double>(values["pointer_chase_thread_ns"]);
    out.tlb1_event_name = expected_tlb1_event_;
    out.tlb2_event_name = expected_tlb2_event_;
    return true;
}

bool LatencyMeasurer::fail_burst(const std::string& error) {
    reset();
    burst_error_ = error;
    return false;
}

void LatencyMeasurer::reset() {
    burst_pipe_.close();
    burst_running_ = false;
    burst_error_.clear();
}
