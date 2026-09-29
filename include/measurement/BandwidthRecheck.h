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

#include <cmath>
#include <algorithm>

namespace measurement {

struct BandwidthReading {
    long long read = 0;
    long long write = 0;
    double seconds = 0.0;
};

enum class BandwidthRecheckStatus { Accepted, Changed, Failed };

struct BandwidthRecheckResult {
    BandwidthRecheckStatus status = BandwidthRecheckStatus::Failed;
    BandwidthReading reading;
    double bandwidth = 0.0;
    int samples = 0;
};

inline bool bandwidth_changed(double baseline, double candidate, double noise_floor = 0.0) {
    return std::abs(candidate - baseline) > std::max(baseline * 0.05, noise_floor);
}

// Callbacks keep backend-specific conversion (CAS, bytes, flits) outside the
// acceptance policy and let tests feed realistic confirmation sequences.
template <typename Sample, typename Convert>
BandwidthRecheckResult confirm_bandwidth(
    double baseline, BandwidthReading first, Sample sample, Convert convert, double noise_floor = 0.0) {
    BandwidthRecheckResult result;
    result.reading = first;
    if (first.seconds <= 0.0 || first.read < 0 || first.write < 0) return result;
    result.samples = 1;
    result.bandwidth = convert(first);
    if (!bandwidth_changed(baseline, result.bandwidth, noise_floor)) {
        result.status = BandwidthRecheckStatus::Accepted;
        return result;
    }
    for (int i = 0; i < 2; ++i) {
        BandwidthReading next;
        if (!sample(next) || next.seconds <= 0.0 || next.read < 0 || next.write < 0) break;
        result.reading.read += next.read;
        result.reading.write += next.write;
        result.reading.seconds += next.seconds;
        ++result.samples;
    }
    // A single outlying observation cannot confirm stability or sustained drift.
    if (result.samples < 2) return result;
    result.bandwidth = convert(result.reading);
    result.status = bandwidth_changed(baseline, result.bandwidth, noise_floor)
        ? BandwidthRecheckStatus::Changed : BandwidthRecheckStatus::Accepted;
    return result;
}

}
