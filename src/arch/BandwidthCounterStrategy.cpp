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

#include "architecture/BandwidthCounterStrategy.h"
#include "architecture/ArchitectureRegistry.h"
#include "platform/Sys.h"
#include "SystemDetection.h"
#include "Utils.h"
#include "utils/PmuSysfs.h"
#include "utils/CpuTopology.h"
#include "utils/SubprocessCapture.h"
#include "utils/StringUtils.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <map>
#include <chrono>
#include <thread>
#include <unistd.h> // access(F_OK) on the sysfs PMU probe
#include <dirent.h>

std::string measurer_type_to_string(MeasurerType type) {
    switch (type) {
        case MeasurerType::AUTO: return "auto";
        case MeasurerType::PERF: return "perf";
        case MeasurerType::LIKWID: return "likwid";
        case MeasurerType::VTUNE: return "vtune";
        case MeasurerType::PCM: return "pcm";
        case MeasurerType::IOREPORT: return "ioreport";
        case MeasurerType::IMC: return "imc";
        case MeasurerType::UPROF: return "uprof";
        default: return "unknown";
    }
}

MeasurerType string_to_measurer_type(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "perf") return MeasurerType::PERF;
    if (lower == "likwid") return MeasurerType::LIKWID;
    if (lower == "vtune") return MeasurerType::VTUNE;
    if (lower == "pcm") return MeasurerType::PCM;
    if (lower == "ioreport") return MeasurerType::IOREPORT;
    if (lower == "imc") return MeasurerType::IMC;
    if (lower == "uprof") return MeasurerType::UPROF;
    return MeasurerType::AUTO;
}

namespace {

std::string join_events(const std::vector<std::string>& events, const std::string& sep = ",") {
    std::string result;
    for (size_t i = 0; i < events.size(); ++i) {
        result += events[i];
        if (i < events.size() - 1) result += sep;
    }
    return result;
}

bool has_usable_bandwidth_counters(const BandwidthCounterSelection& selection) {
    switch (selection.type) {
        case CounterType::UPI_FLITS:
            return selection.upi.has_rxl_txl;
        case CounterType::CAS_COUNT:
        case CounterType::NVIDIA_GRACE:
        case CounterType::TOTAL_BYTES:
            return selection.cas.has_read_write || selection.cas.has_combined_counter;
        case CounterType::UNKNOWN:
        default:
            return false;
    }
}

bool selection_uses_perf_hbm(const BandwidthCounterSelection& selection) {
    if (selection.type != CounterType::CAS_COUNT) {
        return false;
    }
    for (const auto& event : selection.cas.read_events) {
        if (string_utils::contains_ci(event, "uncore_hbm_") || string_utils::contains_ci(event, "uncore_type_14_")) {
            return true;
        }
    }
    for (const auto& event : selection.cas.write_events) {
        if (string_utils::contains_ci(event, "uncore_hbm_") || string_utils::contains_ci(event, "uncore_type_14_")) {
            return true;
        }
    }
    return false;
}

void append_unique_events(std::vector<std::string>& target, const std::vector<std::string>& extra) {
    for (const auto& event : extra) {
        if (std::find(target.begin(), target.end(), event) == target.end()) {
            target.push_back(event);
        }
    }
}

bool has_vtune_binary_available() {
    return run_command_success("command -v vtune >/dev/null 2>&1");
}

std::vector<std::string> find_pmu_instances(const std::string& base_pmu_name) {
    return pmu_sysfs::list_prefixed(base_pmu_name);
}

bool perf_event_probe_succeeds(const std::string& events) {
    if (events.empty()) {
        return false;
    }

    subprocess::Options opts;
    opts.timeout = std::chrono::seconds(3);
    opts.max_output_bytes = 8192;
    const subprocess::Result result = subprocess::capture(
        "perf",
        {"stat", "-a", "-e", events, "--", "sleep", "0.01"},
        opts);
    const std::string& output = result.output;

    return result.ok() &&
           output.find("Performance counter stats") != std::string::npos &&
           output.find("Cannot find PMU") == std::string::npos &&
           output.find("event syntax error") == std::string::npos &&
           output.find("No permission") == std::string::npos &&
           output.find("not supported") == std::string::npos &&
           output.find("<not supported>") == std::string::npos;
}

std::string intel_hbm_type14_event(const std::string& pmu_name, const char* umask, const std::string& label) {
    return pmu_name + "/event=0x05,umask=" + umask + ",name=" + label + "/";
}

std::string pick_canonical_alias(std::vector<std::string> candidates, const std::string& canonical) {
    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates) {
        if (string_utils::contains_ci(candidate, canonical)) {
            return candidate;
        }
    }
    return candidates.front();
}

bool discover_hbm_alias_events(const std::vector<std::string>& pmus,
                               std::vector<std::string>& read_events,
                               std::vector<std::string>& write_events) {
    auto is_cas = [](const std::string& event, const char* a, const char* b) {
        return string_utils::contains_ci(event, "cas") && (string_utils::contains_ci(event, a) || string_utils::contains_ci(event, b));
    };
    for (const auto& pmu : pmus) {
        const auto reads = pmu_sysfs::list_events(pmu, [&](const std::string& e) { return is_cas(e, "rd", "read"); });
        const auto writes = pmu_sysfs::list_events(pmu, [&](const std::string& e) { return is_cas(e, "wr", "write"); });
        if (reads.empty() || writes.empty()) {
            read_events.clear();
            write_events.clear();
            return false;
        }
        read_events.push_back(pick_canonical_alias(reads, "cas_count_rd"));
        write_events.push_back(pick_canonical_alias(writes, "cas_count_wr"));
    }
    return !read_events.empty() && read_events.size() == write_events.size();
}

std::vector<std::string> scan_pmu_event_dir(const std::string& pmu_name,
                                             std::function<bool(const std::string&)> filter = nullptr) {
    return pmu_sysfs::list_events(pmu_name, filter);
}

std::vector<std::string> extract_vtune_tokens_from_output(const std::string& output) {
    std::vector<std::string> events;
    std::set<std::string> seen;
    std::istringstream iss(output);
    std::string line;
    std::regex line_regex(R"(^\s*([A-Za-z0-9_./:-]+)\s{2,}.*$)");

    while (std::getline(iss, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, line_regex)) {
            continue;
        }

        std::string token = match[1].str();
        if (token.empty()) {
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(token[0])) && token[0] != '_') {
            continue;
        }

        if (seen.insert(token).second) {
            events.push_back(token);
        }
    }

    return events;
}

std::string run_perf_capture(const std::vector<std::string>& args) {
    subprocess::Options opts;
    opts.timeout          = std::chrono::seconds(10);
    opts.max_output_bytes = 65536;
    return subprocess::run("perf", args, opts);
}

std::string run_vtune_capture(const std::vector<std::string>& args) {
    subprocess::Options opts;
    opts.timeout          = std::chrono::seconds(15);
    opts.max_output_bytes = 262144;
    return subprocess::run("vtune", args, opts);
}

std::string strip_pmu_instance_suffix(const std::string& name) {
    size_t last_underscore = name.rfind('_');
    if (last_underscore != std::string::npos && last_underscore < name.length() - 1) {
        bool all_digits = true;
        for (size_t i = last_underscore + 1; i < name.length(); ++i) {
            if (!std::isdigit(name[i])) { all_digits = false; break; }
        }
        if (all_digits) return name.substr(0, last_underscore);
    }
    return name;
}

}

class BandwidthCounterStrategySingleton : public BandwidthCounterStrategy {
public:
    static BandwidthCounterStrategySingleton& instance() {
        static BandwidthCounterStrategySingleton instance;
        return instance;
    }
    
    CasCounterSelection detectCasCounters() override {
        if (delegate_strategy_) {
            delegate_strategy_->set_measurer_type(get_requested_measurer_type());
            return delegate_strategy_->detectCasCounters();
        }
        return discoverCasCountersForRequestedMeasurer();
    }
    
    void getTlbMissCounters(uint64_t& tlb1_raw, uint64_t& tlb2_raw, bool& use_tlb1, bool& use_tlb2) override {
        if (delegate_strategy_) {
            delegate_strategy_->getTlbMissCounters(tlb1_raw, tlb2_raw, use_tlb1, use_tlb2);
            return;
        }
        get_tlb_counters(tlb1_raw, tlb2_raw, use_tlb1, use_tlb2);
    }

    double computeTlbOverheadNs(double slot1, double slot2,
                                double freq_ghz,
                                double stlb_hit_latency_ns) const override {
        if (delegate_strategy_) {
            return delegate_strategy_->computeTlbOverheadNs(slot1, slot2, freq_ghz, stlb_hit_latency_ns);
        }
        return BandwidthCounterStrategy::computeTlbOverheadNs(slot1, slot2, freq_ghz, stlb_hit_latency_ns);
    }

    std::string formatTlbEventName(int slot, uint64_t raw_code) const override {
        if (delegate_strategy_) {
            return delegate_strategy_->formatTlbEventName(slot, raw_code);
        }
        return BandwidthCounterStrategy::formatTlbEventName(slot, raw_code);
    }

protected:
    BandwidthCounterStrategySingleton() = default;
};

CasCounterSelection BandwidthCounterStrategy::discoverCasCountersForRequestedMeasurer() const {
    switch (requested_measurer_type_) {
        case MeasurerType::VTUNE:
            return discoverFromVtune();
        case MeasurerType::PERF:
        case MeasurerType::AUTO:
        case MeasurerType::LIKWID:
        case MeasurerType::PCM:
        default:
            return discoverFromPerf();
    }
}

CasCounterSelection BandwidthCounterStrategy::discoverCasCountersForRequestedMeasurer(
    const std::vector<std::string>& preferred_read,
    const std::vector<std::string>& preferred_write,
    const std::vector<std::string>& preferred_combined) const {
    switch (requested_measurer_type_) {
        case MeasurerType::VTUNE:
            return discoverFromVtune(preferred_read, preferred_write, preferred_combined);
        case MeasurerType::PERF:
        case MeasurerType::AUTO:
        case MeasurerType::LIKWID:
        case MeasurerType::PCM:
        default:
            return discoverFromPerf(preferred_read, preferred_write, preferred_combined);
    }
}

BandwidthCounterStrategy& BandwidthCounterStrategy::instance() {
    return BandwidthCounterStrategySingleton::instance();
}

void BandwidthCounterStrategy::set_measurer_type(MeasurerType type) {
    requested_measurer_type_ = type;
}

void BandwidthCounterStrategy::set_extra_counters(const std::vector<std::string>& counters) {
    extra_counters_ = counters;
}

void BandwidthCounterStrategy::set_memory_type(const std::string& mem_type) {
    cached_memory_type_ = mem_type;
}

void BandwidthCounterStrategy::initialize(int src_cpu, const std::vector<int>& target_mem_nodes, const CPUCapabilities& caps) {
    if (initialized_) return;

    cached_src_cpu_ = src_cpu;
    target_nodes_ = target_mem_nodes;
    cached_is_intel_cpu_ = (caps.vendor == CPUVendor::INTEL);
    const auto numa_memory = detect_numa_memory_info(caps.model_name, caps.hasOnPackageHBM());
    cached_memory_type_ = summarize_numa_memory_type_for_nodes(
        numa_memory,
        target_mem_nodes,
        cached_memory_type_.empty() ? caps.memory_type : cached_memory_type_);
    cached_targets_cxl_nodes_ = numa_nodes_target_cxl_or_pmem(numa_memory, target_mem_nodes);
    if (cached_targets_cxl_nodes_) {
        cached_memory_type_ = "CXL";
    }

    is_hbm_ = (cached_memory_type_.find("HBM") != std::string::npos);

    delegate_strategy_.reset();
    auto arch = ArchitectureRegistry::instance().getArchitecture(caps);
    if (arch) {
        delegate_strategy_ = arch->createCounterStrategy(caps);
    }

    if (caps.isAppleSilicon()) {
        selection_ = BandwidthCounterSelection{};
        selection_.type = CounterType::CAS_COUNT;
        selection_.perf_available = true;
        if (delegate_strategy_) {
            selection_.cas = delegate_strategy_->detectCasCounters();
        }
        append_unique_events(selection_.extra_counters, extra_counters_);
        auto_selected_measurer_type_ = MeasurerType::IOREPORT;
        resolved_measurer_type_ = (requested_measurer_type_ != MeasurerType::AUTO)
                                      ? requested_measurer_type_
                                      : MeasurerType::IOREPORT;
        initialized_ = true;
        return;
    }

#if defined(_WIN32)
    auto_selected_measurer_type_ = (windows_auto_measurer_ != MeasurerType::AUTO)
                                        ? windows_auto_measurer_
                                        : (platform::cpu_is_amd() ? MeasurerType::UPROF : MeasurerType::IMC);
    resolved_measurer_type_ = (requested_measurer_type_ != MeasurerType::AUTO)
                                  ? requested_measurer_type_
                                  : auto_selected_measurer_type_;

    const char* read_label = "imc_dram_data_reads";
    const char* write_label = "imc_dram_data_writes";
    switch (resolved_measurer_type_) {
        case MeasurerType::UPROF:
            read_label = "uprof_umc_data_reads";
            write_label = "uprof_umc_data_writes";
            break;
        case MeasurerType::VTUNE:
            read_label = "vtune_imc_data_reads";
            write_label = "vtune_imc_data_writes";
            break;
        default:
            break;
    }

    selection_ = BandwidthCounterSelection{};
    selection_.type = CounterType::CAS_COUNT;
    selection_.perf_available = true;
    selection_.cas.perf_available = true;
    selection_.cas.has_read_write = true;
    selection_.cas.read_events = {read_label};
    selection_.cas.write_events = {write_label};
    selection_.cas.bytes_per_cas = 64;

    append_unique_events(selection_.extra_counters, extra_counters_);

    if (delegate_strategy_) {
        delegate_strategy_->getTlbMissCounters(cached_tlb1_raw_, cached_tlb2_raw_,
                                               cached_use_tlb1_, cached_use_tlb2_);
    }

    initialized_ = true;
    return;
#else
    discover_counters(src_cpu, target_mem_nodes);

    append_unique_events(selection_.extra_counters, extra_counters_);

    resolve_measurer_type();

    if (delegate_strategy_) {
        delegate_strategy_->getTlbMissCounters(cached_tlb1_raw_, cached_tlb2_raw_,
                                               cached_use_tlb1_, cached_use_tlb2_);
    }

    initialized_ = true;
#endif // _WIN32
}

void BandwidthCounterStrategy::resolve_measurer_type() {
    if (requested_measurer_type_ != MeasurerType::AUTO) {
        resolved_measurer_type_ = requested_measurer_type_;

        bool needs_cross_socket = needs_upi();
        if (requested_measurer_type_ == MeasurerType::PERF && is_hbm_ && !needs_cross_socket &&
            !selection_uses_perf_hbm(selection_)) {
            std::cerr << "Warning: Using perf on HBM system. Consider --measurer=likwid for better counter support." << std::endl;
        }
        if (requested_measurer_type_ == MeasurerType::LIKWID && cached_memory_type_ == "CXL") {
            std::cerr << "Warning: Using likwid on CXL system. Consider --measurer=pcm for CXL support." << std::endl;
        }
        if (requested_measurer_type_ == MeasurerType::VTUNE && cached_memory_type_ == "CXL") {
            std::cerr << "Warning: VTune support for CXL-specific bandwidth counters is not implemented; results may be incomplete." << std::endl;
        }
        if (cached_targets_cxl_nodes_ && requested_measurer_type_ != MeasurerType::PCM) {
            std::cerr << "Warning: Binding targets CXL memory-only NUMA nodes; "
                      << "--measurer=pcm is recommended." << std::endl;
        }
        return;
    }

    resolved_measurer_type_ = auto_selected_measurer_type_;
    if (cached_targets_cxl_nodes_ && cached_is_intel_cpu_) {
        resolved_measurer_type_ = MeasurerType::PCM;
        return;
    }
    if (resolved_measurer_type_ == MeasurerType::VTUNE && cached_memory_type_ == "CXL") {
        std::cerr << "Warning: VTune support for CXL-specific bandwidth counters is not implemented; results may be incomplete." << std::endl;
    } else if (cached_memory_type_ == "CXL") {
        std::cerr << "Warning: CXL detected but PCM was not auto-selected; using "
                  << measurer_type_to_string(resolved_measurer_type_) << "." << std::endl;
    }
}

void BandwidthCounterStrategy::discover_counters(int src_cpu, const std::vector<int>& target_mem_nodes) {
    if (requested_measurer_type_ != MeasurerType::AUTO) {
        selection_ = discoverBandwidthCounters(this, src_cpu, target_mem_nodes, requested_measurer_type_);
        auto_selected_measurer_type_ = requested_measurer_type_;
        return;
    }

    bool has_remote_target = false;
    bool has_local_target = false;
    for (int node : target_mem_nodes) {
        const CounterType target_type = detectCounterType(src_cpu, node);
        if (target_type == CounterType::UPI_FLITS) {
            has_remote_target = true;
        } else if (target_type == CounterType::CAS_COUNT) {
            has_local_target = true;
        }
    }
    bool needs_cross_socket = has_remote_target && !has_local_target;

    if (is_hbm_) {
        needs_cross_socket = false;
    }

    const bool allow_vtune_fallback = cached_is_intel_cpu_ && cached_memory_type_ != "CXL" && has_vtune_binary_available();

    auto discover_with_measurer = [&](MeasurerType type) {
        const MeasurerType saved = requested_measurer_type_;
        requested_measurer_type_ = type;
        BandwidthCounterSelection discovered = discoverBandwidthCounters(this, src_cpu, target_mem_nodes, type);
        requested_measurer_type_ = saved;
        return discovered;
    };

    if (cached_targets_cxl_nodes_ && cached_is_intel_cpu_) {
        selection_ = discover_with_measurer(MeasurerType::PCM);
        auto_selected_measurer_type_ = MeasurerType::PCM;
        return;
    }

    BandwidthCounterSelection perf_selection = discover_with_measurer(MeasurerType::PERF);
    if (has_usable_bandwidth_counters(perf_selection)) {
        selection_ = perf_selection;
        auto_selected_measurer_type_ = MeasurerType::PERF;
        return;
    }

    if (is_hbm_ && !needs_cross_socket) {
        selection_ = discover_with_measurer(MeasurerType::LIKWID);
        auto_selected_measurer_type_ = MeasurerType::LIKWID;
        return;
    }

    if (allow_vtune_fallback) {
        BandwidthCounterSelection vtune_selection = discover_with_measurer(MeasurerType::VTUNE);
        if (has_usable_bandwidth_counters(vtune_selection)) {
            selection_ = vtune_selection;
            auto_selected_measurer_type_ = MeasurerType::VTUNE;
            return;
        }

        if (!vtune_selection.failure_reason.empty()) {
            perf_selection.failure_reason = vtune_selection.failure_reason;
        }
    }

    selection_ = perf_selection;
    auto_selected_measurer_type_ = MeasurerType::PERF;
}

std::string BandwidthCounterStrategy::get_tool_name() const {
    switch (resolved_measurer_type_) {
        case MeasurerType::LIKWID:
            return is_hbm_ ? "LIKWID (HBM Mode)" : "LIKWID";
        case MeasurerType::VTUNE:
            return "Intel VTune";
        case MeasurerType::PCM:
            return "PCM (CXL Mode)";
        case MeasurerType::IMC:
            return "IMC counters";
        case MeasurerType::UPROF:
            return "AMD uProf (Data Fabric UMC counters)";
        case MeasurerType::IOREPORT:
            return "IOReport (Apple Silicon AMC)";
        case MeasurerType::PERF:
        default:
            if (is_hbm_ && needs_upi()) {
                return "perf (HBM with remote/cross-socket access)";
            } else if (selection_uses_perf_hbm(selection_)) {
                return "perf (HBM CAS)";
            } else if (selection_.type == CounterType::UPI_FLITS) {
                return "perf (UPI/Interconnect)";
            } else {
                return "perf (Standard)";
            }
    }
}

std::string BandwidthCounterStrategy::get_measurer_name() const {
    return measurer_type_to_string(resolved_measurer_type_);
}

void BandwidthCounterStrategy::get_tlb_counters(uint64_t& tlb1_raw, uint64_t& tlb2_raw, bool& use_tlb1, bool& use_tlb2) const {
    tlb1_raw = cached_tlb1_raw_;
    tlb2_raw = cached_tlb2_raw_;
    use_tlb1 = cached_use_tlb1_;
    use_tlb2 = cached_use_tlb2_;
}

void BandwidthCounterStrategy::print_counter_info(std::ostream& out) const {
    if (!initialized_) {
        out << "BandwidthCounterStrategy not initialized" << std::endl;
        return;
    }

    out << "Bandwidth Measurement Tool: " << get_tool_name() << std::endl;

    switch (resolved_measurer_type_) {
        case MeasurerType::LIKWID:
            if (!selection_.failure_reason.empty()) {
                out << "  Error: " << selection_.failure_reason << std::endl;
            } else {
                out << "  LIKWID Counters: CAS_COUNT_RD/WR (via likwid-perfctr)" << std::endl;
            }
            break;

        case MeasurerType::VTUNE:
            out << "  VTune Counters (via vtune runsa):" << std::endl;
            if (selection_.type == CounterType::UPI_FLITS) {
                if (selection_.upi.has_rxl_txl) {
                    out << "    RXL Events: " << selection_.upi.get_rxl_events_string() << std::endl;
                    out << "    TXL Events: " << selection_.upi.get_txl_events_string() << std::endl;
                } else {
                    out << "    No suitable interconnect counters found." << std::endl;
                    if (!selection_.upi.failure_reason.empty()) {
                        out << "    Reason: " << selection_.upi.failure_reason << std::endl;
                    }
                }
            } else if (selection_.cas.has_read_write) {
                out << "    Read Events: " << selection_.cas.get_read_events_string() << std::endl;
                out << "    Write Events: " << selection_.cas.get_write_events_string() << std::endl;
            } else if (selection_.cas.has_combined_counter) {
                out << "    Combined Events: " << selection_.cas.get_all_events_string() << std::endl;
            } else {
                out << "    No suitable CAS counters found." << std::endl;
                if (!selection_.cas.failure_reason.empty()) {
                    out << "    Reason: " << selection_.cas.failure_reason << std::endl;
                }
            }
            break;

        case MeasurerType::PCM:
            out << "  PCM Counters: (CXL support via Intel PCM)" << std::endl;
            break;
        case MeasurerType::IOREPORT:
            out << "IOReport Counters (Apple Silicon AMC):" << std::endl;
            if (selection_.cas.has_read_write) {
                out << "  Read Events: " << selection_.cas.get_read_events_string() << std::endl;
                out << "  Write Events: " << selection_.cas.get_write_events_string() << std::endl;
            } else {
                out << "  No suitable Apple Memory Controller counters found." << std::endl;
            }
            break;
            
        case MeasurerType::PERF:
        default:
            if (selection_.type == CounterType::UPI_FLITS) {
                out << "UPI/Interconnect Counters (Remote Bandwidth):" << std::endl;
                if (selection_.perf_available) {
                    if (selection_.upi.has_rxl_txl) {
                        out << "  RXL Events: " << selection_.upi.get_rxl_events_string() << std::endl;
                        out << "  TXL Events: " << selection_.upi.get_txl_events_string() << std::endl;
                        if (selection_.upi.requires_link_aggregation) {
                            out << "  (Link aggregation required: Yes)" << std::endl;
                        }
                    } else {
                        out << "  No suitable UPI counters found." << std::endl;
                        if (!selection_.upi.failure_reason.empty()) {
                            out << "  Reason: " << selection_.upi.failure_reason << std::endl;
                        }
                    }
                } else {
                    out << "  perf not available." << std::endl;
                }
            } else {
                if (selection_.type == CounterType::TOTAL_BYTES) {
                    out << "Raw Byte Counters (Local Bandwidth):" << std::endl;
                } else {
                    out << "CAS Counters (Local Bandwidth):" << std::endl;
                }
                if (selection_.perf_available) {
                    if (selection_.cas.has_read_write) {
                        out << "  Read Events: " << selection_.cas.get_read_events_string() << std::endl;
                        out << "  Write Events: " << selection_.cas.get_write_events_string() << std::endl;
                        if (selection_.cas.requires_channel_aggregation) {
                            out << "  (Aggregation required: Yes)" << std::endl;
                        }
                    } else if (selection_.cas.has_combined_counter) {
                        out << "  Combined Events: " << selection_.cas.get_all_events_string() << std::endl;
                    } else {
                        out << "  No suitable CAS counters found." << std::endl;
                        if (!selection_.cas.failure_reason.empty()) {
                            out << "  Reason: " << selection_.cas.failure_reason << std::endl;
                        }
                    }
                } else {
                    out << "  perf not available." << std::endl;
                }
            }
            break;
    }
}

bool BandwidthCounterStrategy::queryEventViaPerf(const std::string& event_name, uint32_t& type, uint64_t& config) {
    std::string safe_event;
    safe_event.reserve(event_name.size());
    for (char c : event_name) {
        bool valid = std::isalnum(static_cast<unsigned char>(c)) ||
                     c == '_' || c == '-' || c == '/' || c == '=' ||
                     c == ',' || c == ':' || c == '.';
        if (!valid) return false;
        safe_event += c;
    }
    if (safe_event.empty() || safe_event.size() > 255) return false;

    std::string output = run_perf_capture({"stat", "-vvv", "-e", safe_event, "true"});

    if (output.find("perf_event_attr") == std::string::npos) {
        return false;
    }

    bool found_type = false, found_config = false;

    size_t type_pos = output.find("\n  type");
    if (type_pos != std::string::npos) {
        size_t line_end = output.find('\n', type_pos + 1);
        if (line_end != std::string::npos) {
            std::string type_line = output.substr(type_pos, line_end - type_pos);
            size_t val_start = type_line.find_first_of("0123456789");
            if (val_start != std::string::npos) {
                size_t val_end = val_start;
                while (val_end < type_line.length() &&
                       std::isdigit(static_cast<unsigned char>(type_line[val_end]))) val_end++;
                try {
                    type = static_cast<uint32_t>(std::stoul(type_line.substr(val_start, val_end - val_start)));
                    found_type = true;
                } catch (...) {}
            }
        }
    }

    size_t config_pos = output.find("\n  config ");
    if (config_pos != std::string::npos) {
        size_t line_end = output.find('\n', config_pos + 1);
        if (line_end != std::string::npos) {
            std::string config_line = output.substr(config_pos, line_end - config_pos);
            size_t hex_pos = config_line.find("0x");
            if (hex_pos != std::string::npos) {
                size_t val_end = hex_pos + 2;
                while (val_end < config_line.length() &&
                       std::isxdigit(static_cast<unsigned char>(config_line[val_end]))) val_end++;
                try {
                    config = std::stoull(config_line.substr(hex_pos, val_end - hex_pos), nullptr, 16);
                    found_config = true;
                } catch (...) {}
            } else {
                size_t val_start = config_line.find_first_of("0123456789");
                if (val_start != std::string::npos) {
                    size_t val_end = val_start;
                    while (val_end < config_line.length() &&
                           std::isdigit(static_cast<unsigned char>(config_line[val_end]))) val_end++;
                    try {
                        config = std::stoull(config_line.substr(val_start, val_end - val_start));
                        found_config = true;
                    } catch (...) {}
                }
            }
        }
    }

    return found_type && found_config;
}

ResolvedPerfEvent BandwidthCounterStrategy::resolveEvent(const std::string& event_name) {
    ResolvedPerfEvent resolved(event_name);

    if (event_name.size() > 1 && event_name[0] == 'r') {
        try {
            resolved.config = std::stoull(event_name.substr(1), nullptr, 16);
            resolved.type = 4; 
            resolved.cpu = -1;
            resolved.valid = true;
            return resolved;
        } catch (...) {
            // Not a raw event, continue with normal resolution
        }
    }

    size_t slash = event_name.find('/');
    if (slash != std::string::npos) {
        std::string pmu_name = event_name.substr(0, slash);

        if (!pmu_sysfs::read_type(pmu_name, resolved.type)) {
            if (!queryEventViaPerf(event_name, resolved.type, resolved.config)) {
                return resolved; 
            }
            resolved.cpu = 0;
            resolved.valid = true;
            return resolved;
        }

        if (!pmu_sysfs::read_cpumask(pmu_name, resolved.cpu)) {
            resolved.cpu = 0;
        }
    }

    if (!queryEventViaPerf(event_name, resolved.type, resolved.config)) {
        return resolved;
    }

    if (slash == std::string::npos) {
        const uint32_t PERF_TYPE_HARDWARE_MAX = 4;
        if (resolved.type > PERF_TYPE_HARDWARE_MAX) {
            DIR* dir = opendir("/sys/bus/event_source/devices");
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string pmu_name = entry->d_name;
                    if (pmu_name[0] == '.') continue;

                    uint32_t pmu_type;
                    if (pmu_sysfs::read_type(pmu_name, pmu_type) && pmu_type == resolved.type) {
                        pmu_sysfs::read_cpumask(pmu_name, resolved.cpu);
                        break;
                    }
                }
                closedir(dir);
            }
            if (resolved.cpu < 0) resolved.cpu = 0;
        } else {
            resolved.cpu = -1;
        }
    }

    resolved.valid = true;
    return resolved;
}

/**
 * @brief Categorizes raw PMU events into logical bandwidth streams.
 * 
 * Since PMU string outputs from tools like `perf` or `vtune` contain raw event names 
 * (e.g., `unc_m_cas_count.rd`), this function maps those strings back into logical 
 * semantic groups: Reads, Writes, RXL (UPI receives), or TXL (UPI transmits). 
 * 
 * This enables the result processor to accurately sum multiple counter streams 
 * into unified Read vs. Write bandwidth metrics regardless of the underlying 
 * hardware nomenclature.
 * 
 * @param event_name The raw PMU event name string reported by the profiling tool.
 * @param selection The active counter selection struct which knows the expected events.
 * @return EventClassification A struct categorizing the event and indicating if it is valid.
 */
EventClassification EventClassification::classify(const std::string& event_name, const BandwidthCounterSelection& selection) {
    EventClassification result;

    auto match_any = [&](const std::vector<std::string>& list) -> bool {
        for (const auto& e : list) {
            if (string_utils::contains_ci(event_name, e)) return true;
        }
        return false;
    };
    
    for (const auto& extra : selection.extra_counters) {
        if (string_utils::contains_ci(event_name, extra)) {
            result.is_extra = true;
            result.extra_key = extra;
            return result;
        }
    }
    
    if (selection.type == CounterType::CAS_COUNT ||
        selection.type == CounterType::NVIDIA_GRACE ||
        selection.type == CounterType::TOTAL_BYTES) {
        if (match_any(selection.cas.combined_events)) { result.is_combined = true; return result; }

        if (string_utils::contains_ci(event_name, "umc_cas_cmd")) {
            if (string_utils::contains_ci(event_name, ".rd")) { result.is_read = true; return result; }
            if (string_utils::contains_ci(event_name, ".wr")) { result.is_write = true; return result; }
            if (string_utils::contains_ci(event_name, ".all")) { result.is_combined = true; return result; }
        }

        if (match_any(selection.cas.read_events)) { result.is_read = true; return result; }

        if (!result.is_read) {
            if (string_utils::contains_ci(event_name, "cas_count") && 
               (string_utils::contains_ci(event_name, "rd") || string_utils::contains_ci(event_name, "read"))) {
                result.is_read = true;
            }
            if (selection_uses_perf_hbm(selection) && string_utils::contains_ci(event_name, "hbm") && string_utils::contains_ci(event_name, "_rd")) {
                result.is_read = true;
            }
            if (string_utils::contains_ci(event_name, "nvidia_scf_pmu") && 
                (string_utils::contains_ci(event_name, "cmem_rd_data") || string_utils::contains_ci(event_name, "remote_socket_rd_data"))) {
                result.is_read = true;
            }
        }
        
        if (!result.is_read) {
            if (match_any(selection.cas.write_events)) { result.is_write = true; return result; }
            if (!result.is_write) {
                if (string_utils::contains_ci(event_name, "cas_count") && 
                   (string_utils::contains_ci(event_name, "wr") || string_utils::contains_ci(event_name, "write"))) {
                    result.is_write = true;
                }
                if (selection_uses_perf_hbm(selection) && string_utils::contains_ci(event_name, "hbm") && string_utils::contains_ci(event_name, "_wr")) {
                    result.is_write = true;
                }
                if (string_utils::contains_ci(event_name, "nvidia_scf_pmu") && 
                    (string_utils::contains_ci(event_name, "cmem_wr_total_bytes") || string_utils::contains_ci(event_name, "remote_socket_wr_total_bytes"))) {
                    result.is_write = true;
                }
            }
        }
    } else if (selection.type == CounterType::UPI_FLITS) {
        if (match_any(selection.upi.rxl_data_events)) { result.is_read = true; return result; }
        if (match_any(selection.upi.txl_data_events)) { result.is_write = true; return result; }
    }
    
    return result;
}

void EventClassification::accumulate_extra(std::map<std::string, long long>& target,
                                           const std::string& extra_key, long long value) {
    if (!extra_key.empty()) {
        target[extra_key] += value;
    }
}

void EventClassification::reset_extra_values(std::map<std::string, long long>& target) {
    for (auto& kv : target) {
        kv.second = 0;
    }
}

std::string CasCounterSelection::get_read_events_string() const {
    return join_events(read_events);
}

std::string CasCounterSelection::get_write_events_string() const {
    return join_events(write_events);
}

std::string CasCounterSelection::get_all_events_string() const {
    std::vector<std::string> all;
    all.insert(all.end(), read_events.begin(), read_events.end());
    all.insert(all.end(), write_events.begin(), write_events.end());
    all.insert(all.end(), combined_events.begin(), combined_events.end());
    return join_events(all);
}

std::string CasCounterSelection::get_popen_events_string() const {
    std::vector<std::string> events;

    if (has_read_write) {
        if (!read_events.empty()) {
            events.push_back(read_events.front());
        }
        if (!write_events.empty()) {
            events.push_back(write_events.front());
        }
    } else if (has_combined_counter) {
        if (!combined_events.empty()) {
            events.push_back(combined_events.front());
        }
    }

    return join_events(events);
}

CasCounterSelection BandwidthCounterStrategy::discoverIntelHbmFromPerf() {
    CasCounterSelection selection;
    if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }

    std::vector<std::string> read_events;
    std::vector<std::string> write_events;

    const std::vector<std::string> named_hbm_pmus = find_pmu_instances("uncore_hbm");
    bool found_hbm_pmus = !named_hbm_pmus.empty();
    if (found_hbm_pmus) {
        discover_hbm_alias_events(named_hbm_pmus, read_events, write_events);
    }

    if (read_events.empty() || write_events.empty()) {
        read_events.clear();
        write_events.clear();

        const std::vector<std::string> type14_pmus = find_pmu_instances("uncore_type_14");
        found_hbm_pmus = found_hbm_pmus || !type14_pmus.empty();
        read_events.reserve(type14_pmus.size());
        write_events.reserve(type14_pmus.size());

        for (size_t i = 0; i < type14_pmus.size(); ++i) {
            const std::string tag = "hbm" + std::to_string(i);
            read_events.push_back(intel_hbm_type14_event(type14_pmus[i], "0xcf", tag + "_rd"));
            write_events.push_back(intel_hbm_type14_event(type14_pmus[i], "0xf0", tag + "_wr"));
        }

        if (!read_events.empty()) {
            selection.bytes_per_cas = 32;
        }
    }

    if (read_events.empty() || write_events.empty()) {
        selection.failure_reason = found_hbm_pmus
            ? "Intel HBM PMUs are exposed but no perf CAS_COUNT read/write events were found"
            : "Intel HBM PMUs are not exposed by the kernel";
        return selection;
    }

    std::vector<std::string> probe_events;
    probe_events.reserve(read_events.size() + write_events.size());
    probe_events.insert(probe_events.end(), read_events.begin(), read_events.end());
    probe_events.insert(probe_events.end(), write_events.begin(), write_events.end());
    if (!perf_event_probe_succeeds(join_events(probe_events))) {
        selection.failure_reason = "Intel HBM PMUs are present but perf cannot open all CAS_COUNT events";
        return selection;
    }

    selection.perf_available = true;
    selection.read_events = std::move(read_events);
    selection.write_events = std::move(write_events);
    selection.has_read_write = true;
    selection.has_combined_counter = false;
    selection.requires_channel_aggregation = true;
    return selection;
}

CasCounterSelection BandwidthCounterStrategy::discoverIntelConsumerImcFromPerf() {
    CasCounterSelection selection;
    if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }

    const std::vector<std::string> imc_pmus = pmu_sysfs::list([](const std::string& name) {
        return name.rfind("uncore_imc", 0) == 0;
    });

    auto exact_event = [](const std::string& target) {
        return [target](const std::string& name) { return name == target; };
    };

    std::vector<std::string> read_events;
    std::vector<std::string> write_events;
    for (const auto& pmu : imc_pmus) {
        auto reads  = pmu_sysfs::list_events(pmu, exact_event("data_read"));
        auto writes = pmu_sysfs::list_events(pmu, exact_event("data_write"));
        if (reads.empty() || writes.empty()) {
            reads  = pmu_sysfs::list_events(pmu, exact_event("data_reads"));
            writes = pmu_sysfs::list_events(pmu, exact_event("data_writes"));
        }
   
        if (!reads.empty() && !writes.empty()) {
            read_events.push_back(reads.front());
            write_events.push_back(writes.front());
        }
    }

    if (read_events.empty() || write_events.empty()) {
        selection.failure_reason = imc_pmus.empty()
            ? "Intel consumer uncore_imc PMU is not exposed by the kernel"
            : "Intel consumer uncore_imc PMU exposes no data_read/data_write events";
        return selection;
    }

    std::vector<std::string> probe_events = read_events;
    probe_events.insert(probe_events.end(), write_events.begin(), write_events.end());
    if (!perf_event_probe_succeeds(join_events(probe_events))) {
        selection.failure_reason =
            "Intel consumer uncore_imc data_read/data_write events present but perf cannot open them";
        return selection;
    }

    selection.perf_available = true;
    selection.read_events = std::move(read_events);
    selection.write_events = std::move(write_events);
    selection.has_read_write = true;
    selection.has_combined_counter = false;
    selection.requires_channel_aggregation = (selection.read_events.size() > 1);
    return selection;
}

CasCounterSelection BandwidthCounterStrategy::discoverAmdUmcFromPerf() {
    CasCounterSelection selection;
    if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }

    // One amd_umc_N PMU per unified memory controller (one per DDR5 channel).
    const std::vector<std::string> umc_pmus = find_pmu_instances("amd_umc");
    const bool has_collapsed_pmu = pmu_sysfs::exists("amd_umc");
    if (umc_pmus.empty() && !has_collapsed_pmu) {
        selection.failure_reason = "amd_umc PMUs are not exposed by the kernel";
        return selection;
    }

    auto umc_cas_event = [](const std::string& suffix) {
        return [suffix](const std::string& name) {
            return string_utils::contains_ci(name, "cas_cmd") && string_utils::contains_ci(name, suffix);
        };
    };

    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> candidates;

    {
        std::vector<std::string> reads;
        std::vector<std::string> writes;
        for (const auto& pmu : umc_pmus) {
            const auto pmu_reads  = pmu_sysfs::list_events(pmu, umc_cas_event(".rd"));
            const auto pmu_writes = pmu_sysfs::list_events(pmu, umc_cas_event(".wr"));
            if (pmu_reads.empty() || pmu_writes.empty()) {
                continue;
            }
            reads.push_back(pmu_reads.front());
            writes.push_back(pmu_writes.front());
        }
        if (reads.size() == umc_pmus.size() && writes.size() == umc_pmus.size()) {
            candidates.emplace_back(std::move(reads), std::move(writes));
        }
    }

    if (!umc_pmus.empty()) {
        std::vector<std::string> reads;
        std::vector<std::string> writes;
        for (const auto& pmu : umc_pmus) {
            reads.push_back(pmu + "/umc_cas_cmd.rd/");
            writes.push_back(pmu + "/umc_cas_cmd.wr/");
        }
        candidates.emplace_back(std::move(reads), std::move(writes));
    }

    candidates.push_back({{"amd_umc/umc_cas_cmd.rd/"}, {"amd_umc/umc_cas_cmd.wr/"}});

    for (auto& candidate : candidates) {
        std::vector<std::string> probe_events = candidate.first;
        probe_events.insert(probe_events.end(), candidate.second.begin(), candidate.second.end());
        if (!perf_event_probe_succeeds(join_events(probe_events))) {
            continue;
        }

        selection.perf_available = true;
        selection.read_events = std::move(candidate.first);
        selection.write_events = std::move(candidate.second);
        selection.has_read_write = true;
        selection.has_combined_counter = false;
        selection.requires_channel_aggregation = (selection.read_events.size() > 1);
        return selection;
    }

    selection.failure_reason = "amd_umc PMUs are present but perf cannot open umc_cas_cmd read/write events";
    return selection;
}

CasCounterSelection BandwidthCounterStrategy::discoverFromPerf() {
    CasCounterSelection selection;
    if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }

    selection.perf_available = true;
    std::vector<std::string> detected_events = extractCasEventsFromPerf();
    if (detected_events.empty()) {
        CasCounterSelection consumer_imc = discoverIntelConsumerImcFromPerf();
        if (consumer_imc.has_read_write || consumer_imc.has_combined_counter) {
            return consumer_imc;
        }
        selection.failure_reason = "perf list did not report CAS counters";
        return selection;
    }

    std::vector<std::string> read_candidates;
    std::vector<std::string> write_candidates;
    std::vector<std::string> combined_candidates;
    bool has_unc_m_rd = false;
    bool has_unc_m_wr = false;

    auto is_weird_specific = [](const std::string& name) {
        return string_utils::contains_ci(name, "reg") || string_utils::contains_ci(name, "underfill") ||
               string_utils::contains_ci(name, "pre") || string_utils::contains_ci(name, "nonpre");
    };

    for (const auto& event : detected_events) {
        if (event.empty()) continue;
        if (event.size() <= 1 && event.back() == '/') continue;

        const std::string lowered_event = string_utils::to_lower(event);
        if (lowered_event == "unc_m_cas_count.rd") {
            has_unc_m_rd = true;
        } else if (lowered_event == "unc_m_cas_count.wr") {
            has_unc_m_wr = true;
        }

        bool is_combined = string_utils::contains_ci(event, "all");
        bool is_read = string_utils::contains_ci(event, "cas_count") && (string_utils::contains_ci(event, "rd") || string_utils::contains_ci(event, "read"));
        bool is_write = string_utils::contains_ci(event, "cas_count") && (string_utils::contains_ci(event, "wr") || string_utils::contains_ci(event, "write"));

        if (string_utils::contains_ci(event, "nvidia_scf_pmu")) {
            if (string_utils::contains_ci(event, "cmem_rd_data") || string_utils::contains_ci(event, "remote_socket_rd_data")) {
                is_read = true;
            } else if (string_utils::contains_ci(event, "cmem_wr_total_bytes") || string_utils::contains_ci(event, "remote_socket_wr_total_bytes")) {
                is_write = true;
            }
        }

        if (string_utils::contains_ci(event, "dram_channel_data_controller")) {
            is_combined = true;
        }
        
        if (is_combined) {
            combined_candidates.push_back(event);
        } else if (is_read) {
            read_candidates.push_back(event);
        } else if (is_write) {
            write_candidates.push_back(event);
        }
    }

    auto is_channel_specific = [](const std::string& name) {
        if (string_utils::contains_ci(name, "_sch") || string_utils::contains_ci(name, "_imc") ||
            string_utils::contains_ci(name, "/imc") || string_utils::contains_ci(name, "uncore_imc") ||
            string_utils::contains_ci(name, "dram_channel_data_controller")) return true;

        static const std::regex channel_regex(R"((ch|channel|unit|imc)[0-9]+)", std::regex_constants::icase);
        if (std::regex_search(name, channel_regex)) return true;

        return false;
    };

    auto select_best_counters = [&](const std::vector<std::string>& candidates) -> std::vector<std::string> {
        if (candidates.empty()) return {};

        std::vector<std::string> aggregates;
        std::vector<std::string> channels;
        std::vector<std::string> others;

        for (const auto& c : candidates) {
            if (is_weird_specific(c)) {
                others.push_back(c);
                continue;
            }

            if (is_channel_specific(c)) {
                channels.push_back(c);
            } else {
                aggregates.push_back(c);
            }
        }

        if (!aggregates.empty()) {
            std::sort(aggregates.begin(), aggregates.end(), [](const std::string& a, const std::string& b) {
                return a.length() < b.length();
            });
            return {aggregates[0]}; 
        }

        if (!channels.empty()) {
            std::vector<std::string> specific_channels;
            for (const auto& c : channels) {
                if (c.find("_sch") != std::string::npos ||
                    std::regex_search(c, std::regex(R"((ch|channel|unit|imc)[0-9]+)", std::regex_constants::icase))) {
                    specific_channels.push_back(c);
                }
            }

            if (!specific_channels.empty()) {
                return specific_channels;
            }
            return channels;
        }

        if (!others.empty()) {
             std::sort(others.begin(), others.end(), [](const std::string& a, const std::string& b) {
                return a.length() < b.length();
            });
            return {others[0]};
        }

        return {};
    };

    selection.read_events = select_best_counters(read_candidates);
    selection.write_events = select_best_counters(write_candidates);

    if (selection.read_events.empty() || selection.write_events.empty()) {
        selection.combined_events = select_best_counters(combined_candidates);
    } else {
        selection.combined_events.clear();
    }

    selection.has_read_write = !selection.read_events.empty() && !selection.write_events.empty();
    selection.has_combined_counter = !selection.combined_events.empty();

    selection.requires_channel_aggregation = (selection.read_events.size() > 1) || (selection.write_events.size() > 1);

    // Prefer package-level Emerald/Sapphire aggregate counters when available.
    if (has_unc_m_rd && has_unc_m_wr) {
        selection.read_events = {"unc_m_cas_count.rd"};
        selection.write_events = {"unc_m_cas_count.wr"};
        selection.combined_events.clear();
        selection.has_read_write = true;
        selection.has_combined_counter = false;
        selection.requires_channel_aggregation = false;
    }

    if (!selection.has_read_write && !selection.has_combined_counter) {
        CasCounterSelection consumer_imc = discoverIntelConsumerImcFromPerf();
        if (consumer_imc.has_read_write || consumer_imc.has_combined_counter) {
            return consumer_imc;
        }
        selection.failure_reason = "No usable CAS counters detected";
    }

    return selection;
}

CasCounterSelection BandwidthCounterStrategy::discoverFromPerf(
    const std::vector<std::string>& preferred_read,
    const std::vector<std::string>& preferred_write,
    const std::vector<std::string>& preferred_combined
) {
    auto result = discoverFromPerf();
    
    if (!preferred_read.empty() || !preferred_write.empty() || !preferred_combined.empty()) {
        std::vector<std::string> found_read;
        std::vector<std::string> found_write;
        std::vector<std::string> found_combined;
        
        for (const auto& event : preferred_read) {
            std::string encoding = lookupEventEncoding(event);
            if (!encoding.empty()) {
                found_read.push_back(encoding);
            }
        }
        
        for (const auto& event : preferred_write) {
            std::string encoding = lookupEventEncoding(event);
            if (!encoding.empty()) {
                found_write.push_back(encoding);
            }
        }
        
        for (const auto& event : preferred_combined) {
            std::string encoding = lookupEventEncoding(event);
            if (!encoding.empty()) {
                found_combined.push_back(encoding);
            }
        }
        
        if (!found_read.empty() && !found_write.empty()) {
            result.read_events = found_read;
            result.write_events = found_write;
            result.has_read_write = true;
            result.failure_reason = "";
        }
        if (!found_combined.empty()) {
            result.combined_events = found_combined;
            result.has_combined_counter = true;
            result.failure_reason = "";
        }
    }
    
    return result;
}

std::vector<std::string> BandwidthCounterStrategy::extractCasEventsFromVtune() {
    static std::vector<std::string> cache;
    static bool cached = false;

    if (cached) return cache;

    if (!run_command_success("command -v vtune >/dev/null 2>&1")) {
        cached = true;
        return cache;
    }

    const std::string output = run_vtune_capture({
        "-collect-with", "runsa",
        "-knob", "event-config=?",
        "--", "/bin/true"
    });

    std::vector<std::string> candidates = extract_vtune_tokens_from_output(output);
    for (const auto& evt : candidates) {
        const std::string lowered = string_utils::to_lower(evt);
        if (lowered.find("cas_count") != std::string::npos ||
            lowered.find("dram_channel_data_controller") != std::string::npos ||
            lowered.find("nvidia_scf_pmu") != std::string::npos) {
            cache.push_back(evt);
        }
    }

    cached = true;
    return cache;
}

CasCounterSelection BandwidthCounterStrategy::discoverFromVtune() {
    CasCounterSelection selection;
    if (!run_command_success("command -v vtune >/dev/null 2>&1")) {
        selection.failure_reason = "vtune binary not found";
        return selection;
    }

    selection.perf_available = true;
    std::vector<std::string> detected_events = extractCasEventsFromVtune();
    if (detected_events.empty()) {
        selection.failure_reason = "vtune event discovery did not report CAS counters";
        return selection;
    }

    std::vector<std::string> read_candidates;
    std::vector<std::string> write_candidates;
    std::vector<std::string> combined_candidates;

    auto is_weird_specific = [](const std::string& name) {
        return string_utils::contains_ci(name, "reg") || string_utils::contains_ci(name, "underfill") ||
               string_utils::contains_ci(name, "pre") || string_utils::contains_ci(name, "nonpre");
    };

    for (const auto& event : detected_events) {
        if (event.empty()) continue;

        bool is_combined = string_utils::contains_ci(event, ".all") || string_utils::contains_ci(event, "_all");
        bool is_read = string_utils::contains_ci(event, "cas_count") && (string_utils::contains_ci(event, ".rd") || string_utils::contains_ci(event, "_rd") || string_utils::contains_ci(event, "read"));
        bool is_write = string_utils::contains_ci(event, "cas_count") && (string_utils::contains_ci(event, ".wr") || string_utils::contains_ci(event, "_wr") || string_utils::contains_ci(event, "write"));

        if (string_utils::contains_ci(event, "nvidia_scf_pmu")) {
            if (string_utils::contains_ci(event, "cmem_rd_data") || string_utils::contains_ci(event, "remote_socket_rd_data")) {
                is_read = true;
            } else if (string_utils::contains_ci(event, "cmem_wr_total_bytes") || string_utils::contains_ci(event, "remote_socket_wr_total_bytes")) {
                is_write = true;
            }
        }

        if (string_utils::contains_ci(event, "dram_channel_data_controller")) {
            is_combined = true;
        }

        if (is_combined) {
            combined_candidates.push_back(event);
        } else if (is_read) {
            read_candidates.push_back(event);
        } else if (is_write) {
            write_candidates.push_back(event);
        }
    }

    auto is_channel_specific = [](const std::string& name) {
        static const std::regex channel_regex(R"((ch|channel|unit|imc|mbox|hbm)[0-9]+)", std::regex_constants::icase);
        return std::regex_search(name, channel_regex);
    };

    auto select_best_counters = [&](const std::vector<std::string>& candidates) -> std::vector<std::string> {
        if (candidates.empty()) return {};

        std::vector<std::string> aggregates;
        std::vector<std::string> channels;
        std::vector<std::string> others;

        for (const auto& c : candidates) {
            if (is_weird_specific(c)) {
                others.push_back(c);
                continue;
            }

            if (is_channel_specific(c)) {
                channels.push_back(c);
            } else {
                aggregates.push_back(c);
            }
        }

        if (!aggregates.empty()) return {aggregates.front()};
        if (!channels.empty()) return channels;
        if (!others.empty()) return {others.front()};
        return {};
    };

    selection.read_events = select_best_counters(read_candidates);
    selection.write_events = select_best_counters(write_candidates);

    if (selection.read_events.empty() || selection.write_events.empty()) {
        selection.combined_events = select_best_counters(combined_candidates);
    }

    selection.has_read_write = !selection.read_events.empty() && !selection.write_events.empty();
    selection.has_combined_counter = !selection.combined_events.empty();
    selection.requires_channel_aggregation = (selection.read_events.size() > 1) || (selection.write_events.size() > 1);

    if (!selection.has_read_write && !selection.has_combined_counter) {
        selection.failure_reason = "No usable VTune CAS counters detected";
    }

    return selection;
}

CasCounterSelection BandwidthCounterStrategy::discoverFromVtune(
    const std::vector<std::string>& preferred_read,
    const std::vector<std::string>& preferred_write,
    const std::vector<std::string>& preferred_combined
) {
    auto result = discoverFromVtune();
    std::vector<std::string> available = extractCasEventsFromVtune();

    auto select_matching = [&](const std::vector<std::string>& preferred) {
        std::vector<std::string> found;
        for (const auto& event : preferred) {
            for (const auto& available_event : available) {
                if (string_utils::contains_ci(available_event, event)) {
                    found.push_back(available_event);
                }
            }
        }
        return found;
    };

    if (!preferred_read.empty() || !preferred_write.empty() || !preferred_combined.empty()) {
        std::vector<std::string> found_read = select_matching(preferred_read);
        std::vector<std::string> found_write = select_matching(preferred_write);
        std::vector<std::string> found_combined = select_matching(preferred_combined);

        if (!found_read.empty() && !found_write.empty()) {
            result.read_events = found_read;
            result.write_events = found_write;
            result.combined_events.clear();
            result.has_read_write = true;
            result.has_combined_counter = false;
            result.requires_channel_aggregation = (found_read.size() > 1) || (found_write.size() > 1);
            result.failure_reason.clear();
        }

        if (!found_combined.empty()) {
            result.combined_events = found_combined;
            result.has_combined_counter = true;
            if (result.read_events.empty() || result.write_events.empty()) {
                result.has_read_write = false;
            }
            result.failure_reason.clear();
        }
    }

    return result;
}

std::string BandwidthCounterStrategy::lookupEventEncoding(const std::string& event_name) {
    std::string cmd = "perf list -j 2>&1 | grep -A10 '\"EventName\": \"" + event_name + "\"'";
    subprocess::Options shell_opts;
    shell_opts.timeout          = std::chrono::seconds(10);
    shell_opts.max_output_bytes = 65536;
    std::string output = subprocess::run_shell(cmd, shell_opts);
    
    size_t enc_pos = output.find("\"Encoding\"");
    if (enc_pos == std::string::npos) {
        return "";
    }
    
    size_t colon = output.find(':', enc_pos);
    if (colon == std::string::npos) return "";
    
    size_t quote1 = output.find('"', colon);
    size_t quote2 = output.find('"', quote1 + 1);
    if (quote1 == std::string::npos || quote2 == std::string::npos) return "";
    
    return output.substr(quote1 + 1, quote2 - quote1 - 1);
}

std::vector<std::string> BandwidthCounterStrategy::extractCasEventsFromPerf() {
    static std::vector<std::string> cache;
    static bool cached = false;
    
    if (cached) return cache;

    std::vector<std::string> perf_list_events;

    // Pass globs to `perf list` directly so it only emits the event
    // families we care about. On large systems (e.g. Granite Rapids with
    // many uncore PMUs) the full output can exceed the capture buffer,
    // which previously truncated `unc_m_cas_count.*` aliases away and
    // forced a fallback to per-instance `uncore_imc_N/cas_count_*/`
    // sysfs events.
    subprocess::Options perf_list_opts;
    perf_list_opts.timeout          = std::chrono::seconds(10);
    perf_list_opts.max_output_bytes = 262144;
    std::string output = subprocess::run_shell(
        "perf list '*cas_count*' 'nvidia_scf_pmu*' "
        "'dram_channel_data_controller*' 2>/dev/null",
        perf_list_opts);
    if (!output.empty()) {
        std::istringstream output_stream(output);
        std::string line;
        while (std::getline(output_stream, line)) {
            std::istringstream iss(line);
            std::string token;
            iss >> token;

            if (token.empty()) continue;

            if (token.find("unc_m_cas_count") != std::string::npos) {
                perf_list_events.push_back(token);
            } else if (token.find("cas_count") != std::string::npos) {
                perf_list_events.push_back(token);
            } else if (token.find("nvidia_scf_pmu") != std::string::npos) {
                perf_list_events.push_back(token);
            } else if (token.find("dram_channel_data_controller") != std::string::npos) {
                perf_list_events.push_back(token);
            }
        }
    }

    std::vector<std::string> unc_m_sch_events;
    std::vector<std::string> unc_m_other_events;
    std::vector<std::string> other_perf_events;
    for (const auto& evt : perf_list_events) {
        if (evt.find("unc_m_cas_count_sch") != std::string::npos) {
            unc_m_sch_events.push_back(evt);
        } else if (evt.find("unc_m_cas_count") != std::string::npos) {
            unc_m_other_events.push_back(evt);
        } else {
            other_perf_events.push_back(evt);
        }
    }
    
    if (!unc_m_sch_events.empty()) {
        cache = unc_m_sch_events;
        cached = true;
        return unc_m_sch_events;
    }
    
    if (!unc_m_other_events.empty()) {
        cache = unc_m_other_events;
        cached = true;
        return unc_m_other_events;
    }

    auto cas_filter = [](const std::string& name) {
        return name.find("cas_count") != std::string::npos &&
               name.find(".scale") == std::string::npos &&
               name.find(".unit") == std::string::npos;
    };

    std::vector<std::string> sysfs_events;
    std::set<std::string> added_events;

    DIR* pmu_dir = opendir("/sys/bus/event_source/devices");
    if (pmu_dir) {
        struct dirent* pmu_entry;
        while ((pmu_entry = readdir(pmu_dir)) != nullptr) {
            std::string pmu_name = pmu_entry->d_name;
            if (pmu_name.find("nvidia_scf_pmu") != std::string::npos) {
                std::string base_pmu_name = strip_pmu_instance_suffix(pmu_name);
                for (const auto& evt : scan_pmu_event_dir(pmu_name)) {
                    if (added_events.insert(evt).second) sysfs_events.push_back(evt);
                    size_t slash = evt.find('/');
                    std::string base_evt = base_pmu_name + evt.substr(slash);
                    if (added_events.insert(base_evt).second) sysfs_events.push_back(base_evt);
                }
            }
            if (pmu_name == "uncore_imc") {
                for (const auto& evt : scan_pmu_event_dir(pmu_name, cas_filter)) {
                    sysfs_events.push_back(evt);
                }
            }
        }
        closedir(pmu_dir);
    }
    
    if (!sysfs_events.empty()) {
        cache = sysfs_events;
        cached = true;
        return sysfs_events;
    }

    auto imc_instances = find_pmu_instances("uncore_imc");
    for (const auto& inst : imc_instances) {
        for (const auto& evt : scan_pmu_event_dir(inst, cas_filter)) {
            sysfs_events.push_back(evt);
        }
    }

    if (!sysfs_events.empty()) {
        cache = sysfs_events;
        cached = true;
        return sysfs_events;
    }
    
    if (!other_perf_events.empty()) {
        cache = other_perf_events;
        cached = true;
        return other_perf_events;
    }
    
    cache = perf_list_events;
    cached = true;
    return perf_list_events;
}
std::string UpiFlitSelection::get_rxl_events_string() const {
    return join_events(rxl_data_events);
}

std::string UpiFlitSelection::get_txl_events_string() const {
    return join_events(txl_data_events);
}

std::string UpiFlitSelection::get_all_events_string() const {
    std::vector<std::string> all;
    all.insert(all.end(), rxl_data_events.begin(), rxl_data_events.end());
    all.insert(all.end(), txl_data_events.begin(), txl_data_events.end());
    return join_events(all);
}

std::string BandwidthCounterSelection::get_bw_events_string() const {
    if (type == CounterType::CAS_COUNT ||
        type == CounterType::NVIDIA_GRACE ||
        type == CounterType::TOTAL_BYTES) {
        return cas.get_all_events_string();
    } else if (type == CounterType::UPI_FLITS) {
        return upi.get_all_events_string();
    }
    return {};
}

std::string BandwidthCounterSelection::get_all_events_string() const {
    std::string result = get_bw_events_string();
    
    for (const auto& counter : extra_counters) {
        if (!result.empty()) result += ",";
        result += counter;
    }
    
    return result;
}

std::string BandwidthCounterSelection::get_popen_events_string() const {
    std::string result;
    if (type == CounterType::CAS_COUNT ||
        type == CounterType::NVIDIA_GRACE ||
        type == CounterType::TOTAL_BYTES) {
        result = cas.get_popen_events_string();
    } else if (type == CounterType::UPI_FLITS) {
        result = upi.get_all_events_string();
    }

    for (const auto& counter : extra_counters) {
        if (!result.empty()) result += ",";
        result += counter;
    }

    return result;
}

bool BandwidthCounterSelection::is_valid() const {
    if (!perf_available) return false;
    if (type == CounterType::CAS_COUNT ||
        type == CounterType::NVIDIA_GRACE ||
        type == CounterType::TOTAL_BYTES) {
        return cas.has_read_write || cas.has_combined_counter;
    } else if (type == CounterType::UPI_FLITS) {
        return upi.has_rxl_txl;
    }
    return false;
}

std::vector<std::string> BandwidthCounterStrategy::extractUpiFlitEventsFromPerf() {
    static std::vector<std::string> cache;
    static bool cached = false;
    
    if (cached) return cache;
    
    std::vector<std::string> events;
    subprocess::Options upi_perf_list_opts;
    upi_perf_list_opts.timeout          = std::chrono::seconds(10);
    upi_perf_list_opts.max_output_bytes = 262144;
    std::string output = subprocess::run_shell("perf list 2>/dev/null", upi_perf_list_opts);
    if (!output.empty()) {
        std::istringstream output_stream(output);
        std::string line;
        while (std::getline(output_stream, line)) {
            std::string token = string_utils::trim(line);
            if (token.empty()) continue;

            std::string lowered = string_utils::to_lower(token);
            if ((lowered.find("upi") != std::string::npos && lowered.find("flits") != std::string::npos) ||
                (lowered.find("qpi") != std::string::npos && lowered.find("flits") != std::string::npos) ||
                (lowered.find("amd_df") != std::string::npos)) {
                events.push_back(token);
            }
        }
    }
    
    cache = events;
    cached = true;
    return events;
}

std::vector<std::string> BandwidthCounterStrategy::extractUpiFlitEventsFromVtune() {
    static std::vector<std::string> cache;
    static bool cached = false;

    if (cached) return cache;

    if (!run_command_success("command -v vtune >/dev/null 2>&1")) {
        cached = true;
        return cache;
    }

    const std::string output = run_vtune_capture({
        "-collect-with", "runsa",
        "-knob", "event-config=?",
        "--", "/bin/true"
    });

    std::vector<std::string> candidates = extract_vtune_tokens_from_output(output);
    for (const auto& evt : candidates) {
        std::string lowered = string_utils::to_lower(evt);
        if ((lowered.find("upi") != std::string::npos && lowered.find("flit") != std::string::npos) ||
            (lowered.find("qpi") != std::string::npos && lowered.find("flit") != std::string::npos) ||
            (lowered.find("amd_df") != std::string::npos)) {
            cache.push_back(evt);
        }
    }

    cached = true;
    return cache;
}

UpiFlitSelection BandwidthCounterStrategy::discoverUpiFlitsFromPerf() {
    UpiFlitSelection selection;
    if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }

    selection.perf_available = true;
    std::vector<std::string> detected_events = extractUpiFlitEventsFromPerf();
    if (detected_events.empty()) {
        selection.failure_reason = "perf list did not report interconnect flit/fabric counters";
        return selection;
    }

    std::vector<std::string> rxl_candidates;
    std::vector<std::string> txl_candidates;

    for (const auto& event : detected_events) {
        if (event.empty()) continue;
        std::string lowered = string_utils::to_lower(event);
        
        if (event.back() == '/') continue;

        bool is_intel_upi = lowered.find("upi") != std::string::npos || lowered.find("qpi") != std::string::npos;
        bool is_amd_df = lowered.find("amd_df") != std::string::npos;
        
        if (is_intel_upi) {
            bool is_all_data = lowered.find("all_data") != std::string::npos;
            bool is_rxl = lowered.find("rxl") != std::string::npos;
            bool is_txl = lowered.find("txl") != std::string::npos;

            if (is_all_data) {
                if (is_rxl) {
                    rxl_candidates.push_back(event);
                } else if (is_txl) {
                    txl_candidates.push_back(event);
                }
            }
        } else if (is_amd_df) {
            bool is_rx = lowered.find("rx") != std::string::npos ||
                         lowered.find("read") != std::string::npos ||
                         lowered.find("in") != std::string::npos;
            bool is_tx = lowered.find("tx") != std::string::npos || 
                         lowered.find("write") != std::string::npos ||
                         lowered.find("out") != std::string::npos;
            
            if (is_rx && !is_tx) {
                rxl_candidates.push_back(event);
            } else if (is_tx && !is_rx) {
                txl_candidates.push_back(event);
            }
        }
    }

    auto is_link_specific = [](const std::string& name) {
        static const std::regex link_regex(R"((upi|qpi|df)[0-9]+)", std::regex_constants::icase);
        if (std::regex_search(name, link_regex)) return true;
        if (string_utils::contains_ci(name, "/upi") || string_utils::contains_ci(name, "/amd_df")) return true;
        return false;
    };

    auto is_aggregate_event = [&is_link_specific](const std::string& name) -> bool {
        return string_utils::contains_ci(name, "all_data") && !is_link_specific(name);
    };

    auto select_best_counters = [&](const std::vector<std::string>& candidates) -> std::vector<std::string> {
        if (candidates.empty()) return {};

        std::vector<std::string> aggregates;
        std::vector<std::string> links;

        for (const auto& c : candidates) {
            if (is_link_specific(c)) {
                links.push_back(c);
            } else {
                aggregates.push_back(c);
            }
        }

        for (const auto& agg : aggregates) {
            if (is_aggregate_event(agg)) {
                return {agg};
            }
        }

        if (!links.empty()) {
            return links;
        }

        if (!aggregates.empty()) {
            std::sort(aggregates.begin(), aggregates.end(), [](const std::string& a, const std::string& b) {
                return a.length() < b.length();
            });
            return {aggregates[0]};
        }

        return {};
    };

    selection.rxl_data_events = select_best_counters(rxl_candidates);
    selection.txl_data_events = select_best_counters(txl_candidates);

    selection.has_rxl_txl = !selection.rxl_data_events.empty() && !selection.txl_data_events.empty();
    selection.requires_link_aggregation = (selection.rxl_data_events.size() > 1) || (selection.txl_data_events.size() > 1);

    if (!selection.has_rxl_txl) {
        selection.failure_reason = "No usable UPI flit counters detected (need both RXL and TXL all_data)";
    }

    return selection;
}

UpiFlitSelection BandwidthCounterStrategy::discoverUpiFlitsFromVtune() {
    UpiFlitSelection selection;
    if (!run_command_success("command -v vtune >/dev/null 2>&1")) {
        selection.failure_reason = "vtune binary not found";
        return selection;
    }

    selection.perf_available = true;
    std::vector<std::string> detected_events = extractUpiFlitEventsFromVtune();
    if (detected_events.empty()) {
        selection.failure_reason = "vtune event discovery did not report interconnect flit counters";
        return selection;
    }

    for (const auto& event : detected_events) {
        std::string lowered = string_utils::to_lower(event);
        if (lowered.find("rxl") != std::string::npos ||
            lowered.find("rx_data") != std::string::npos ||
            lowered.find("receive") != std::string::npos) {
            selection.rxl_data_events.push_back(event);
        } else if (lowered.find("txl") != std::string::npos ||
                   lowered.find("tx_data") != std::string::npos ||
                   lowered.find("transmit") != std::string::npos) {
            selection.txl_data_events.push_back(event);
        }
    }

    selection.has_rxl_txl = !selection.rxl_data_events.empty() && !selection.txl_data_events.empty();
    if (!selection.has_rxl_txl) {
        selection.failure_reason = "No usable VTune UPI counters detected";
    }

    return selection;
}

CounterType BandwidthCounterStrategy::detectCounterType(int src_cpu, int target_mem_node) {
    int src_socket = cpu_topology::physical_package_id(src_cpu);
    int dst_socket = cpu_topology::socket_of_node(target_mem_node);

    if (src_socket != -1 && dst_socket != -1) {
        if (src_socket == dst_socket) {
            return CounterType::CAS_COUNT;
        } else {
            return CounterType::UPI_FLITS;
        }
    }

    int src_node = cpu_topology::numa_node_of(src_cpu);
    if (src_node != -1) {
        int distance = cpu_topology::distance(src_node, target_mem_node);
        if (distance > 15) {
            return CounterType::UPI_FLITS;
        } else if (distance != -1) {
            return CounterType::CAS_COUNT;
        }
    }

    return CounterType::UNKNOWN;
}

/**
 * @brief Top-level entry point to detect and configure the appropriate hardware bandwidth counters.
 * 
 * This static method is responsible for instantiating the right set of performance events 
 * (IMC/CAS or UPI flits) based on the execution topology. 
 * 
 * 1. It checks if the traffic generator (`src_cpu`) and the target memory (`target_mem_nodes`)
 *    are on different sockets. If so, it prioritizes discovering UPI (interconnect) counters
 *    to measure cross-socket traffic.
 * 2. If it's a local access (or UPI counters fail/are unavailable), it falls back to 
 *    discovering standard Integrated Memory Controller (CAS) counters via `detectCasCounters()`.
 * 3. It also coordinates between different profiling backends (e.g., `perf`, `vtune`, `likwid`)
 *    depending on what is available and requested by the user.
 * 
 * @param strategy The initialized architecture-specific strategy instance.
 * @param src_cpu The CPU core executing the benchmark traffic.
 * @param target_mem_nodes A list of NUMA nodes where the memory allocations reside.
 * @param measurer_type The requested profiling backend (AUTO, PERF, VTUNE, etc.).
 * @return BandwidthCounterSelection The fully resolved set of counters to monitor.
 */
BandwidthCounterSelection BandwidthCounterStrategy::discoverBandwidthCounters(BandwidthCounterStrategy* strategy, int src_cpu, const std::vector<int>& target_mem_nodes, MeasurerType measurer_type) {
    BandwidthCounterSelection selection;
    const bool use_vtune = (measurer_type == MeasurerType::VTUNE);

    if (strategy) {
        strategy->set_measurer_type(measurer_type);
    }

    bool has_remote_target = false;
    bool has_local_target = false;
    for (int node : target_mem_nodes) {
        const CounterType target_type = detectCounterType(src_cpu, node);
        if (target_type == CounterType::UPI_FLITS) {
            has_remote_target = true;
        } else if (target_type == CounterType::CAS_COUNT) {
            has_local_target = true;
        }
    }
    bool use_upi = has_remote_target && !has_local_target;

    if (strategy && strategy->is_hbm()) {
        use_upi = false;
    }

    selection.cross_socket = use_upi;
    if (use_upi) {
        selection.type = CounterType::UPI_FLITS;
    } else {
        selection.type = CounterType::CAS_COUNT;
    }

    if (measurer_type == MeasurerType::PCM) {
        selection.type = CounterType::CAS_COUNT;
        selection.perf_available = true;
        selection.cas.perf_available = true;
        selection.cas.has_read_write = true;
        selection.cas.read_events = {"pcm_cxl/read_mbps"};
        selection.cas.write_events = {"pcm_cxl/write_mbps"};
        return selection;
    }

    if (measurer_type == MeasurerType::LIKWID) {
        return selection;
    }

    if (use_vtune) {
        if (!run_command_success("command -v vtune >/dev/null 2>&1")) {
            selection.failure_reason = "vtune binary not found";
            return selection;
        }
    } else if (!run_command_success("which perf > /dev/null 2>&1")) {
        selection.failure_reason = "perf binary not found";
        return selection;
    }
    selection.perf_available = true;

    CasCounterSelection cas_counters;
    
    if (!use_upi && !use_vtune && strategy && strategy->is_hbm()) {
        cas_counters = discoverIntelHbmFromPerf();
    }

    if (!cas_counters.has_read_write && !cas_counters.has_combined_counter && strategy) {
        cas_counters = strategy->detectCasCounters();
    } else if (!cas_counters.has_read_write && !cas_counters.has_combined_counter) {
        cas_counters = use_vtune ? discoverFromVtune() : discoverFromPerf();
    }
    
    bool cas_available = cas_counters.has_read_write || cas_counters.has_combined_counter;
    
    if (use_upi) {
        UpiFlitSelection upi_counters = use_vtune ? discoverUpiFlitsFromVtune() : discoverUpiFlitsFromPerf();
        bool upi_available = upi_counters.has_rxl_txl;
        
        if (upi_available) {
            selection.upi = upi_counters;
            return selection;
        }
        
        int target_mem_node = target_mem_nodes.empty() ? 0 : target_mem_nodes[0];
        std::string target_pmu = "nvidia_scf_pmu_" + std::to_string(target_mem_node);
        
        std::string target_pmu_path = "/sys/bus/event_source/devices/" + target_pmu;
        if (access((target_pmu_path + "/type").c_str(), F_OK) == 0) {
            CasCounterSelection grace_remote;
            grace_remote.perf_available = true;
            grace_remote.read_events = {target_pmu + "/cmem_rd_data/"};
            grace_remote.write_events = {target_pmu + "/cmem_wr_total_bytes/"};
            grace_remote.has_read_write = true;
            
            selection.type = CounterType::NVIDIA_GRACE;
            selection.cas = grace_remote;
            return selection;
        }
        
        selection.failure_reason = 
            "Cross-socket traffic detected but no UPI flit or Grace remote_socket counters found.\n"
            "    - The counters needed for your selected binding are not available via the selected measurer.\n"
            "    - Try adding --measurer=perf or --measurer=likwid.\n"
            "    - If nothing works, please contact the maintainers to add support for this machine.";
        selection.upi = upi_counters;
        return selection;
    }

    selection.type = CounterType::CAS_COUNT;
    selection.cas = cas_counters;
    append_unique_events(selection.extra_counters, cas_counters.read_subtract_events);
    
    if (cas_available) {
        bool is_nvidia = false;
        if (!cas_counters.read_events.empty() && cas_counters.read_events[0].find("nvidia_scf_pmu") != std::string::npos) is_nvidia = true;
        if (!cas_counters.write_events.empty() && cas_counters.write_events[0].find("nvidia_scf_pmu") != std::string::npos) is_nvidia = true;
        
        if (is_nvidia) {
            selection.type = CounterType::NVIDIA_GRACE;
        }

    }

    if (!cas_available) {
        selection.failure_reason = cas_counters.failure_reason;
    }

    return selection;
}
