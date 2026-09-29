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
#include "measurement/VtuneCounterParser.h"
#include "BenchmarkConfig.h"
#include "SystemInfo.h"
#include <memory>

namespace measurement::vtune {

class CollectionSession {
public:
    CollectionSession();
    ~CollectionSession();
    CollectionSession(const CollectionSession&);
    CollectionSession& operator=(const CollectionSession&) = delete;
    VtuneSample collect(const std::string& binary, const BandwidthCounterSelection& selection,
        double seconds, int helper_cpu, int verbosity);
    std::vector<VtuneSample> collect_batch(const std::string& binary,
        const BandwidthCounterSelection& selection, double seconds, int loops,
        int helper_cpu, int verbosity);
    VtuneSample profile(const std::string& binary, const BandwidthCounterSelection& selection,
        const std::string& command, int verbosity);
    bool probe_available(int verbosity);
    std::string setup_guidance();
    std::string locate_binary();
    bool process_elevated();
    int helper_cpu(const system_info&, const BenchmarkConfig&, int cores);
    double window_seconds(double requested_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<CollectionSession> make_collection_session();
bool probe_available(int verbosity);
std::string setup_guidance();
std::string locate_binary();
bool process_elevated();
}
