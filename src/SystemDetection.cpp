/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          javier.beiro     [at] bsc [dot] es
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

#include "SystemDetection.h"
#include "SystemInfo.h"
#include "utils/SubprocessCapture.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>
#include <cctype>
#include <regex>
#include <iomanip>
#include <unordered_map>
#include <memory>
#include <cstdlib>
#include <limits>
#if !defined(_WIN32)
#include <sys/utsname.h>   // uname() fallback; guarded, see detect()
#endif

#ifdef __linux__
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <stdint.h>
#elif defined(__linux__) && defined(__aarch64__)
#include <sys/prctl.h>
#include <linux/prctl.h>
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM)
#elif defined(__powerpc__) || defined(__ppc__) || defined(_M_PPC)
#endif

bool CPUCapabilities::hasOnPackageHBM() const {
    return cpu_has_onpackage_hbm(model_name);
}

namespace {
#if defined(__x86_64__) || defined(_M_X64)
    static inline uint64_t xgetbv(uint32_t xcr) {
        uint32_t eax, edx;
        __asm__ volatile ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(xcr));
        return (static_cast<uint64_t>(edx) << 32) | eax;
    }
#endif
    std::string read_file(const std::string& path) {
        std::ifstream file(path);
        if (!file) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    int detect_apple_gpu_cores() {
#ifdef __APPLE__
        FILE* pipe = popen("/usr/sbin/ioreg -r -c IOGPU -d 1 2>/dev/null", "r");
        if (!pipe) return 0;
        char line[512];
        while (std::fgets(line, sizeof(line), pipe)) {
            const char* key = std::strstr(line, "\"gpu-core-count\"");
            if (key) {
                int cores = 0;
                std::sscanf(key, "\"gpu-core-count\" = %d", &cores);
                pclose(pipe);
                return cores;
            }
        }
        pclose(pipe);
#endif
        return 0;
    }

#ifdef __linux__
    std::string normalize_device_tree_string(std::string value) {
        std::replace(value.begin(), value.end(), '\0', ' ');

        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        auto begin = std::find_if(value.begin(), value.end(), not_space);
        auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
        if (begin >= end) {
            return "";
        }

        value = std::string(begin, end);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }

    bool file_exists(const std::string& path) {
        return access(path.c_str(), F_OK) == 0;
    }
#endif

    bool probe_nvidia_scf_pmu() {
#ifdef __linux__
        for (int i = 0; i < 64; ++i) {
            const std::string pmu_path =
                "/sys/bus/event_source/devices/nvidia_scf_pmu_" + std::to_string(i);
            if (file_exists(pmu_path + "/type") || file_exists(pmu_path)) {
                return true;
            }
        }
#endif
        return false;
    }


    NvidiaPlatformInfo detect_nvidia_platform_info() {
        NvidiaPlatformInfo info;

#ifdef __linux__
        info.model = normalize_device_tree_string(read_file("/proc/device-tree/model"));
        if (info.model.empty()) {
            info.model = normalize_device_tree_string(read_file("/sys/firmware/devicetree/base/model"));
        }

        info.compatible = normalize_device_tree_string(read_file("/proc/device-tree/compatible"));
        if (info.compatible.empty()) {
            info.compatible = normalize_device_tree_string(read_file("/sys/firmware/devicetree/base/compatible"));
        }

#endif

        info.has_nvidia_scf_pmu = probe_nvidia_scf_pmu();
        return info;
    }

    bool looks_like_grace_core(const std::string& model) {
        std::string upper = model;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper.find("NEOVERSE-V2") != std::string::npos ||
               upper.find("NEOVERSE V2") != std::string::npos ||
               upper.find("GRACE") != std::string::npos;
    }


    void apply_nvidia_platform_identity(system_info& sys_info,
                                        const NvidiaPlatformInfo& platform) {

        if (platform.has_nvidia_scf_pmu && looks_like_grace_core(sys_info.cpu_model)) {
            std::snprintf(sys_info.cpu_vendor, sizeof(sys_info.cpu_vendor), "NVIDIA");
            std::snprintf(sys_info.cpu_model, sizeof(sys_info.cpu_model), "Grace (Neoverse-V2)");
        }
    }

    bool parse_first_uint64(const std::string& text, uint64_t& out) {
        size_t i = 0;
        while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) {
            return false;
        }
        uint64_t value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            uint64_t digit = static_cast<uint64_t>(text[i] - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            ++i;
        }
        out = value;
        return true;
    }

    size_t parse_size_with_unit(const std::string& str) {
        if (str.empty()) return 0;
        
        size_t multiplier = 1;
        size_t value = 0;
        
        try {
            size_t pos = 0;
            while (pos < str.length() && std::isdigit(str[pos])) {
                value = value * 10 + (str[pos] - '0');
                pos++;
            }
            
            if (pos < str.length()) {
                switch (std::toupper(str[pos])) {
                    case 'K': multiplier = 1000; break;
                    case 'M': multiplier = 1000 * 1000; break;
                    case 'G': multiplier = 1000 * 1000 * 1000; break;
                    default: break;
                }
            }
            
            return value * multiplier;
        } catch (...) {
            return 0;
        }
    }

    int count_cpu_list_entries(const std::string& list) {
        int count = 0;
        std::stringstream stream(list);
        std::string range;

        while (std::getline(stream, range, ',')) {
            size_t dash = range.find('-');
            try {
                if (dash == std::string::npos) {
                    if (range.find_first_of("0123456789") != std::string::npos) ++count;
                } else {
                    int first = std::stoi(range.substr(0, dash));
                    int last = std::stoi(range.substr(dash + 1));
                    if (last >= first) count += last - first + 1;
                }
            } catch (...) {
                continue;
            }
        }

        return count;
    }

    /**
     * @brief Parses the sysfs interface to determine the CPU cache size for a given level.
     *
     * Iterates through `/sys/devices/system/cpu/cpu0/cache/index*` to find the cache
     * matching the requested level (e.g., 1 for L1, 2 for L2). When found, it reads
     * the `size` file, converting strings (e.g., "32K", "1M") into exact byte values.
     *
     * For the LLC the reported value is scaled up to the whole socket. Chiplet CPUs
     * (AMD Zen CCDs) expose one L3 instance per chiplet, and sysfs reports the size of
     * that single slice, so a 12-CCD EPYC otherwise looks like it has 32 MB of L3
     * instead of 384 MB. The slice count comes from the CPUs in the socket divided by
     * the CPUs sharing this cache. Lower levels are per-core/per-cluster by definition
     * and are returned untouched.
     *
     * @param level The cache level to detect (1, 2, or 3).
     * @return size_t The size of the cache in bytes, or 0 if not found.
     */
    size_t detect_cache_size_sysfs(int level) {
        // First try matching by actual cache level (not just index)
        for (int idx = 0; idx < 16; ++idx) {
            std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx);
            std::string level_content = read_file(base + "/level");
            if (level_content.empty()) break; // no more cache indices

            // Trim whitespace
            while (!level_content.empty() && (level_content.back() == '\n' || level_content.back() == ' '))
                level_content.pop_back();

            int cache_level = 0;
            try { cache_level = std::stoi(level_content); } catch (...) { continue; }

            if (cache_level == level) {
                std::string size_content = read_file(base + "/size");
                if (!size_content.empty()) {
                    size_t size = parse_size_with_unit(size_content);
                    if (size > 0) {
                        if (level < 3) return size;

                        std::string package_cpus = read_file("/sys/devices/system/cpu/cpu0/topology/package_cpus_list");
                        if (package_cpus.empty()) {
                            package_cpus = read_file("/sys/devices/system/cpu/cpu0/topology/core_siblings_list");
                        }

                        const int cpus_per_slice = count_cpu_list_entries(read_file(base + "/shared_cpu_list"));
                        const int cpus_per_socket = count_cpu_list_entries(package_cpus);
                        if (cpus_per_slice > 0 && cpus_per_socket > cpus_per_slice) {
                            size *= static_cast<size_t>(cpus_per_socket / cpus_per_slice);
                        }
                        return size;
                    }
                }
            }
        }
        return 0;
    }


#ifndef __APPLE__
#if defined(__x86_64__) || defined(_M_X64)
    size_t detect_cache_size_x86(int level) {
        unsigned int eax, ebx, ecx, edx;
        
        if (level == 3) {
            __cpuid(0x80000006, eax, ebx, ecx, edx);
            if ((ecx >> 18) > 0) {
                size_t size = ((ecx >> 18) + 1) * 512 * 1024;
                return size;
            }
        }
        
        return detect_cache_size_sysfs(level);
    }
#endif
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
    size_t detect_cache_size_macos(int level) {
        if (level == 1) {
            uint64_t size = 0;
            size_t len = sizeof(size);
            if (sysctlbyname("hw.l1dcachesize", &size, &len, nullptr, 0) == 0) {
                return static_cast<size_t>(size);
            }
            return 0;
        }

        if (level == 2) {
            uint64_t size = 0;
            size_t len = sizeof(size);
            if (sysctlbyname("hw.perflevel0.l2cachesize", &size, &len, nullptr, 0) == 0 && size > 0) {
                return static_cast<size_t>(size);
            }
            if (sysctlbyname("hw.l2cachesize", &size, &len, nullptr, 0) == 0) {
                return static_cast<size_t>(size);
            }
            return 0;
        }

        if (level == 3) {
            uint64_t size = 0;
            size_t len = sizeof(size);
            if (sysctlbyname("hw.l3cachesize", &size, &len, nullptr, 0) == 0 && size > 0) {
                return static_cast<size_t>(size);
            }

            char brand[256] = {};
            size_t blen = sizeof(brand);
            if (sysctlbyname("machdep.cpu.brand_string", brand, &blen, nullptr, 0) == 0) {
                std::string s(brand);
                bool is_ultra  = s.find("Ultra")  != std::string::npos;
                bool is_max    = s.find("Max")    != std::string::npos;
                bool is_pro    = s.find("Pro")    != std::string::npos;
                bool is_m1     = s.find("M1")     != std::string::npos;
                bool is_m2     = s.find("M2")     != std::string::npos;
                bool is_m3     = s.find("M3")     != std::string::npos;
                bool is_m4     = s.find("M4")     != std::string::npos;
                bool is_m5     = s.find("M5")     != std::string::npos;
                bool is_m6     = s.find("M6")     != std::string::npos;

                if (is_ultra) {
                    return 96ULL * 1000 * 1000;
                }
                if (is_max) {
                    return 48ULL * 1000 * 1000;
                }
                if (is_pro) {
                    if (is_m3) return 24ULL * 1000 * 1000;
                    return 32ULL * 1000 * 1000;
                }
                if (is_m1 || is_m2 || is_m3 || is_m4 || is_m5 || is_m6) {
                    return 16ULL * 1000 * 1000;
                }
            }
            return 0;
        }

        return 0;
    }
#endif

#if defined(__linux__) && defined(__aarch64__) && defined(PR_SVE_GET_VL) && defined(PR_SVE_VL_LEN_MASK)
    int detect_current_sve_bits_via_prctl() {
        long vl = prctl(PR_SVE_GET_VL);
        if (vl < 0) {
            return 0;
        }
        return static_cast<int>((vl & PR_SVE_VL_LEN_MASK) * 8);
    }
#endif
}

const NvidiaPlatformInfo& get_nvidia_platform_info() {
    static const NvidiaPlatformInfo info = detect_nvidia_platform_info();
    return info;
}

bool has_nvidia_scf_pmu() {
    return get_nvidia_platform_info().has_nvidia_scf_pmu;
}


size_t detect_cache_size(int level) {
    size_t size = detect_cache_size_sysfs(level);
    if (size > 0) return size;
    
#if defined(__APPLE__)
    return detect_cache_size_macos(level);
#elif defined(__x86_64__) || defined(_M_X64)
    return detect_cache_size_x86(level);
#else
    return 0;
#endif
}

int detect_cache_line_size_fallback() {
#if defined(__APPLE__)
    uint64_t line_size = 0;
    size_t len = sizeof(line_size);
    if (sysctlbyname("hw.cachelinesize", &line_size, &len, nullptr, 0) == 0 && line_size > 0) {
        return static_cast<int>(line_size);
    }
#else
    uint64_t line_size = 0;
    if (parse_first_uint64(read_file("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size"), line_size) &&
        line_size > 0) {
        return static_cast<int>(line_size);
    }
#endif
    return 64;
}

SystemDetector::SystemDetector() {
    memset(&sys_info_, 0, sizeof(sys_info_));
    capabilities_ = {};
    nvidia_platform_ = {};
}

/**
 * @brief Runs the hardware discovery process.
 * 
 * This method runs a series of detection passes to populate `sys_info_` and `capabilities_`.
 * It detects the CPU architecture, vendor, ISA extensions (e.g., AVX, SVE, RVV), and specific
 * CPU models (e.g., A64FX, Grace). 
 * 
 * After gathering CPU metadata, it runs `detect_caches()` to map out the memory hierarchy,
 * which informs cache-line sizes, array dimensions, and stride offsets
 * used during the benchmark generation.
 * 
 * @return bool True if detection was broadly successful, false on failure.
 */
bool SystemDetector::detect() {
    if (system_info_detect(&sys_info_) != 0) {
        return false;
    }
    nvidia_platform_ = ::get_nvidia_platform_info();
    apply_nvidia_platform_identity(sys_info_, nvidia_platform_);

    std::string vendor = sys_info_.cpu_vendor;
    std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
    if (vendor.find("intel") != std::string::npos) {
        capabilities_.vendor = CPUVendor::INTEL;
    } else if (vendor.find("amd") != std::string::npos) {
        capabilities_.vendor = CPUVendor::AMD;
    } else if (vendor.find("apple") != std::string::npos) {
        capabilities_.vendor = CPUVendor::APPLE;
    } else if (vendor.find("arm") != std::string::npos
            || vendor.find("fujitsu") != std::string::npos
            || vendor.find("ampere") != std::string::npos
            || vendor.find("nvidia") != std::string::npos) {
        capabilities_.vendor = CPUVendor::ARM;
    } else if (vendor.find("ibm") != std::string::npos) {
        capabilities_.vendor = CPUVendor::IBM;
    } else if (vendor.find("sifive") != std::string::npos) {
        capabilities_.vendor = CPUVendor::SIFIVE;
    } else {
        capabilities_.vendor = CPUVendor::UNKNOWN;
    }

    {
        std::string cpu_model = sys_info_.cpu_model;
        std::string cpu_vendor = sys_info_.cpu_vendor;
        
        size_t pos = 0;
        while ((pos = cpu_model.find("(R)", pos)) != std::string::npos) {
            cpu_model.erase(pos, 3);
        }
        
        pos = cpu_vendor.find("GenuineIntel");
        if (pos != std::string::npos) {
            cpu_vendor.erase(pos, 12);
        }
        
        auto cleanup_string = [](std::string& str) {
            size_t pos = 0;
            while ((pos = str.find("  ", pos)) != std::string::npos) {
                str.erase(pos, 1);
            }
            pos = str.find_first_not_of(" ");
            if (pos != std::string::npos) {
                str.erase(0, pos);
            } else {
                str.clear();
            }
            pos = str.find_last_not_of(" ");
            if (pos != std::string::npos) {
                str.erase(pos + 1);
            }
        };
        
        cleanup_string(cpu_model);
        cleanup_string(cpu_vendor);
        
        strncpy(sys_info_.cpu_model, cpu_model.c_str(), sizeof(sys_info_.cpu_model) - 1);
        sys_info_.cpu_model[sizeof(sys_info_.cpu_model) - 1] = '\0';
        strncpy(sys_info_.cpu_vendor, cpu_vendor.c_str(), sizeof(sys_info_.cpu_vendor) - 1);
        sys_info_.cpu_vendor[sizeof(sys_info_.cpu_vendor) - 1] = '\0';
    }

    capabilities_.arch = CPUArchitecture::UNKNOWN;

#if defined(__x86_64__) || defined(_M_X64)
    capabilities_.arch = CPUArchitecture::X86_64;
#elif defined(__aarch64__) || defined(__arm64__) || defined(__arm__)
    capabilities_.arch = CPUArchitecture::ARM64;
#elif defined(__powerpc__) || defined(__ppc__) || defined(_M_PPC)
    capabilities_.arch = CPUArchitecture::POWERPC;
#elif defined(__riscv) && (__riscv_xlen == 64)
    capabilities_.arch = CPUArchitecture::RISCV64;
#endif

#if !defined(_WIN32)
    if (capabilities_.arch == CPUArchitecture::UNKNOWN) {
        struct utsname uname_info;
        if (uname(&uname_info) == 0) {
            std::string machine = uname_info.machine;
            if (machine == "x86_64") {
                capabilities_.arch = CPUArchitecture::X86_64;
            } else if (machine == "aarch64" || machine == "arm64") {
                capabilities_.arch = CPUArchitecture::ARM64;
            } else if (machine.find("ppc64") != std::string::npos) {
                capabilities_.arch = CPUArchitecture::POWERPC;
            } else if (machine.find("riscv64") != std::string::npos) {
                capabilities_.arch = CPUArchitecture::RISCV64;
            }
        }
    }
#endif

    capabilities_.model_name = sys_info_.cpu_model;
    capabilities_.physical_cores = sys_info_.total_physical_cores;
    capabilities_.logical_cores = sys_info_.total_logical_cores;
    capabilities_.sockets = sys_info_.socket_count;
    capabilities_.total_memory = static_cast<size_t>(sys_info_.total_mem_bytes);
    capabilities_.memory_type = sys_info_.mem_technology;
    capabilities_.memory_frequency = sys_info_.mem_frequency;
    capabilities_.cpu_inferred = sys_info_.cpu_inferred;

    bool has_cache_info = false;
    int detected_cache_line_size = 0;
    for (int i = 0; i < sys_info_.socket_count; ++i) {
        const auto& socket = sys_info_.sockets[i];
        for (int j = 0; j < socket.cache_count; ++j) {
            const auto& cache = socket.caches[j];
            if (cache.size_bytes > 0) {
                has_cache_info = true;
                switch (cache.level) {
                    case 1:
                        if (cache.type == si_cache_type::DATA) {
                            capabilities_.l1d_size = static_cast<size_t>(cache.size_bytes);
                        } else if (cache.type == si_cache_type::INSTRUCTION) {
                            capabilities_.l1i_size = static_cast<size_t>(cache.size_bytes);
                        }
                        break;
                    case 2:
                        capabilities_.l2_size = static_cast<size_t>(cache.size_bytes);
                        break;
                    case 3:
                        capabilities_.l3_size = static_cast<size_t>(cache.size_bytes);
                        break;
                }
            }
            if (detected_cache_line_size <= 0 && cache.line_size_bytes > 0) {
                detected_cache_line_size = cache.line_size_bytes;
            }
        }
        capabilities_.memory_channels = socket.mem_channels;
    }
    capabilities_.cache_line_size_bytes = (detected_cache_line_size > 0)
        ? detected_cache_line_size
        : detect_cache_line_size_fallback();
    
    auto get_bus_width_bytes = [](const std::string& mem_type, const std::string& vendor) -> int {
        std::string type_upper = mem_type;
        std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(), ::toupper);
        
        if (type_upper.find("HBM") != std::string::npos) {
            return 128;
        }
        
        if (vendor.find("Apple") != std::string::npos) {
            return 2;
        }
        
        return 8;
    };

    capabilities_.bus_width = get_bus_width_bytes(capabilities_.memory_type, sys_info_.cpu_vendor) * 8;

    std::string model_upper = sys_info_.cpu_model;
    std::transform(model_upper.begin(), model_upper.end(), model_upper.begin(), ::toupper);
    auto is_unknown_or_empty = [](const std::string& value) {
        return value.empty() || value == "Unknown";
    };

    if (model_upper.find("A64FX") != std::string::npos) {
        if (is_unknown_or_empty(capabilities_.memory_type)) {
            capabilities_.memory_type = "HBM2";
        }
        if (is_unknown_or_empty(capabilities_.memory_frequency)) {
            capabilities_.memory_frequency = "2000 MT/s";
        }
        if (capabilities_.memory_channels <= 0 || capabilities_.memory_channels == 2) {
            capabilities_.memory_channels = 4;
        }
        capabilities_.bus_width = 1024;
    } else if (model_upper.find("NEOVERSE-V2") != std::string::npos ||
               model_upper.find("NEOVERSE V2") != std::string::npos ||
               model_upper.find("GRACE") != std::string::npos) {
        if (is_unknown_or_empty(capabilities_.memory_type)) {
            capabilities_.memory_type = "LPDDR5X";
        }
        if (is_unknown_or_empty(capabilities_.memory_frequency)) {
            capabilities_.memory_frequency = "4266";
        }
        if (capabilities_.memory_channels <= 0 || capabilities_.memory_channels == 2) {
            capabilities_.memory_channels = 16;
        }
    } else if (capabilities_.vendor == CPUVendor::APPLE) {
        const int gpu_cores = detect_apple_gpu_cores();
        capabilities_.bus_width = 16;
        if (model_upper.find("ULTRA") != std::string::npos) {
            capabilities_.memory_channels = 64;
        } else if (model_upper.find("MAX") != std::string::npos) {
            capabilities_.memory_channels =
                ((gpu_cores == 30 && model_upper.find("M3") != std::string::npos) ||
                 (gpu_cores == 32 &&
                  (model_upper.find("M4") != std::string::npos ||
                   model_upper.find("M5") != std::string::npos)) ||
                 (gpu_cores == 0 && capabilities_.physical_cores == 14 &&
                  (model_upper.find("M3") != std::string::npos ||
                   model_upper.find("M4") != std::string::npos)) ||
                 (gpu_cores == 0 && model_upper.find("M5") != std::string::npos &&
                  capabilities_.total_memory > 0 &&
                  capabilities_.total_memory <= 36ULL * 1024 * 1024 * 1024)) ? 24 : 32;
        } else if (model_upper.find("PRO") != std::string::npos) {
            capabilities_.memory_channels = model_upper.find("M3") != std::string::npos ? 12 : 16;
        } else {
            capabilities_.memory_channels = 8;
        }

        const bool apple_pro_class =
            model_upper.find("PRO") != std::string::npos ||
            model_upper.find("MAX") != std::string::npos ||
            model_upper.find("ULTRA") != std::string::npos;
        if (model_upper.find("M1") != std::string::npos && !apple_pro_class) {
            capabilities_.memory_type = "LPDDR4X";
            capabilities_.memory_frequency = "4266";
        } else if (model_upper.find("M6") != std::string::npos) {
            capabilities_.memory_type = "LPDDR5X";
            capabilities_.memory_frequency =
                capabilities_.total_memory > 0 &&
                capabilities_.total_memory <= 16ULL * 1024 * 1024 * 1024 ? "9600" : "10667";
        } else if (model_upper.find("M5") != std::string::npos) {
            capabilities_.memory_type = "LPDDR5X";
            capabilities_.memory_frequency = "9600";
        } else if (model_upper.find("M4") != std::string::npos) {
            capabilities_.memory_type = "LPDDR5X";
            capabilities_.memory_frequency = apple_pro_class ? "8533" : "7500";
        } else if (model_upper.find("M1") != std::string::npos ||
                   model_upper.find("M2") != std::string::npos ||
                   model_upper.find("M3") != std::string::npos) {
            capabilities_.memory_type = "LPDDR5";
            capabilities_.memory_frequency = "6400";
        }
    }

    capabilities_.upi_freq = 0;
    capabilities_.n_data_lanes = 0;
    capabilities_.flit_bit = 0;
    capabilities_.data_flit_bit = 0;
    capabilities_.n_upi_channels = 0;

    std::string model = sys_info_.cpu_model; 
    // Sapphire Rapids (8480/9480)
    if (model.find("8480") != std::string::npos || model.find("9480") != std::string::npos || model.find("9462") != std::string::npos) {
        capabilities_.upi_freq = 16.0;
        capabilities_.n_data_lanes = 20;
        capabilities_.flit_bit = 80;
        capabilities_.data_flit_bit = 64;
        capabilities_.n_upi_channels = 4;
    }
    // Emerald Rapids (8568)
    else if (model.find("8568") != std::string::npos) {
        capabilities_.upi_freq = 20.0;
        capabilities_.n_data_lanes = 20;
        capabilities_.flit_bit = 80;
        capabilities_.data_flit_bit = 64;
        capabilities_.n_upi_channels = 4;
    }
    
    if (!has_cache_info || capabilities_.l3_size == 0) {
        capabilities_.l1d_size = detect_cache_size(1);
        capabilities_.l1i_size = capabilities_.l1i_size ? capabilities_.l1i_size : capabilities_.l1d_size;
        capabilities_.l2_size = capabilities_.l2_size ? capabilities_.l2_size : detect_cache_size(2);
        capabilities_.l3_size = capabilities_.l3_size ? capabilities_.l3_size : detect_cache_size(3);
    }

    // When sysfs doesn't expose cache sizes, try to identify the CPU via
    // the ARM MIDR register and use known cache geometry.
    if (capabilities_.arch == CPUArchitecture::ARM64 &&
        (!capabilities_.l1d_size || !capabilities_.l2_size || !capabilities_.l3_size)) {
        std::string midr_str = read_file("/sys/devices/system/cpu/cpu0/regs/identification/midr_el1");
        if (!midr_str.empty()) {
            uint64_t midr = 0;
            try { midr = std::stoull(midr_str, nullptr, 0); } catch (...) {}

            if (midr != 0) {
                uint8_t  implementer = (midr >> 24) & 0xFF;
                uint16_t part        = (midr >> 4)  & 0xFFF;

                struct midr_cache_info {
                    uint8_t  implementer;
                    uint16_t part;
                    size_t   l1d;           // per core
                    size_t   l1i;           // per core
                    size_t   l2_shared;     // shared L2 per cluster/CMG
                    int      cores_per_group; // cores sharing the L2
                    size_t   l3;            // total L3 (0 = no L3, LLC is L2)
                };

                // MIDR implementer codes: 0x46=Fujitsu, 0x41=ARM, 0x4E=NVIDIA
                static const midr_cache_info known[] = {
                    // Fujitsu A64FX: impl=0x46, part=0x001
                    { 0x46, 0x001, 64*1024, 64*1024, 8*1024*1024, 12, 0 },
                };

                for (const auto& k : known) {
                    if (implementer == k.implementer && part == k.part) {
                        if (!capabilities_.l1d_size) capabilities_.l1d_size = k.l1d;
                        if (!capabilities_.l1i_size) capabilities_.l1i_size = k.l1i;
                        if (!capabilities_.l2_size)  capabilities_.l2_size  = k.l2_shared;
                        if (!capabilities_.l3_size) {
                            if (k.l3 > 0) {
                                capabilities_.l3_size = k.l3;
                            } else {
                                // No L3 — total LLC = L2 × number of groups
                                int cores = sys_info_.total_physical_cores > 0 ? sys_info_.total_physical_cores : 1;
                                int groups = (cores + k.cores_per_group - 1) / k.cores_per_group;
                                capabilities_.l3_size = k.l2_shared * static_cast<size_t>(groups);
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    if (capabilities_.l3_size == 0) {
        if (capabilities_.l2_size > 0) {
            // No L3 and not a known CPU — use L2 as a rough LLC estimate.
            capabilities_.l3_size = capabilities_.l2_size;
        } else {
            std::cerr << "Warning: Could not determine LLC size from system. Using default 32MB." << std::endl;
            capabilities_.l3_size = 32 * 1000 * 1000;
        }
    }


    bool cpu_detected = true;
    switch (capabilities_.arch) {
        case CPUArchitecture::X86_64: cpu_detected = detect_x86_capabilities(); break;
        case CPUArchitecture::ARM64: cpu_detected = detect_arm_capabilities(); break;
        case CPUArchitecture::POWERPC: cpu_detected = detect_powerpc_capabilities(); break;
        case CPUArchitecture::RISCV64: cpu_detected = detect_riscv_capabilities(); break;
        default: break;
    }
    detect_gpu_capabilities();
    return cpu_detected;
}

bool SystemDetector::detect_x86_capabilities() {
#ifdef __x86_64__
    unsigned int eax, ebx, ecx, edx;

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        bool hw_avx512f = (ebx & bit_AVX512F);
        unsigned int eax1, ebx1, ecx1, edx1;
        bool os_supports_state = false;
        if (__get_cpuid(1, &eax1, &ebx1, &ecx1, &edx1)) {
            bool osxsave = (ecx1 & bit_OSXSAVE);
            if (osxsave) {
                uint64_t xcr0 = xgetbv(0);
                const uint64_t need = (1ULL<<1) | (1ULL<<2) | (1ULL<<5) | (1ULL<<6) | (1ULL<<7);
                os_supports_state = (xcr0 & need) == need;
            }
        }
        if (hw_avx512f && os_supports_state) {
            capabilities_.extensions.push_back(ISAExtension::AVX512);
        }
    }

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & bit_AVX2) {
            capabilities_.extensions.push_back(ISAExtension::AVX2);
        }
    }

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        bool hw_avx = (ecx & bit_AVX);
        bool osxsave = (ecx & bit_OSXSAVE);
        bool os_avx = false;
        if (osxsave) {
            uint64_t xcr0 = xgetbv(0);
            const uint64_t need = (1ULL<<1) | (1ULL<<2);
            os_avx = (xcr0 & need) == need;
        }
        if (hw_avx && os_avx) {
            capabilities_.extensions.push_back(ISAExtension::AVX);
        }
    }

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & bit_SSE4_2) capabilities_.extensions.push_back(ISAExtension::SSE4_2);
        if (ecx & bit_SSE4_1) capabilities_.extensions.push_back(ISAExtension::SSE4_1);
        if (ecx & bit_SSSE3) capabilities_.extensions.push_back(ISAExtension::SSSE3);
        if (ecx & bit_SSE3) capabilities_.extensions.push_back(ISAExtension::SSE3);
        if (edx & bit_SSE2) capabilities_.extensions.push_back(ISAExtension::SSE2);
        if (edx & bit_SSE) capabilities_.extensions.push_back(ISAExtension::SSE);
    }
#endif
    return true;
}

bool SystemDetector::detect_arm_capabilities() {
#ifdef __linux__
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        bool has_neon = false;
        bool has_sve = false;
        bool has_sve2 = false;
        bool is_grace = false;
        bool is_a64fx = false;
        while (std::getline(cpuinfo, line)) {
            std::string lower_line = line;
            std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lower_line.find("asimd") != std::string::npos ||
                lower_line.find(" neon") != std::string::npos ||
                lower_line.rfind("neon", 0) == 0) {
                has_neon = true;
            }

            if (lower_line.find("sve2") != std::string::npos) {
                has_sve2 = true;
                has_sve = true;
            } else if (lower_line.find("sve") != std::string::npos) {
                has_sve = true;
            }

            std::string upper_line = line;
            std::transform(upper_line.begin(), upper_line.end(), upper_line.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (upper_line.find("NEOVERSE-V2") != std::string::npos || 
                upper_line.find("NEOVERSE V2") != std::string::npos ||
                upper_line.find("GRACE") != std::string::npos) {
                is_grace = true;
            }
            if (upper_line.find("A64FX") != std::string::npos) {
                is_a64fx = true;
            }
        }

        if (has_neon) {
            capabilities_.extensions.push_back(ISAExtension::NEON);
        }
        if (has_sve2) {
            capabilities_.extensions.push_back(ISAExtension::SVE2);
        }
        if (has_sve) {
            capabilities_.extensions.push_back(ISAExtension::SVE);
        }

        if (has_sve) {
            // Linux reports SVE VL in bytes through this tunable.
            // Convert to bits for ISA width resolution in the generator.
            const std::string sve_vl_text = read_file("/proc/sys/abi/sve_default_vector_length");
            uint64_t sve_vl_bytes = 0;
            if (parse_first_uint64(sve_vl_text, sve_vl_bytes) && sve_vl_bytes > 0) {
                capabilities_.sve_vector_bits = static_cast<int>(sve_vl_bytes * 8);
            } else if (is_a64fx || capabilities_.isA64FX()) {
                // A64FX has a fixed 512-bit SVE implementation.
                capabilities_.sve_vector_bits = 512;
            }

#if defined(__linux__) && defined(__aarch64__) && defined(PR_SVE_GET_VL) && defined(PR_SVE_VL_LEN_MASK)
            const int current_sve_bits = detect_current_sve_bits_via_prctl();
            if (current_sve_bits > 0) {
                capabilities_.sve_vector_bits = current_sve_bits;
            }
#endif
        }

        if (is_grace) {
            capabilities_.nvlink_bw_gb_s = 450.0;
        }
    }
#elif defined(__APPLE__)
    int has_advsimd = 1;
    size_t advsimd_len = sizeof(has_advsimd);
    if (sysctlbyname("hw.optional.AdvSIMD", &has_advsimd, &advsimd_len, nullptr, 0) != 0) {
        has_advsimd = 1;  // guaranteed on Apple Silicon
    }
    if (has_advsimd) {
        capabilities_.extensions.push_back(ISAExtension::NEON);
    }
#endif

    return true;
}

bool SystemDetector::detect_powerpc_capabilities() {
    capabilities_.extensions.push_back(ISAExtension::VSX);
    capabilities_.extensions.push_back(ISAExtension::VMX);
    return true;
}

bool SystemDetector::detect_riscv_capabilities() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        bool found_rvv = false;
        while (std::getline(cpuinfo, line)) {
            if (line.find("rvv") != std::string::npos) {
                if (line.find("rvv1.0") != std::string::npos) {
                    capabilities_.extensions.push_back(ISAExtension::RVV1_0);
                } else if (line.find("rvv0.7") != std::string::npos) {
                    capabilities_.extensions.push_back(ISAExtension::RVV0_7);
                }
                found_rvv = true;
            }

            // Common CPUinfo fields: "vlenb : 32" or "vlen : 256"
            if (line.find("vlenb") != std::string::npos) {
                uint64_t vlenb = 0;
                if (parse_first_uint64(line, vlenb) && vlenb > 0) {
                    capabilities_.rvv_vector_bits = static_cast<int>(vlenb * 8);
                }
            } else if (line.find("vlen") != std::string::npos) {
                uint64_t vlen_bits = 0;
                if (parse_first_uint64(line, vlen_bits) && vlen_bits > 0) {
                    capabilities_.rvv_vector_bits = static_cast<int>(vlen_bits);
                }
            }
        }

        if (found_rvv && capabilities_.rvv_vector_bits == 0) {
            capabilities_.rvv_vector_bits = 256;
        }
    }
    return true;
}

bool SystemDetector::detect_gpu_capabilities() {
    gpu_capabilities_ = GPUCapabilities{};
    subprocess::Options options;
    options.timeout = std::chrono::seconds(5);
    const auto query = subprocess::capture("nvidia-smi",
        {"--query-gpu=name,memory.total", "--format=csv,noheader"}, options);
    if (!query.ok() || query.output.size() >= options.max_output_bytes) return false;

    std::vector<std::string> compute_capabilities;
    const auto capability_query = subprocess::capture("nvidia-smi",
        {"--query-gpu=compute_cap", "--format=csv,noheader"}, options);
    if (capability_query.ok()) {
        std::istringstream lines(capability_query.output);
        for (std::string line; std::getline(lines, line);) {
            line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
            compute_capabilities.push_back(line);
        }
    }

    std::istringstream rows(query.output);
    std::string row;
    int device_index = 0;
    while (std::getline(rows, row)) {
        const int index = device_index++;
        const auto comma = row.rfind(',');
        if (comma == std::string::npos) continue;
        GPUInfo info;
        info.name = row.substr(0, comma);
        if (index < static_cast<int>(compute_capabilities.size())) info.compute_capability = compute_capabilities[index];
        try {
            info.memory_bytes = std::stoull(row.substr(comma + 1)) * 1024ULL * 1024ULL;
        } catch (const std::exception&) {
            continue;
        }
        gpu_capabilities_.gpus.push_back(info);
    }
    gpu_capabilities_.num_gpus = static_cast<int>(gpu_capabilities_.gpus.size());
    gpu_capabilities_.has_gpu = !gpu_capabilities_.gpus.empty();
    if (gpu_capabilities_.has_gpu) gpu_capabilities_.gpu_arch = GPUArchitecture::NVIDIA;
    return gpu_capabilities_.has_gpu;
}

void SystemDetector::print(std::ostream& os, int verbosity) const {
    os << "System Information:" << std::endl;
    os << "  OS: " << sys_info_.os_name << " " << sys_info_.os_release << std::endl;
    os << "  Architecture: ";
    switch (capabilities_.arch) {
        case CPUArchitecture::X86_64: os << "x86_64"; break;
        case CPUArchitecture::ARM64: os << "ARM64"; break;
        case CPUArchitecture::POWERPC: os << "PowerPC"; break;
        case CPUArchitecture::RISCV64: os << "RISC-V 64"; break;
        default: os << "Unknown"; break;
    }
    os << std::endl;

    os << "  CPU: " << sys_info_.cpu_vendor << " " << sys_info_.cpu_model << std::endl;
    os << "  Cores: " << capabilities_.physical_cores << " physical, "
       << capabilities_.logical_cores << " logical" << std::endl;
    os << "  Sockets: " << capabilities_.sockets << std::endl;

    if (verbosity >= 2) {
        os << "  Memory: " << (capabilities_.total_memory / (1000*1000*1000)) << " GB" << std::endl;

        auto is_unknown = [](const std::string& v) { return v.empty() || v == "Unknown"; };
        const bool mem_undetected =
            is_unknown(capabilities_.memory_type) || is_unknown(capabilities_.memory_frequency);

        // Windows reads the same data from SMBIOS without privilege, so the
        // consumer-CPU caveat and the dmidecode remedy do not apply there.
#if defined(_WIN32)
        const bool report_undetected = mem_undetected;
        const char* const undetected_reason = "firmware reported no SMBIOS memory devices";
#else
        const bool report_undetected = cpu_is_consumer(sys_info_.cpu_model) && mem_undetected;
        const char* const undetected_reason = "dmidecode requires root/sudo";
#endif

        if (report_undetected) {
            os << "  Memory system not detected (" << undetected_reason << "). "
                  "Theoretical peaks not available." << std::endl;
        } else {
            if (!capabilities_.memory_type.empty()) {
                os << "  Memory Type: " << capabilities_.memory_type;
                if (!capabilities_.memory_frequency.empty()) {
                    os << " @ " << capabilities_.memory_frequency;
                }
                os << std::endl;
            }
            os << "  Memory Channels: " << capabilities_.memory_channels;
            if (capabilities_.bus_width > 0) {
                os << " x " << capabilities_.bus_width << "-bit";
            }
            os << std::endl;
        }

        if (capabilities_.l1d_size > 0) os << "  L1D Cache: " << (capabilities_.l1d_size / 1000) << " KB" << std::endl;
        if (capabilities_.l1i_size > 0) os << "  L1I Cache: " << (capabilities_.l1i_size / 1000) << " KB" << std::endl;
        if (capabilities_.l2_size > 0) os << "  L2 Cache: " << (capabilities_.l2_size / 1000) << " KB" << std::endl;
        if (capabilities_.l3_size > 0) os << "  L3 Cache: " << (capabilities_.l3_size / (1000*1000)) << " MB" << std::endl;

        if (verbosity >= 3 && !capabilities_.extensions.empty()) {
            os << "  ISA Extensions: ";
            for (size_t i = 0; i < capabilities_.extensions.size(); ++i) {
                if (i > 0) os << ", ";
                switch (capabilities_.extensions[i]) {
                    case ISAExtension::AVX512: os << "AVX512"; break;
                    case ISAExtension::AVX2: os << "AVX2"; break;
                    case ISAExtension::AVX: os << "AVX"; break;
                    case ISAExtension::SSE4_2: os << "SSE4.2"; break;
                    case ISAExtension::SSE4_1: os << "SSE4.1"; break;
                    case ISAExtension::SSSE3: os << "SSSE3"; break;
                    case ISAExtension::SSE3: os << "SSE3"; break;
                    case ISAExtension::SSE2: os << "SSE2"; break;
                    case ISAExtension::SSE: os << "SSE"; break;
                    case ISAExtension::NEON: os << "NEON"; break;
                    case ISAExtension::SVE2: os << "SVE2"; break;
                    case ISAExtension::SVE: os << "SVE"; break;
                    case ISAExtension::RVV0_7: os << "RVV0.7"; break;
                    case ISAExtension::RVV1_0: os << "RVV1.0"; break;
                    case ISAExtension::VMX: os << "VMX"; break;
                    case ISAExtension::VSX: os << "VSX"; break;
                    default: os << "Unknown"; break;
                }
            }
            os << std::endl;
        }

        if (capabilities_.sve_vector_bits > 0) {
            os << "  SVE Vector Length: " << capabilities_.sve_vector_bits << " bits" << std::endl;
        }
        if (capabilities_.rvv_vector_bits > 0) {
            os << "  RVV Vector Length: " << capabilities_.rvv_vector_bits << " bits" << std::endl;
        }
    }
}
