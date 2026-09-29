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

#include "platform/Sys.h"

#include "WinSysInfo.h"

#include <windows.h>
#include <cpuid.h>
#include <cstring>
#include <string>

namespace platform {

// Not GetSystemInfo().dwNumberOfProcessors, which saturates at 64.
int online_cpu_count() {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return (n > 0) ? static_cast<int>(n) : 0;
}

long page_size() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return (si.dwPageSize > 0) ? static_cast<long>(si.dwPageSize) : 0;
}

// Flat ids, group * 64 + bit, which is what PinSpec::cpu means.
std::vector<CpuPlacement> cpu_placements() {
    const win::WinTopology topo = win::detect_topology();

    std::vector<CpuPlacement> out;
    out.reserve(topo.cpus.size());
    for (const win::LogicalCpu& cpu : topo.cpus) {
        CpuPlacement p;
        p.cpu       = cpu.id;
        p.numa_node = (cpu.numa_node >= 0) ? cpu.numa_node : 0;
        p.package   = (cpu.package   >= 0) ? cpu.package   : 0;
        p.core_id   = (cpu.core_id   >= 0) ? cpu.core_id   : cpu.id;
        out.push_back(p);
    }
    return out;
}


bool large_pages_available() {
    if (GetLargePageMinimum() == 0) {
        return false;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return false;
    }

    std::vector<unsigned char> buffer(needed);
    if (!GetTokenInformation(token, TokenPrivileges, buffer.data(), needed, &needed)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    const TOKEN_PRIVILEGES* privileges =
        reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        const LUID& held = privileges->Privileges[i].Luid;
        if (held.LowPart == luid.LowPart && held.HighPart == luid.HighPart) {
            return true;
        }
    }
    return false;
}

bool cpu_is_amd() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return false;
    char vendor[13] = {};
    std::memcpy(vendor + 0, &b, 4);
    std::memcpy(vendor + 4, &d, 4);
    std::memcpy(vendor + 8, &c, 4);
    return std::string(vendor) == "AuthenticAMD";
}

}
