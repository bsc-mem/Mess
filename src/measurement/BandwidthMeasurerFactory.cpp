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

#include "Measurement.h"
#include "measurement/bw_measurers/PerfBandwidthMeasurer.h"
#include "measurement/bw_measurers/LikwidBandwidthMeasurer.h"
#include "measurement/bw_measurers/PcmBandwidthMeasurer.h"
#include "measurement/bw_measurers/VtuneBandwidthMeasurer.h"
#include "measurement/bw_measurers/IOReportBandwidthMeasurer.h"
#if defined(_WIN32)
#include "measurement/bw_measurers/WindowsImcBandwidthMeasurer.h"
#include "measurement/bw_measurers/AMDuProfBandwidthMeasurer.h"
#endif
#include "architecture/BandwidthCounterStrategy.h"
#if defined(_WIN32)
#include "platform/Sys.h"
#endif
#include <iostream>

namespace {
void announce_measurer(bool enabled, const char* name) {
    static bool announced = false;
    if (enabled && !announced) {
        std::cout << "Using " << name << std::endl;
        announced = true;
    }
}

void print_missing_tool(const char* tool, const char* mode, const char* requirement, const char* hint) {
    std::cerr << "\n\033[1;31m============================================================\033[0m" << std::endl;
    std::cerr << "\033[1;31m ERROR: Bandwidth Measurement Tool (" << tool << ") Not Found!\033[0m" << std::endl;
    std::cerr << "\033[1;31m------------------------------------------------------------\033[0m" << std::endl;
    std::cerr << " Mode: " << mode << std::endl;
    std::cerr << " Requirement: " << requirement << std::endl;
    if (hint) {
        std::cerr << " Hint: " << hint << std::endl;
    }
    std::cerr << "\n Aborting execution." << std::endl;
    std::cerr << "\033[1;31m============================================================\033[0m\n" << std::endl;
}
}

#if defined(_WIN32)
const WindowsAutoMeasurer& windows_auto_measurer(int verbosity) {
    static const WindowsAutoMeasurer choice = [verbosity] {
        WindowsAutoMeasurer c;

        if (platform::cpu_is_amd()) {
            c.type = MeasurerType::UPROF;
            c.available = AMDuProfBandwidthMeasurer::counters_available(c.diagnosis);
            return c;
        }

        if (!VtuneBandwidthMeasurer::locate_binary().empty() &&
            !VtuneBandwidthMeasurer::process_elevated()) {
            c.type = MeasurerType::VTUNE;
            c.available = false;
            c.diagnosis = "VTune is installed, but Mess is not running as Administrator. "
                          "Re-run Mess from an elevated prompt.";
            return c;
        }

        if (VtuneBandwidthMeasurer::probe_available(verbosity)) {
            c.type = MeasurerType::VTUNE;
            c.available = true;
            return c;
        }

        std::string imc_reason;
        if (WindowsImcBandwidthMeasurer::counters_available(imc_reason)) {
            c.type = MeasurerType::IMC;
            c.available = true;
            c.fell_back = !VtuneBandwidthMeasurer::locate_binary().empty();
            return c;
        }

        c.type = MeasurerType::IMC;
        c.available = false;
        c.diagnosis = imc_reason;

        const std::string vtune_help = VtuneBandwidthMeasurer::setup_guidance();
        if (!vtune_help.empty()) {
            c.diagnosis += "\n         " + vtune_help;
        }
        return c;
    }();
    return choice;
}

std::unique_ptr<BandwidthMeasurer> unavailable_on_windows(const char* backend,
                                                          const char* reason) {
    std::cerr << "Error: bandwidth backend '" << backend
              << "' is not available on Windows: " << reason << std::endl;
    std::cerr << "Use --measurer=auto, imc, uprof or vtune." << std::endl;
    return nullptr;
}
#endif

std::unique_ptr<BandwidthMeasurer> create_bandwidth_measurer(
    const BenchmarkConfig& config,
    const system_info& sys_info,
    const CPUCapabilities& caps,
    MeasurementStorage* storage,
    TrafficGenProcessManager* traffic_gen_manager,
    std::function<std::vector<int>()> numa_resolver,
    ExecutionMode mode) {

    auto& strategy = BandwidthCounterStrategy::instance();
    MeasurerType measurer_type = strategy.get_resolved_measurer_type();

    std::unique_ptr<BandwidthMeasurer> measurer;

    switch (measurer_type) {
        case MeasurerType::LIKWID: {
#if defined(_WIN32)
            return unavailable_on_windows("likwid", "LIKWID has no Windows port.");
#else
            if (!strategy.get_selection().failure_reason.empty()) {
                std::cerr << "Error: " << strategy.get_selection().failure_reason << '\n';
                return nullptr;
            }


            announce_measurer(config.verbosity >= 1, "Likwid Bandwidth Measurer");

            auto likwid_measurer = std::make_unique<LikwidBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);

            if (likwid_measurer->find_likwid_binary().empty()) {
                if (strategy.get_requested_measurer_type() == MeasurerType::LIKWID || strategy.is_hbm()) {
                    print_missing_tool("LIKWID",
                        strategy.get_requested_measurer_type() == MeasurerType::LIKWID
                            ? "Forced Likwid (--measurer=likwid)"
                            : "Auto-selected Likwid (HBM binding, perf HBM CAS counters unavailable)",
                        "'likwid-perfctr' must be in PATH.",
                        "install/load LIKWID or explicitly override with --measurer=perf.");
                    return nullptr;
                }
                std::cerr << "Warning: LIKWID backend unavailable; falling back to perf measurer." << std::endl;
                measurer = std::make_unique<PerfBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);
                break;
            }
            measurer = std::move(likwid_measurer);
            break;
#endif
        }

        case MeasurerType::PCM: {
#if defined(_WIN32)
            return unavailable_on_windows(
                "pcm", "the pcm-memory CLI it drives is POSIX-only.");
#else
            announce_measurer(config.verbosity >= 1, "Intel PCM Bandwidth Measurer");

            auto pcm_measurer = std::make_unique<PcmBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);
            if (pcm_measurer->find_pcm_binary().empty()) {
                print_missing_tool("Intel PCM",
                    strategy.get_requested_measurer_type() == MeasurerType::PCM
                        ? "Forced PCM (--measurer=pcm)"
                        : "Auto-selected PCM (CXL memory-node binding detected)",
                    "'pcm-memory' must be in PATH.",
                    "install Intel PCM or override with --measurer=perf.");
                return nullptr;
            }
            measurer = std::move(pcm_measurer);
            break;
#endif
        }

        case MeasurerType::VTUNE: {
            announce_measurer(config.verbosity >= 1, "Intel VTune Bandwidth Measurer");
            auto vtune_measurer = std::make_unique<VtuneBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);

            if (vtune_measurer->find_vtune_binary().empty()) {
                if (strategy.get_requested_measurer_type() == MeasurerType::VTUNE) {
                    print_missing_tool("VTune", "Forced VTune (--measurer=vtune)",
                        "'vtune' must be installed and available in PATH.", nullptr);
                    return nullptr;
                }
#if defined(_WIN32)
                return unavailable_on_windows("vtune", "VTune is installed but its binary was not found.");
#else
                std::cerr << "Warning: VTune backend unavailable; falling back to perf measurer." << std::endl;
                measurer = std::make_unique<PerfBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);
                break;
#endif
            }

            measurer = std::move(vtune_measurer);
            break;
        }


        case MeasurerType::IOREPORT: {
#if defined(_WIN32)
            return unavailable_on_windows("ioreport", "IOReport is an Apple Silicon framework.");
#else
            announce_measurer(config.verbosity >= 1, "IOReport Bandwidth Measurer (Apple Silicon AMC)");
            measurer = std::make_unique<IOReportBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);
            break;
#endif
        }

        case MeasurerType::IMC: {
#if defined(_WIN32)
            announce_measurer(config.verbosity >= 1, "IMC Bandwidth Measurer");

            auto imc_measurer = std::make_unique<WindowsImcBandwidthMeasurer>(
                config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);

            if (!imc_measurer->init()) {
                std::cerr << "Error: memory controller counters are not readable: "
                          << imc_measurer->error() << std::endl;
                std::cerr << "See the Windows section of README.md." << std::endl;
                return nullptr;
            }

            measurer = std::move(imc_measurer);
            break;
#else
            std::cerr << "Error: the IMC backend is only available on Windows." << std::endl;
            return nullptr;
#endif
        }

        case MeasurerType::UPROF: {
#if defined(_WIN32)
            announce_measurer(config.verbosity >= 1, "AMD uProf Bandwidth Measurer");

            auto uprof_measurer = std::make_unique<AMDuProfBandwidthMeasurer>(
                config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);

            if (uprof_measurer->find_uprof_binary().empty()) {
                std::cerr << "Error: AMDuProfPcm was not found. Install AMD uProf and ensure AMDuProfPcm is in PATH."
                          << std::endl;
                return nullptr;
            }

            std::string reason;
            if (!AMDuProfBandwidthMeasurer::counters_available(reason)) {
                std::cerr << "Error: AMD Data Fabric counters are not readable: "
                          << reason << std::endl;
                return nullptr;
            }

            measurer = std::move(uprof_measurer);
            break;
#else
            std::cerr << "Error: the uProf backend is only available on Windows." << std::endl;
            return nullptr;
#endif
        }

        case MeasurerType::PERF:
        default: {
#if defined(_WIN32)
            return unavailable_on_windows(
                "perf", "perf_event_open is a Linux system call.");
#else
            announce_measurer(config.verbosity >= 2, "Perf Bandwidth Measurer");
            measurer = std::make_unique<PerfBandwidthMeasurer>(config, sys_info, caps, storage, traffic_gen_manager, numa_resolver, mode);
            break;
#endif
        }
    }

    measurer->set_counter_selection(strategy.get_selection());

    return measurer;
}
