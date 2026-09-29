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

#ifndef VTUNE_BANDWIDTH_MEASURER_H
#define VTUNE_BANDWIDTH_MEASURER_H

#include "Measurement.h"
#include "measurement/VtuneCollection.h"

/**
 * @file VtuneBandwidthMeasurer.h
 * @brief VTune-based implementation of the bandwidth measurer interface.
 */

/**
 * @brief Samples memory bandwidth through Intel VTune.
 */
class VtuneBandwidthMeasurer : public BandwidthMeasurer {
public:
    using BandwidthMeasurer::BandwidthMeasurer;

    bool sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed, const std::vector<int>& mem_nodes) const override;

    bool wait_for_stabilization(int& samples_collected, long long& bw_cas_rd, long long& bw_cas_wr, double& bw_elapsed, int pause, int ratio_pct, bool reuse_existing_traffic_gen, std::function<void()> on_sample_callback = nullptr) override;

    bool monitor_command(const std::string& command,
                         MonitorCallback callback,
                         bool summary_mode) override;

    std::string find_vtune_binary() const;

    /**
     * @brief Absolute path of the vtune binary, or empty when it is not installed.
     *
     * Static so AUTO resolution can ask before a measurer exists. No spawn.
     */
    static std::string locate_binary();

    static bool process_elevated();

    /**
     * @brief Whether VTune can actually read this machine's memory controllers.
     *
     * Installed is not working: a hypervisor or missing driver leaves a
     * collection that exits cleanly and reports nothing. Runs one real
     * collection per counter set tried, cached with the measurer's own probe.
     */
    static bool probe_available(int verbosity);

    /**
     * @brief Why VTune cannot collect here, phrased as something to do about it.
     *
     * Empty when VTune is absent or working. An unregistered driver and an
     * unsupported processor otherwise fail identically.
     */
    static std::string setup_guidance();
private:
    std::unique_ptr<measurement::vtune::CollectionSession> collection_ = measurement::vtune::make_collection_session();
};

#endif
