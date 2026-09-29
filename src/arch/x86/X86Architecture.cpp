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

#include "arch/x86/X86Architecture.h"
#include "arch/x86/X86Assembler.h"
#include "arch/x86/counters/IntelSkylakeCounters.h"
#include "arch/x86/counters/IntelSPRCounters.h"
#include "arch/x86/counters/IntelEmeraldRapidsCounters.h"
#include "arch/x86/counters/IntelGraniteRapidsCounters.h"
#include "arch/x86/counters/AmdZenCounters.h"
#include "arch/x86/counters/AmdZen4Counters.h"
#include "arch/x86/counters/AmdZen5Counters.h"
#include "arch/x86/X86Counters.h"
#include "architecture/ArchitectureRegistry.h"
#include "utils/StringUtils.h"
#include <algorithm>
#include <string>
#include <cctype>

static ArchitectureRegistrar<X86Architecture> x86_registrar;

namespace {

class X86ISA : public ISA {
    ISAMode mode_;
    std::string name_;
    int width_bits_;
    int regs_;
public:
    X86ISA(ISAMode mode, std::string name, int width_bits, int regs)
        : mode_(mode), name_(std::move(name)), width_bits_(width_bits), regs_(regs) {}

    ISAMode getMode() const override { return mode_; }
    std::string getName() const override { return name_; }
    int getVectorWidthBits() const override { return width_bits_; }
    int getMaxSimdRegisters() const override { return regs_; }
};



std::unique_ptr<BandwidthCounterStrategy> makeLatestIntelCounterStrategy() {
    // Keep unknown Intel fallback pinned to latest supported Intel server generation.
    // Update this helper when adding a newer Intel counter implementation.
    return std::make_unique<IntelGraniteRapidsCounters>();
}

std::unique_ptr<BandwidthCounterStrategy> makeLatestAmdCounterStrategy() {
    return std::make_unique<AmdZen5Counters>();
}

}

bool X86Architecture::supports(const CPUCapabilities& caps) const {
    return caps.arch == CPUArchitecture::X86_64;
}

std::unique_ptr<KernelAssembler> X86Architecture::createAssembler(const KernelConfig& config) const {
    return std::make_unique<X86Assembler>(config);
}

std::unique_ptr<BandwidthCounterStrategy> X86Architecture::createCounterStrategy(const CPUCapabilities& caps) const {
    std::string model = caps.model_name;
    
    if (caps.vendor == CPUVendor::INTEL) {
        if (string_utils::contains_ci(model, "granite") || string_utils::contains_ci(model, "gnr")) {
            return std::make_unique<IntelGraniteRapidsCounters>();
        } else if (string_utils::contains_ci(model, "emerald") || string_utils::contains_ci(model, "emr")) {
            return std::make_unique<IntelEmeraldRapidsCounters>();
        } else if (string_utils::contains_ci(model, "sapphire") || string_utils::contains_ci(model, "spr")) {
            return std::make_unique<IntelSPRCounters>();
        } else if (string_utils::contains_ci(model, "skylake")) {
            return std::make_unique<IntelSkylakeCounters>();
        }
        
        return makeLatestIntelCounterStrategy();
        
    } else if (caps.vendor == CPUVendor::AMD) {
        if (string_utils::contains_ci(model, "turin") || string_utils::contains_ci(model, "zen5") || string_utils::contains_ci(model, "zen 5")) {
            return std::make_unique<AmdZen5Counters>();
        } else if (string_utils::contains_ci(model, "genoa") || string_utils::contains_ci(model, "bergamo") || string_utils::contains_ci(model, "zen4") || string_utils::contains_ci(model, "zen 4")) {
            return std::make_unique<AmdZen4Counters>();
        } else if (string_utils::contains_ci(model, "milan") || string_utils::contains_ci(model, "rome") || string_utils::contains_ci(model, "zen3") || string_utils::contains_ci(model, "zen 3") || string_utils::contains_ci(model, "zen2") || string_utils::contains_ci(model, "zen 2")) {
            return std::make_unique<AmdZenCounters>();
        }
        
        return makeLatestAmdCounterStrategy();
    }

    return std::make_unique<X86Counters>();
}

std::vector<std::shared_ptr<ISA>> X86Architecture::getSupportedISAs() const {
    return {
        std::make_shared<X86ISA>(ISAMode::AVX512, "AVX-512", 512, 32),
        std::make_shared<X86ISA>(ISAMode::AVX2,   "AVX2",    256, 16),
        std::make_shared<X86ISA>(ISAMode::AVX,    "AVX",     256, 16),
        std::make_shared<X86ISA>(ISAMode::SSE2,   "SSE2",    128, 16),
        std::make_shared<X86ISA>(ISAMode::SCALAR, "SCALAR",  64,  14)
    };
}

std::shared_ptr<ISA> X86Architecture::selectBestISA(const CPUCapabilities& caps) const {
    auto has = [&](ISAExtension e){ 
        return std::find(caps.extensions.begin(), caps.extensions.end(), e) != caps.extensions.end(); 
    };

    if (has(ISAExtension::AVX2))   return std::make_shared<X86ISA>(ISAMode::AVX2,   "AVX2",    256, 16);
    if (has(ISAExtension::AVX512)) return std::make_shared<X86ISA>(ISAMode::AVX512, "AVX-512", 512, 32);
    if (has(ISAExtension::AVX))    return std::make_shared<X86ISA>(ISAMode::AVX,    "AVX",     256, 16);
    if (has(ISAExtension::SSE2))   return std::make_shared<X86ISA>(ISAMode::SSE2,   "SSE2",    128, 16);
    
    return std::make_shared<X86ISA>(ISAMode::SCALAR, "SCALAR", 64, 14);
}

double X86Architecture::getUpiScalingFactor(const CPUCapabilities& caps) const {
    if (caps.vendor == CPUVendor::INTEL) {
        // Intel UPI typically uses 1 header flit + 8 data flits = 9 flits per 64B cache line
        return 1.0 / 9.0;
    } else if (caps.vendor == CPUVendor::AMD) {
        return 1.0;
    }
    return 1.0;
}
