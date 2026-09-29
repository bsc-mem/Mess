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

#include "measurement/BurstCompletion.h"

namespace measurement {

BurstCompletion poll_burst_completion(
    platform::Semaphore& done, platform::MessagePipe& pipe, platform::ProcessId pid,
    std::chrono::steady_clock::time_point deadline,
    std::string& record, std::string& error) {
    record.clear();
    error.clear();
    if (!done.valid() || !pipe.valid() || pid <= 0) {
        error = "invalid pointer-chase IPC endpoint";
        return BurstCompletion::Failed;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        error = "pointer-chase burst exceeded its deadline";
        return BurstCompletion::TimedOut;
    }
    const auto ready = done.try_wait();
    if (ready != platform::WaitResult::Acquired) {
        if (ready == platform::WaitResult::Failed) {
            error = "pointer-chase completion semaphore failed";
            return BurstCompletion::Failed;
        }
        if (!platform::process_exists(pid)) {
            error = "pointer-chase process exited before completing its burst";
            return BurstCompletion::Failed;
        }
        return BurstCompletion::Pending;
    }

    char buffer[1024];
    // Bound both bytes and reads, including repeated signal interruptions.
    for (int attempt = 0; attempt < 8 && record.size() < sizeof(buffer); ++attempt) {
        std::size_t count = 0;
        const auto status = pipe.receive(buffer, sizeof(buffer) - record.size(),
                                         std::chrono::milliseconds(0), count);
        if (status == platform::ReceiveStatus::Data && count > 0) {
            record.append(buffer, static_cast<size_t>(count));
            if (record.back() == '\n') return BurstCompletion::Complete;
        } else if (status == platform::ReceiveStatus::Retry) {
            continue;
        } else {
            break;
        }
    }
    error = "missing, truncated, or unreadable pointer-chase result";
    return BurstCompletion::Failed;
}

}
