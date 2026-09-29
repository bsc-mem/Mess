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

#pragma once
#include <algorithm>
#include <chrono>
#include <string>

namespace measurement::uprof {
// A one-second collection has no complete report interval; uProf needs two.
inline constexpr int kUprofMinDurationSeconds = 2;
inline constexpr int kUprofReportIntervalMs = 1000;
// Callers use the same effective duration when converting GB/s back to counts.
inline int uprof_collection_seconds(int requested) {
    return std::max(kUprofMinDurationSeconds, requested);
}

// Each invocation owns a temporary directory. Failed processes never yield CSV.
// A negative timeout selects the collection duration plus a 60-second allowance.
std::string run_collection(const std::string& binary, int duration_s, bool cumulative,
                           int interval_ms, std::string& error,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
} // namespace measurement::uprof
