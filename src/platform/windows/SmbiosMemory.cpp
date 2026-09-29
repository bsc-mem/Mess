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

#include "WinSysInfo.h"

#include <cctype>
#include <cstring>

namespace platform {
namespace win {

namespace {

#pragma pack(push, 1)
/** @brief Header GetSystemFirmwareTable prepends to the SMBIOS table data. */
struct RawSmbiosData {
    BYTE  Used20CallingMethod;
    BYTE  SMBIOSMajorVersion;
    BYTE  SMBIOSMinorVersion;
    BYTE  DmiRevision;
    DWORD Length;
    BYTE  SMBIOSTableData[1];
};
struct SmbiosHeader { BYTE Type; BYTE Length; WORD Handle; };
#pragma pack(pop)

// 'RSMB' spelled out: a multi-character constant warns and its byte order is
// implementation-defined.
constexpr DWORD kProviderRSMB = 0x52534D42u;

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/** Strings follow the formatted area: NUL-terminated, double-NUL-terminated. */
std::string smbios_string(const BYTE* start, BYTE formatted_len, BYTE index, const BYTE* end) {
    if (index == 0) return {};
    const char* s = reinterpret_cast<const char*>(start + formatted_len);
    for (BYTE i = 1; s < reinterpret_cast<const char*>(end) && *s; ++i) {
        if (i == index) return trim(std::string(s));
        s += std::strlen(s) + 1;
    }
    return {};
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}

}

const char* smbios_memory_type_name(uint8_t type) {
    switch (type) {
        case 0x12: return "DDR";    case 0x13: return "DDR2";
        case 0x18: return "DDR3";   case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";  case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3"; case 0x1E: return "LPDDR4";
        case 0x20: return "HBM";    case 0x21: return "HBM2";
        case 0x22: return "DDR5";   case 0x23: return "LPDDR5";
        case 0x24: return "HBM3";
        default:   return "";       // leave Unknown; SystemDetection has fallbacks
    }
}

std::vector<MemoryDevice> read_smbios_memory_devices() {
    std::vector<MemoryDevice> out;

    const DWORD needed = GetSystemFirmwareTable(kProviderRSMB, 0, nullptr, 0);
    if (needed == 0) return out;

    std::vector<BYTE> buf(needed);
    if (GetSystemFirmwareTable(kProviderRSMB, 0, buf.data(), needed) != needed) return out;

    const auto* raw = reinterpret_cast<const RawSmbiosData*>(buf.data());
    const BYTE* p = raw->SMBIOSTableData;
    const BYTE* const end = p + raw->Length;

    while (p + sizeof(SmbiosHeader) <= end) {
        const auto* h = reinterpret_cast<const SmbiosHeader*>(p);
        if (h->Length < sizeof(SmbiosHeader)) break;
        if (h->Type == 127) break;                       // end-of-table

        if (h->Type == 17 && h->Length >= 0x15 && p + h->Length <= end) {
            const uint16_t size_field = *reinterpret_cast<const uint16_t*>(p + 0x0C);
            if (size_field != 0 && size_field != 0xFFFF) {
                MemoryDevice d;
                if (size_field == 0x7FFF && h->Length >= 0x20) {
                    // Extended Size at 0x1C, in MB, bit31 reserved.
                    d.size_mb = *reinterpret_cast<const uint32_t*>(p + 0x1C) & 0x7FFFFFFFu;
                } else if (size_field & 0x8000) {
                    d.size_mb = (size_field & 0x7FFF) / 1024;   // value is in KB
                } else {
                    d.size_mb = size_field;
                }
                d.memory_type = *(p + 0x12);
                d.data_width_bits = *reinterpret_cast<const uint16_t*>(p + 0x0A);
                d.speed_mts = *reinterpret_cast<const uint16_t*>(p + 0x15);
                if (h->Length >= 0x22) {
                    d.configured_mts = *reinterpret_cast<const uint16_t*>(p + 0x20);
                }
                d.device_locator = smbios_string(p, h->Length, *(p + 0x10), end);
                d.bank_locator = smbios_string(p, h->Length, *(p + 0x11), end);
                out.push_back(std::move(d));
            }
        }

        // Skip the formatted area, then the string set (terminated by 0x0000).
        const BYTE* q = p + h->Length;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        p = q + 2;
    }
    return out;
}

std::string channel_key(const std::string& device, const std::string& bank) {
    const std::string d = to_upper(device);

    // Channel names can repeat across controllers.
    std::string controller;
    const auto ctrl = d.find("CONTROLLER");
    if (ctrl != std::string::npos) {
        for (size_t i = ctrl + 10; i < d.size(); ++i) {
            if (d[i] >= '0' && d[i] <= '9') { controller.push_back(d[i]); continue; }
            if (controller.empty() && (d[i] == ' ' || d[i] == '-' || d[i] == '_')) continue;
            break;
        }
        if (!controller.empty()) controller += "|";
    }

    const auto ch = d.find("CHANNEL");
    if (ch != std::string::npos) {
        for (size_t i = ch + 7; i < d.size(); ++i) {
            if (d[i] >= 'A' && d[i] <= 'H') return controller + std::string(1, d[i]);
            if (d[i] >= '0' && d[i] <= '9') return controller + std::string(1, d[i]);
            if (d[i] != ' ' && d[i] != '-' && d[i] != '_') break;
        }
    }
    const auto dimm = d.find("DIMM");
    if (dimm != std::string::npos) {
        for (size_t i = dimm + 4; i < d.size(); ++i) {
            if (d[i] == ' ' || d[i] == '_' || d[i] == '-') continue;
            if (d[i] >= 'A' && d[i] <= 'H') return controller + std::string(1, d[i]);
            break;
        }
    }
    if (!bank.empty()) return controller + to_upper(bank);
    return d.empty() ? std::string("?") : d;
}

}
}
