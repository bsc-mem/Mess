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

#include "platform/SysInfoBackend.h"

#include "WinSysInfo.h"

#include <cpuid.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace platform {

namespace {

std::string trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string squeeze_spaces(const std::string &s) {
  std::string out;
  bool prev_space = false;
  for (char c : s) {
    const bool sp = (c == ' ' || c == '\t');
    if (sp && prev_space)
      continue;
    out.push_back(sp ? ' ' : c);
    prev_space = sp;
  }
  return trim(out);
}

/** Mirrors strip_cpu_frequency() in src/SystemInfo.cpp: drop a "@ 3.60GHz". */
void strip_cpu_frequency(std::string &model) {
  const auto at = model.find(" @ ");
  if (at != std::string::npos)
    model.erase(at);
  model = trim(model);
}



std::string cpu_brand_string() {
  unsigned a, b, c, d;
  if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u)
    return {};
  char brand[49] = {};
  unsigned off = 0;
  for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf, off += 16) {
    if (!__get_cpuid(leaf, &a, &b, &c, &d))
      break;
    std::memcpy(brand + off + 0, &a, 4);
    std::memcpy(brand + off + 4, &b, 4);
    std::memcpy(brand + off + 8, &c, 4);
    std::memcpy(brand + off + 12, &d, 4);
  }
  return squeeze_spaces(std::string(brand));
}

std::string cpu_vendor_string() {
  unsigned a, b, c, d;
  if (!__get_cpuid(0, &a, &b, &c, &d))
    return {};
  char vendor[13] = {};
  std::memcpy(vendor + 0, &b, 4);
  std::memcpy(vendor + 4, &d, 4);
  std::memcpy(vendor + 8, &c, 4);
  return std::string(vendor);
}

void fill_os_version(system_info *out) {
  std::snprintf(out->os_name, sizeof(out->os_name), "Windows");

  // GetVersionEx reports 6.2 without a compatibility manifest; RtlGetVersion does not.
  RTL_OSVERSIONINFOW rovi{};
  rovi.dwOSVersionInfoSize = sizeof(rovi);
  using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    // Through void*: a direct cast trips -Wcast-function-type on GCC.
    if (auto *raw =
            reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      reinterpret_cast<RtlGetVersionFn>(raw)(&rovi);
    }
  }
  std::snprintf(out->os_release, sizeof(out->os_release), "%lu.%lu.%lu",
                rovi.dwMajorVersion, rovi.dwMinorVersion, rovi.dwBuildNumber);

  // Windows 11 kept major version 10; the build number is the discriminator.
  const char *marketing = (rovi.dwBuildNumber >= 22000) ? "11" : "10";
  std::snprintf(out->os_version, sizeof(out->os_version),
                "Windows %s (build %lu)", marketing, rovi.dwBuildNumber);
}

struct SmbiosSummary {
  std::string technology;
  int speed_mts = 0;
  int dimm_count = 0;
  int channels = 0;
};

SmbiosSummary summarize_smbios() {
  SmbiosSummary s;
  const auto dimms = win::read_smbios_memory_devices();
  if (dimms.empty())
    return s;

  for (const auto &d : dimms) {
    int mts = d.configured_mts ? d.configured_mts : d.speed_mts;

    // Some firmware writes the memory clock where SMBIOS specifies a data rate. A rated speed
    // twice the configured one is that mix-up, not a downclocked module.
    const int twice = 2 * d.configured_mts;
    if (d.configured_mts > 0 && d.speed_mts >= twice - 32 &&
        d.speed_mts <= twice + 32) {
      mts = d.speed_mts;
    }

    if (mts > s.speed_mts)
      s.speed_mts = mts;
    if (s.technology.empty())
      s.technology = win::smbios_memory_type_name(d.memory_type);
  }

  std::set<std::string> channels;
  for (const auto &d : dimms) {
    channels.insert(win::channel_key(d.device_locator, d.bank_locator));
  }
  const int channel_keys = static_cast<int>(channels.size());

  int width_bits = 0;
  bool width_consistent = true;
  for (const auto &d : dimms) {
    const uint16_t w = d.data_width_bits;
    if (w != 8 && w != 16 && w != 32 && w != 64)
      continue;  // 0 and 0xFFFF are SMBIOS's "unknown"
    if (width_bits == 0)
      width_bits = w;
    else if (width_bits != w)
      width_consistent = false;
  }

  s.dimm_count = static_cast<int>(dimms.size());
  s.channels = (width_consistent && width_bits > 0 && width_bits < 64)
                   ? (std::max)(1, channel_keys * width_bits / 64)
                   : channel_keys;
  return s;
}

void fill_numa_nodes(si_numa_node *nodes, int *count, int max_nodes,
                     const win::WinTopology &topo, long long total_mem_bytes,
                     const std::string &technology) {
  ULONG highest = 0;
  GetNumaHighestNodeNumber(&highest);
  const int n = (std::min)(static_cast<int>(highest) + 1, max_nodes);
  *count = n;

  for (int i = 0; i < n; ++i) {
    si_numa_node &node = nodes[i];
    node = si_numa_node{};
    node.id = i;
    node.mem_total_bytes = total_mem_bytes / (std::max)(n, 1);
    node.has_cpus = 0;
    for (const auto &c : topo.cpus) {
      if (c.numa_node == i) {
        node.has_cpus = 1;
        break;
      }
    }
    node.is_dax_backed = 0; // no Windows equivalent of daxctl or HMAT
    node.read_bandwidth_mb_s = 0;
    node.read_latency_ns = 0;
    std::snprintf(node.memory_type, sizeof(node.memory_type), "%s",
                  technology.empty() ? "UNKNOWN" : technology.c_str());
  }
}

}

int detect_system_info(system_info *out) {
  if (!out)
    return -1;

  const win::WinTopology t = win::detect_topology();

  out->total_logical_cores = static_cast<int>(t.cpus.size());
  out->total_physical_cores = t.physical_cores;
  out->socket_count = (std::min)(t.packages, SI_MAX_SOCKETS);

  out->performance_cores = t.hybrid.performance_cores;
  out->efficiency_cores = t.hybrid.efficiency_cores;

  const std::string brand = cpu_brand_string();
  std::string model = brand;
  strip_cpu_frequency(model);
  std::snprintf(out->cpu_model, sizeof(out->cpu_model), "%s", model.c_str());
  std::snprintf(out->cpu_vendor, sizeof(out->cpu_vendor), "%s",
                cpu_vendor_string().c_str());

  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);
  out->total_mem_bytes = static_cast<long long>(ms.ullTotalPhys);

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si); // Native, not GetSystemInfo, which lies under WOW64
  out->page_size = static_cast<long>(si.dwPageSize);
  std::snprintf(
      out->arch, sizeof(out->arch), "%s",
      si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? "x86_64"
      : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                  : "unknown");

  fill_os_version(out);

  for (int i = 0; i < out->socket_count; ++i) {
    si_socket &s = out->sockets[i];
    s.id = i;

    std::set<int> cores;
    int threads = 0;
    for (const auto &c : t.cpus) {
      if (c.package != i)
        continue;
      ++threads;
      cores.insert(c.core_id);
    }
    s.core_count = static_cast<int>(cores.size());
    s.thread_count = threads;
    s.mem_total_bytes = out->total_mem_bytes / (std::max)(out->socket_count, 1);
    s.mem_channels = 2;

    // SI_MAX_CACHE_LEVELS is 4, so an L4 must not evict L3 in discovery order.
    auto pit = t.caches.find(i);
    if (pit != t.caches.end()) {
      for (int slot : {0, 1, 2, 3}) {
        auto cit = pit->second.find(slot);
        if (cit == pit->second.end())
          continue;
        if (s.cache_count >= SI_MAX_CACHE_LEVELS)
          break;
        s.caches[s.cache_count++] = cit->second;
      }
    }
  }

  const SmbiosSummary smbios = summarize_smbios();
  if (!smbios.technology.empty()) {
    std::snprintf(out->mem_technology, sizeof(out->mem_technology), "%s",
                  smbios.technology.c_str());
  }
  if (smbios.speed_mts > 0) {
    std::snprintf(out->mem_frequency, sizeof(out->mem_frequency), "%d MT/s",
                  smbios.speed_mts);
  }
  if (smbios.dimm_count > 0) {
    const int sockets = (std::max)(out->socket_count, 1);
    for (int i = 0; i < out->socket_count; ++i) {
      out->sockets[i].mem_dimm_count = smbios.dimm_count / sockets;
      out->sockets[i].mem_speed_mts = smbios.speed_mts;
      if (smbios.channels > 0) {
        out->sockets[i].mem_channels = (std::max)(smbios.channels / sockets, 1);
      }
    }
  }

  // Left empty so system_info_detect()'s server-SKU inference still runs.

  fill_numa_nodes(out->numa_nodes, &out->numa_node_count, SI_MAX_NUMA_NODES, t,
                  out->total_mem_bytes, out->mem_technology);

  return 0;
}

int detect_numa_nodes(si_numa_node *out, int max_nodes) {
  if (!out || max_nodes <= 0)
    return 0;

  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);

  int count = 0;
  fill_numa_nodes(out, &count, max_nodes, win::detect_topology(),
                  static_cast<long long>(ms.ullTotalPhys),
                  summarize_smbios().technology);
  return count;
}

}
