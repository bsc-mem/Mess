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

#ifndef IOREPORT_BANDWIDTH_MEASURER_H
#define IOREPORT_BANDWIDTH_MEASURER_H

#include "Measurement.h"

class IOReportBandwidthMeasurer : public BandwidthMeasurer {
public:
    IOReportBandwidthMeasurer(const BenchmarkConfig& config,
                              const system_info& sys_info,
                              const CPUCapabilities& caps,
                              MeasurementStorage* storage,
                              TrafficGenProcessManager* traffic_gen_manager,
                              std::function<std::vector<int>()> numa_resolver,
                              ExecutionMode mode = ExecutionMode::MULTISEQUENTIAL);
    ~IOReportBandwidthMeasurer() override;

    bool sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed,
                          const std::vector<int>& mem_nodes) const override;
    bool pools_stabilized_window() const override { return true; }

    bool wait_for_stabilization(int& samples_collected, long long& bw_cas_rd, long long& bw_cas_wr,
                                double& bw_elapsed, int pause, int ratio_pct,
                                bool reuse_existing_traffic_gen,
                                std::function<void()> on_sample_callback = nullptr) override;

private:
    struct IOReportState;
    IOReportState* state_ = nullptr;

    bool ensure_subscribed() const;
    void reset_subscription() const;
    bool restart_dcs_window() const;
    bool read_dcs_delta(long long& rd_bytes, long long& wr_bytes, double& dt_seconds) const;
};

#endif
