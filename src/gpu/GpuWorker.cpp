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

#include "gpu/GpuWorker.h"
#include <algorithm>
#include <utility>

namespace {
constexpr std::chrono::minutes kStartTimeout{5};
constexpr std::chrono::minutes kSampleTimeout{2};

std::chrono::milliseconds sample_timeout(int pause) {
    return kSampleTimeout + std::chrono::milliseconds(static_cast<long long>(pause) / 50);
}
}

GpuWorker::GpuWorker(std::string executable, std::vector<std::string> args, std::string working_dir, const std::string& log_path)
    : executable_(std::move(executable)), args_(std::move(args)), working_dir_(std::move(working_dir)),
      log_(log_path, std::ios::app) {}

bool GpuWorker::sample(const std::string& mode, int ratio_pct, int pause, std::string& line, std::string& error) {
    if (!process_ && !start(error)) return false;
    const std::string request = mode + " " + std::to_string(ratio_pct) + " " + std::to_string(pause);
    log_ << "> " << request << '\n';
    if (!process_->write_line(request)) {
        error = "GPU worker stopped accepting requests";
        stop();
        return false;
    }
    if (receive("SAMPLE", sample_timeout(pause), line, error)) return true;
    stop();
    return false;
}

void GpuWorker::stop() {
    process_.reset();
    log_.flush();
}

bool GpuWorker::start(std::string& error) {
    process_ = platform::Coprocess::spawn(executable_, args_, working_dir_, error);
    if (!process_) return false;
    if (receive("READY", kStartTimeout, ready_, error)) return true;
    stop();
    return false;
}

bool GpuWorker::receive(const std::string& expected, std::chrono::milliseconds timeout, std::string& line, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        switch (process_->read_line(line, std::max(remaining, std::chrono::milliseconds(0)))) {
            case platform::ReceiveStatus::Data:
                log_ << line << '\n';
                if (line.rfind(expected + " ", 0) == 0) return true;
                if (line.rfind("ERROR", 0) == 0) {
                    error = "GPU worker: " + line;
                    return false;
                }
                continue;
            case platform::ReceiveStatus::Timeout:
                error = "GPU worker timed out waiting for " + expected;
                return false;
            case platform::ReceiveStatus::Eof:
                error = "GPU worker exited before " + expected;
                return false;
            default:
                error = "GPU worker output could not be read";
                return false;
        }
    }
}
