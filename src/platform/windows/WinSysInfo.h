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

#ifndef PLATFORM_WINDOWS_WINSYSINFO_H
#define PLATFORM_WINDOWS_WINSYSINFO_H

#include "SystemInfo.h"

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace platform {
namespace win {

struct LogicalCpu {
    int  id = 0;
    WORD group = 0;
    int  bit = 0;
    int  core_id = -1;
    int  package = -1;
    int  numa_node = -1;
    BYTE efficiency = 0;
    bool is_smt_sibling = false;
};

struct HybridInfo {
    int  performance_cores = 0;
    int  efficiency_cores = 0;
    bool is_hybrid = false;
};

struct WinTopology {
    std::vector<LogicalCpu> cpus;
    std::map<int, size_t> by_flat_id;
    int packages = 0;
    int numa_nodes = 0;
    int physical_cores = 0;
    HybridInfo hybrid;
    std::map<int, std::map<int, si_cache>> caches;
    bool multi_group = false;
};

WinTopology detect_topology();

struct MemoryDevice {
    uint32_t    size_mb = 0;
    uint16_t    speed_mts = 0;
    uint16_t    configured_mts = 0;
    uint8_t     memory_type = 0;
    uint16_t    data_width_bits = 0;
    std::string device_locator;
    std::string bank_locator;
};

std::vector<MemoryDevice> read_smbios_memory_devices();
const char* smbios_memory_type_name(uint8_t type);
std::string channel_key(const std::string& device, const std::string& bank);

}
}

#endif // PLATFORM_WINDOWS_WINSYSINFO_H
