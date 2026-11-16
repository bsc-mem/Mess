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

#ifndef CAS_SIZE_CALIBRATOR_H
#define CAS_SIZE_CALIBRATOR_H

#include "architecture/BandwidthCounterStrategy.h"

#include <string>
#include <vector>

struct CasSizeCalibration {
    bool attempted = false;
    int bytes_per_cas = 0;
    double read_bytes_per_cas = 0.0;
    double write_bytes_per_cas = 0.0;
    double bytes_moved = 0.0;
    std::string error;
    bool ok() const { return bytes_per_cas > 0 && error.empty(); }
};

int snap_bytes_per_cas(double bytes_per_cas);

CasSizeCalibration summarize_cas_size(double read_bytes, long long read_cas, double write_bytes, long long write_cas);

CasSizeCalibration calibrate_cas_size(const CasCounterSelection& counters, const std::vector<int>& cpus, const std::vector<int>& mem_nodes);

CasSizeCalibration calibrate_strategy_cas_size(BandwidthCounterStrategy& strategy, const std::vector<int>& cpus, const std::vector<int>& mem_nodes);

std::string describe_cas_size_calibration(const CasSizeCalibration& calibration, int bytes_per_cas);

#endif
