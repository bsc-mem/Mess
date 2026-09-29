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
#include <windows.h>
#include <cstddef>
#include <cstring>

namespace platform::win {
// Wire-compatible with old SDKs whose final Reserved bytes contain GroupCount.
// https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-numa_node_relationship
template<class F>
bool for_each_numa_affinity(const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& record,
                            bool extended, F&& visit) {
    constexpr std::size_t payload = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode);
    constexpr std::size_t masks = payload + offsetof(NUMA_NODE_RELATIONSHIP, GroupMask);
    constexpr std::size_t group_count = payload + sizeof(DWORD) + 18;
    static_assert(masks == payload + 24, "unexpected Windows NUMA layout");
    if (record.Size < masks + sizeof(GROUP_AFFINITY)) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
    WORD count = 1;
    if (extended) {
        std::memcpy(&count, bytes + group_count, sizeof(count));
        if (count == 0) count = 1; // Older Windows reports the single GroupMask.
    }
    if (count > (record.Size - masks) / sizeof(GROUP_AFFINITY)) return false;
    for (WORD i = 0; i < count; ++i) {
        GROUP_AFFINITY affinity{};
        std::memcpy(&affinity, bytes + masks + i * sizeof(affinity), sizeof(affinity));
        visit(affinity);
    }
    return true;
}
} // namespace platform::win
