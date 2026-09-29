/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
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

#include "WinSysInfo.h"
#include "WinTopologyRecords.h"

#include <algorithm>

namespace platform {
namespace win {

namespace {

// Enum constants cannot be detected with #ifndef. Value 6 supports older SDKs.
constexpr auto kRelationNumaNodeEx = static_cast<LOGICAL_PROCESSOR_RELATIONSHIP>(6);

std::vector<BYTE> query_lpi(LOGICAL_PROCESSOR_RELATIONSHIP rel) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(rel, nullptr, &len);
    if (len == 0) return {};
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(
            rel, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
        return {};
    }
    buf.resize(len);
    return buf;
}

// Records are variable-length: advance by e->Size. The loop guard must use the fixed header
// only.
constexpr size_t kLpiHeaderSize = sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD);

template <class F>
void for_each_lpi(std::vector<BYTE>& buf, F&& f) {
    BYTE* p = buf.data();
    BYTE* const end = p + buf.size();
    while (p + kLpiHeaderSize <= end) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
        if (e->Size < kLpiHeaderSize || p + e->Size > end) break;
        f(*e);
        p += e->Size;
    }
}

inline int flat_id(WORD group, int bit) { return group * 64 + bit; }

template <class F>
void for_each_bit(const GROUP_AFFINITY& gm, F&& f) {
    for (int bit = 0; bit < 64; ++bit) {
        if (gm.Mask & (static_cast<KAFFINITY>(1) << bit)) f(gm.Group, bit);
    }
}

}

WinTopology detect_topology() {
    WinTopology t;
    t.multi_group = GetActiveProcessorGroupCount() > 1;

    auto core_buf = query_lpi(RelationProcessorCore);
    int core_index = 0;
    BYTE max_efficiency = 0;
    for_each_lpi(core_buf, [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& e) {
        const auto& pr = e.Processor;
        const bool smt = (pr.Flags & LTP_PC_SMT) != 0;
        bool first_in_core = true;
        for (WORD g = 0; g < pr.GroupCount; ++g) {
            for_each_bit(pr.GroupMask[g], [&](WORD grp, int bit) {
                LogicalCpu c;
                c.id = flat_id(grp, bit);
                c.group = grp;
                c.bit = bit;
                c.core_id = core_index;
                c.efficiency = pr.EfficiencyClass;
                c.is_smt_sibling = smt && !first_in_core;
                t.by_flat_id[c.id] = t.cpus.size();
                t.cpus.push_back(c);
                first_in_core = false;
            });
        }
        max_efficiency = (std::max)(max_efficiency, pr.EfficiencyClass);
        ++core_index;
    });
    t.physical_cores = core_index;

    // A hybrid part reports more than one EfficiencyClass; a homogeneous one reports 0
    // everywhere.
    t.hybrid.is_hybrid = max_efficiency > 0;
    for (const auto& c : t.cpus) {
        if (c.is_smt_sibling) continue;
        if (t.hybrid.is_hybrid && c.efficiency < max_efficiency) ++t.hybrid.efficiency_cores;
        else ++t.hybrid.performance_cores;
    }

    auto pkg_buf = query_lpi(RelationProcessorPackage);
    int pkg_index = 0;
    for_each_lpi(pkg_buf, [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& e) {
        for (WORD g = 0; g < e.Processor.GroupCount; ++g) {
            for_each_bit(e.Processor.GroupMask[g], [&](WORD grp, int bit) {
                auto it = t.by_flat_id.find(flat_id(grp, bit));
                if (it != t.by_flat_id.end()) t.cpus[it->second].package = pkg_index;
            });
        }
        ++pkg_index;
    });
    t.packages = (std::max)(pkg_index, 1);
    // Never leave a CPU unassigned, or its socket reports 0 cores.
    for (auto& c : t.cpus) if (c.package < 0) c.package = 0;

    // RelationNumaNode reports only the primary group's affinity; the Ex form returns all.
    auto numa_buf = query_lpi(kRelationNumaNodeEx);
    const bool numa_ex = !numa_buf.empty();
    if (!numa_ex) numa_buf = query_lpi(RelationNumaNode);
    for_each_lpi(numa_buf, [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& e) {
        const auto& nn = e.NumaNode;
        const int node = static_cast<int>(nn.NodeNumber);
        auto assign = [&](WORD grp, int bit) {
            auto it = t.by_flat_id.find(flat_id(grp, bit));
            if (it != t.by_flat_id.end()) t.cpus[it->second].numa_node = node;
        };
        for_each_numa_affinity(e, numa_ex, [&](const GROUP_AFFINITY& affinity) {
            for_each_bit(affinity, assign);
        });
        t.numa_nodes = (std::max)(t.numa_nodes, node + 1);
    });
    if (t.numa_nodes == 0) t.numa_nodes = 1;
    for (auto& c : t.cpus) if (c.numa_node < 0) c.numa_node = 0;

    // L3 summed per package: a chiplet CPU reports one record per L3 instance.
    auto cache_buf = query_lpi(RelationCache);
    for_each_lpi(cache_buf, [&](SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& e) {
        const auto& c = e.Cache;

        int pkg = 0;
        bool found = false;
        for_each_bit(c.GroupMask, [&](WORD grp, int bit) {
            if (found) return;
            auto it = t.by_flat_id.find(flat_id(grp, bit));
            if (it != t.by_flat_id.end() && t.cpus[it->second].package >= 0) {
                pkg = t.cpus[it->second].package;
                found = true;
            }
        });

        si_cache sc{};
        sc.level = c.Level;
        sc.size_bytes = static_cast<long long>(c.CacheSize);
        sc.line_size_bytes = c.LineSize;
        sc.associativity = (c.Associativity == CACHE_FULLY_ASSOCIATIVE) ? -1 : c.Associativity;
        switch (c.Type) {
            case CacheUnified:     sc.type = si_cache_type::UNIFIED;     break;
            case CacheInstruction: sc.type = si_cache_type::INSTRUCTION; break;
            case CacheData:        sc.type = si_cache_type::DATA;        break;
            default:               sc.type = si_cache_type::TRACE;       break;
        }

        // Key L1 by type so L1D and L1I do not overwrite each other.
        int slot = sc.level;
        if (sc.level == 1 && sc.type == si_cache_type::INSTRUCTION) slot = 0;

        auto& pkg_caches = t.caches[pkg];
        auto it = pkg_caches.find(slot);
        if (it == pkg_caches.end()) {
            pkg_caches[slot] = sc;
        } else if (sc.level >= 3) {
            it->second.size_bytes += sc.size_bytes;
        }
    });

    return t;
}

}
}
