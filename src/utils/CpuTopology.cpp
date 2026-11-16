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

#include "utils/CpuTopology.h"
#include "utils/FileUtils.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace cpu_topology {

std::vector<int> select_workers_on_socket(const std::map<int, CpuTopology>& topology,
                                         const std::vector<int>& allowed,
                                         int requested_count, int reserved_cpu) {
    std::vector<int> selected;
    if (requested_count <= 0 || allowed.empty()) return selected;
    auto anchor = topology.find(reserved_cpu);
    if (anchor == topology.end()) anchor = topology.find(allowed.front());
    if (anchor == topology.end()) return selected;
    const int socket = anchor->second.socket;
    std::set<std::pair<int, int>> used_cores;
    auto reserved = topology.find(reserved_cpu);
    if (reserved != topology.end()) {
        used_cores.emplace(reserved->second.socket, reserved->second.core_id);
    }
    for (int cpu : allowed) {
        if (static_cast<int>(selected.size()) >= requested_count) break;
        auto it = topology.find(cpu);
        if (cpu == reserved_cpu || it == topology.end() || it->second.socket != socket) continue;
        if (used_cores.emplace(it->second.socket, it->second.core_id).second) selected.push_back(cpu);
    }
    return selected;
}


namespace {

constexpr const char* kCpuRoot  = "/sys/devices/system/cpu";
constexpr const char* kNodeRoot = "/sys/devices/system/node";

#if defined(__linux__)
void trim(std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s.clear(); return; }
    const size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
}

int parse_first_cpu_in_list(const std::string& cpulist) {
    if (cpulist.empty()) return -1;
    const size_t pos = cpulist.find_first_of(",-");
    const std::string head = (pos != std::string::npos) ? cpulist.substr(0, pos) : cpulist;
    try {
        return std::stoi(head);
    } catch (...) {
        return -1;
    }
}

int parse_indexed_name(const std::string& name, const char* prefix) {
    const size_t prefix_len = std::strlen(prefix);
    if (name.rfind(prefix, 0) != 0 || name.size() <= prefix_len) {
        return -1;
    }
    for (size_t i = prefix_len; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return -1;
        }
    }
    try {
        return std::stoi(name.substr(prefix_len));
    } catch (...) {
        return -1;
    }
}

std::set<int> scan_nodes() {
    std::set<int> nodes;
#if defined(__linux__)
    file_utils::for_each_dir_entry(kNodeRoot, [&](const std::string& name) {
        const int node = parse_indexed_name(name, "node");
        if (node >= 0) {
            nodes.insert(node);
        }
    });
#endif
    if (nodes.empty()) nodes.insert(0);
    return nodes;
}
#endif

}

std::string cpu_path(int cpu) {
    return std::string(kCpuRoot) + "/cpu" + std::to_string(cpu);
}

std::string node_path(int node) {
    return std::string(kNodeRoot) + "/node" + std::to_string(node);
}

int numa_node_of(int cpu) {
#if defined(__linux__)
    static std::map<int, int> cache;
    static std::mutex cache_mutex;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(cpu);
        if (it != cache.end()) return it->second;
    }

    int node = -1;
    file_utils::for_each_dir_entry(cpu_path(cpu), [&](const std::string& name) {
        if (node >= 0) return;
        if (name.rfind("node", 0) != 0 || name.size() <= 4) return;
        const int parsed = parse_indexed_name(name, "node");
        if (parsed >= 0) {
            node = parsed;
        }
    });

    if (node != -1) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[cpu] = node;
    }
    return node;
#else
    (void)cpu;
    return -1;
#endif
}

int physical_package_id(int cpu) {
#if defined(__linux__)
    static std::map<int, int> cache;
    static std::mutex cache_mutex;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(cpu);
        if (it != cache.end()) return it->second;
    }

    int pkg = -1;
    file_utils::read_int_file(cpu_path(cpu) + "/topology/physical_package_id", pkg);

    if (pkg >= 0) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[cpu] = pkg;
    }
    return pkg;
#else
    (void)cpu;
    return -1;
#endif
}

int distance(int src_node, int dst_node) {
    if (src_node == dst_node) return 10;

    static std::map<std::pair<int, int>, int> cache;
    static std::mutex cache_mutex;
    const auto key = std::make_pair(src_node, dst_node);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    std::ifstream f(node_path(src_node) + "/distance");
    if (!f.is_open()) return -1;

    int result = -1;
    int val;
    int idx = 0;
    while (f >> val) {
        if (idx == dst_node) {
            result = val;
            break;
        }
        ++idx;
    }

    if (result != -1) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[key] = result;
    }
    return result;
}

int socket_of_node(int node) {
#if defined(__linux__)
    static std::map<int, int> cache;
    static std::mutex cache_mutex;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(node);
        if (it != cache.end()) return it->second;
    }

    std::string cpulist;
    if (!file_utils::read_first_line(node_path(node) + "/cpulist", cpulist)) return -1;
    trim(cpulist);

    const int first_cpu = parse_first_cpu_in_list(cpulist);
    if (first_cpu < 0) return -1;
    const int socket = physical_package_id(first_cpu);
    if (socket >= 0) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[node] = socket;
    }
    return socket;
#else
    (void)node;
    return -1;
#endif
}

int socket_of_memory_node(int node) {
    const int socket = socket_of_node(node);
    if (socket >= 0) return socket;
    int nearest_socket = -1;
    int nearest_distance = std::numeric_limits<int>::max();
    for (int cpu_node : list_nodes()) {
        const int cpu_socket = socket_of_node(cpu_node);
        const int d = distance(node, cpu_node);
        if (cpu_socket >= 0 && d >= 0 && d < nearest_distance) {
            nearest_distance = d;
            nearest_socket = cpu_socket;
        }
    }
    return nearest_socket;
}

std::set<int> list_nodes(int max_index) {
    std::set<int> nodes;
#if defined(__linux__)
    static std::set<int> cached_nodes = scan_nodes();
    for (int node : cached_nodes) {
        if (node < max_index) {
            nodes.insert(node);
        }
    }
#else
    (void)max_index;
#endif
    if (nodes.empty()) nodes.insert(0);
    return nodes;
}

std::set<int> nodes_for_cpus(const std::set<int>& cpus) {
    std::set<int> nodes;
#if defined(__linux__)
    for (int cpu : cpus) {
        const int n = numa_node_of(cpu);
        if (n >= 0) nodes.insert(n);
    }
#else
    (void)cpus;
#endif
    return nodes;
}

bool is_remote(int cpu, int mem_node) {
#if defined(__linux__)
    static std::map<std::pair<int, int>, bool> cache;
    static std::mutex cache_mutex;
    const auto key = std::make_pair(cpu, mem_node);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    const int cpu_node = numa_node_of(cpu);
    const bool remote = (cpu_node >= 0) ? (cpu_node != mem_node) : false;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[key] = remote;
    }
    return remote;
#else
    (void)cpu;
    (void)mem_node;
#endif
    return false;
}

}
