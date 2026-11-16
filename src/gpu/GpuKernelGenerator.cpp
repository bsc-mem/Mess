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

#include "gpu/GpuKernelGenerator.h"
#include "BenchmarkConfig.h"
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>

namespace {
constexpr int kPausePollThreshold = 256;
constexpr int kOpsPerPauseBlock = 10;

struct MultiSeqLayout {
    int loadSlotBytes;
    int loadBytes;
    int storeSlotBytes;
    int storeBytes;
};

MultiSeqLayout multiSeqLayout(const KernelConfigMultiSeq& config) {
    if (config.granularity == GpuAccessGranularity::LINE) return {4, 4, 4, 4};
    return {128, 8, 32, config.full_sector_stores ? 32 : 8};
}

std::string sinkDeclarations() {
    const std::string count = std::to_string(kOpsPerPauseBlock);
    return "        \".reg .u64 sink<" + count + ">;\"\n"
           "        \".reg .u32 sinkWord<" + count + ">;\"\n";
}

// Independent loads get their own register and are consumed only at the next pause block;
// warps issue in order, so consuming a load right away would stall the following ones.
class LoadSinks {
public:
    explicit LoadSinks(bool wide) : wide_(wide) {}

    std::string load(const std::string& address) {
        const std::string index = std::to_string(pending_.size());
        pending_.push_back(index);
        return wide_ ? "                \"ld.global.cv.u64 sink" + index + ", [" + address + "];\"\n"
                     : "                \"ld.global.cv.u32 sinkWord" + index + ", [" + address + "];\"\n";
    }

    std::string consume() {
        std::string code;
        for (const std::string& index : pending_) {
            if (!wide_) code += "                \"cvt.u64.u32 sink" + index + ", sinkWord" + index + ";\"\n";
            code += "                \"add.u64 loadedElem, loadedElem, sink" + index + ";\"\n";
        }
        pending_.clear();
        return code;
    }

private:
    bool wide_;
    std::vector<std::string> pending_;
};

struct ModeSymbols {
    const char* name;
    const char* prefix;
};

ModeSymbols mode_symbols([[maybe_unused]] ExecutionMode mode) {
    return {"multiseq", "ratio_"};
}

std::string launcher(const std::string& name, const std::string& kernel) {
    return "\nvoid launch_" + name + "(const MessArgs& args, int blocks, int threads) {\n"
           "    " + kernel + "<<<blocks, threads>>>(args);\n}\n\n"
           "int blocks_" + name + "(int threads) {\n"
           "    return resident_blocks(" + kernel + ", threads);\n}\n";
}

std::string storeCode(int bytes, const std::string& address, const std::string& value, const std::string& valueReg) {
    if (bytes == 4) return "                \"st.global.wt.u32 [" + address + "], " + value + ";\"\n";
    if (bytes == 8) return "                \"st.global.wt.u64 [" + address + "], " + value + ";\"\n";
    const std::string pair = "{" + valueReg + ", " + valueReg + "}";
    return "                \"st.global.wt.v2.u64 [" + address + "], " + pair + ";\"\n"
           "                \"st.global.wt.v2.u64 [" + address + "+16], " + pair + ";\"\n";
}
}

std::string GpuKernelGenerator::generate_pointer_chase() {
    constexpr int loop_size = 2000;
    const std::string instruction = "\"ld.global.cg.u64 %0, [%0];\\n\\t\" \\\n";
    std::string result = "#define PTR_CHASE_LOOP_SIZE " + std::to_string(loop_size) +
                         "\n#define GPU_PTR_CHASE_LOOP \\\n";
    for (int i = 0; i < loop_size; ++i) {
        result += instruction;
    }
    result += "\n";
    return result;
}

const char* GpuKernelGenerator::mode_name(ExecutionMode mode) {
    return mode_symbols(mode).name;
}

std::pair<std::string, std::string> GpuKernelGenerator::mode_workload([[maybe_unused]] ExecutionMode mode, int minTotalOps, int ratio_pct, const BenchmarkConfig& config) {
    const std::string suffix = std::to_string(ratio_pct) + ".cu";
    return {"workload_" + suffix, generate_workload(minTotalOps, ratio_pct, config.gpu_multiseq)};
}

std::string GpuKernelGenerator::generate_multiseq_body(int minTotalOps, int ratio_pct, const KernelConfigMultiSeq& config) {
    int totalOps = 0, loads = 0, stores = 0;
    computeLoadStoreOps(minTotalOps, ratio_pct, totalOps, loads, stores);
    return "struct WorkloadRatio" + std::to_string(ratio_pct) + " {\n" + genFunctInit() +
        loadStoreOpsGen(totalOps, loads, stores, config) + genFunctEnd(loads, stores, config) + "\n};\n";
}

std::string GpuKernelGenerator::generate_workload(int minTotalOps, int ratio_pct, const KernelConfigMultiSeq& config) {
    const std::string ratio = std::to_string(ratio_pct);
    return "#include \"kernels.cuh\"\n\n" + generate_multiseq_body(minTotalOps, ratio_pct, config) +
        launcher("ratio_" + ratio, "mess<WorkloadRatio" + ratio + ">");
}

std::string GpuKernelGenerator::generate_workload_table(int minTotalOps, const std::vector<int>& ratios_pct, const std::vector<ExecutionMode>& modes,
                                                        const KernelConfigMultiSeq& config) {
    std::string declarations;
    std::string entries;
    for (ExecutionMode mode : modes) {
        const ModeSymbols symbols = mode_symbols(mode);
        for (int ratio_pct : ratios_pct) {
            int totalOps = 0;
            int loads = 0;
            int stores = 0;
            computeLoadStoreOps(minTotalOps, ratio_pct, totalOps, loads, stores);
            const std::string name = symbols.prefix + std::to_string(ratio_pct);
            declarations += "void launch_" + name + "(const MessArgs& args, int blocks, int threads);\n";
            declarations += "int blocks_" + name + "(int threads);\n";
            entries += "    {\"" + std::string(symbols.name) + "\", " + std::to_string(ratio_pct) + ", " + std::to_string(loads) + ", " + std::to_string(stores) +
                       ", launch_" + name + ", blocks_" + name + "},\n";
        }
    }
    const MultiSeqLayout layout = multiSeqLayout(config);
    return "#include \"kernels.cuh\"\n\n"
           "#define MESS_LOAD_SLOT_BYTES " + std::to_string(layout.loadSlotBytes) + "\n"
           "#define MESS_LOAD_BYTES " + std::to_string(layout.loadBytes) + "\n"
           "#define MESS_STORE_SLOT_BYTES " + std::to_string(layout.storeSlotBytes) + "\n\n" + declarations +
           "\nconst WorkloadVariant kWorkloads[] = {\n" + entries + "};\n";
}


void GpuKernelGenerator::computeLoadStoreOps(int minTotalOps, double loadPercentage, int &outTotalOps, int &outLoadOps, int &outStoreOps) {
    loadPercentage /= 100.0;
    outLoadOps = std::round(minTotalOps * loadPercentage);
    outStoreOps = minTotalOps - outLoadOps;
    outTotalOps = outLoadOps + outStoreOps;
}

std::string GpuKernelGenerator::genFunctInit() {
    return R"(
__device__ static void run(uint64_t threadPointer_LD, uint64_t threadPointer_ST, uint64_t pauseValue,
        uint64_t loadStrideBytes, uint64_t storeStrideBytes, uint64_t endFlagPointer, uint64_t startFlagPointer,
        uint64_t sectionsLD, uint64_t sectionsST, uint64_t firstPause, int* result){

    uint64_t loopAccum = 0;
    uint64_t loadedElem = 0;

    asm volatile ( "{"

        ".reg .pred pausePred, laneSTPred, laneLDPred, endFlagPred, startFlagPred, numberSTPred, numberLDPred;"
        ".reg .pred pollPred, pollNowPred, endNowPred;"
        ".reg .u64 threadPointerLD;"
        ".reg .u64 threadPointerST;"
        ".reg .u64 loadedElem, storeValue;"
        ".reg .u32 loadedWord;"
        ".reg .u64 pauseValue, nopIter, pollIter, loopAccum, firstNop;"
        ".reg .u64 numberLD, numberST, elemOffsetLD, elemOffsetST;"
        ".reg .u64 endFlagPointer;"
        ".reg .u32 endFlagValue, endFlagTrue;"
        ".reg .u64 startFlagPointer;"
        ".reg .u64 sectionsLD, sectionsST, loadStrideBytes, storeStrideBytes;"
)" + sinkDeclarations() + R"(
        "mov.u64 threadPointerLD, %2;"
        "mov.u64 threadPointerST, %3;"
        "mov.u64 pauseValue, %4;"
        "mov.u64 loadStrideBytes, %5;"
        "mov.u64 storeStrideBytes, %6;"
        "mov.u64 endFlagPointer, %7;"
        "mov.u64 startFlagPointer, %8;"
        "mov.u64 sectionsLD, %9;"
        "mov.u64 sectionsST, %10;"
        "mov.u64 firstNop, %11;"
        "mov.u64 loopAccum, 0;"
        "mov.u64 loadedElem, 0;"
        "mov.u64 storeValue, 1024;"

        "mov.u32 endFlagTrue, 1;"
        "mov.u32 endFlagValue, 0;"
        "mov.pred startFlagPred, 1;"
        "mov.pred endNowPred, 0;"
        "mov.pred pollNowPred, 0;"
        "setp.ge.u64 pollPred, pauseValue, )" + std::to_string(kPausePollThreshold) + R"(;"

        "mov.u64 numberLD, 0;"
        "mov.u64 elemOffsetLD, 0;"
        "mov.u64 numberST, 0;"
        "mov.u64 elemOffsetST, 0;"

        // Access to different elements in the cache line
        "cache_line_loop:"
)";
}

std::string GpuKernelGenerator::genPauseBlock(int index) {
    std::string label = "noploop_" + std::to_string(index);
    return R"(
                "@startFlagPred st.global.u32 [startFlagPointer], 1;"
                "@startFlagPred mov.pred startFlagPred, 0;"
                "@pollPred ld.global.cv.u32 endFlagValue, [endFlagPointer];"
                "setp.ne.u64 pausePred, pauseValue, 0;"
                "@pausePred mov.u64 nopIter, )" + std::string(index == 0 ? "firstNop" : "0") + R"(;"
                "@pollPred bra poll_)" + label + R"(;"
                ")" + label + R"(:"
                    "@pausePred add.u64 nopIter, nopIter, 1;"
                    "@pausePred setp.lt.u64 pausePred, nopIter, pauseValue;"
                    "@pausePred add.u64 loopAccum, loopAccum, nopIter;"
                    "@pausePred bra )" + label + R"(;"
                "bra pause_done_)" + label + R"(;"
                "poll_)" + label + R"(:"
                    "@pausePred add.u64 nopIter, nopIter, 1;"
                    "@pausePred setp.lt.u64 pausePred, nopIter, pauseValue;"
                    "@pausePred add.u64 loopAccum, loopAccum, nopIter;"
                    "and.b64 pollIter, nopIter, )" + std::to_string(kPausePollThreshold - 1) + R"(;"
                    "setp.eq.u64 pollNowPred, pollIter, 0;"
                    "@pollNowPred ld.global.cv.u32 endFlagValue, [endFlagPointer];"
                    "setp.eq.u32 endNowPred, endFlagValue, endFlagTrue;"
                    "@endNowPred bra workload_done;"
                    "@pausePred bra poll_)" + label + R"(;"
                "pause_done_)" + label + R"(:"
                "@pollPred setp.eq.u32 endNowPred, endFlagValue, endFlagTrue;"
                "@endNowPred bra workload_done;"

)";
}

std::vector<bool> GpuKernelGenerator::operationOrder(int totalOps, int targetLoads, int targetStores) {
    std::vector<bool> loads;
    loads.reserve(totalOps);
    loads.insert(loads.end(), targetLoads, true);
    loads.insert(loads.end(), targetStores, false);
    std::mt19937 g(12345);
    std::shuffle(loads.begin(), loads.end(), g);
    return loads;
}

std::string GpuKernelGenerator::loadStoreOpsGen(int totalOps, int targetLoads, int targetStores, const KernelConfigMultiSeq& config) {
    const std::vector<bool> operations = operationOrder(totalOps, targetLoads, targetStores);
    const MultiSeqLayout layout = multiSeqLayout(config);
    const bool wideLoads = layout.loadBytes == 8;

    std::string ptxCode = "";
    int pauseBlockIndex = 0;

    const std::string LOAD_STR = wideLoads
        ? "                \"ld.global.cv.u64 loadedElem, [threadPointerLD];\"\n"
        : "                \"ld.global.cv.u32 loadedWord, [threadPointerLD];\"\n"
          "                \"cvt.u64.u32 loadedElem, loadedWord;\"\n";
    const std::string ADVANCE_LOAD_STR = "                \"add.u64 threadPointerLD, threadPointerLD, " +
        std::string(config.dependent_loads ? "loadedElem" : "loadStrideBytes") + ";\"\n";

    const std::string STORE_STR = storeCode(layout.storeBytes, "threadPointerST", "1024", "storeValue") +
        "                \"add.u64 threadPointerST, threadPointerST, storeStrideBytes;\"\n";

    LoadSinks sinks(wideLoads);
    for (size_t i = 0; i < operations.size(); ++i) {
        if (!operations[i]) {
            ptxCode += STORE_STR;
        } else {
            ptxCode += (config.dependent_loads ? LOAD_STR : sinks.load("threadPointerLD")) + ADVANCE_LOAD_STR;
        }

        if ((i + 1) % kOpsPerPauseBlock == 0) {
            ptxCode += sinks.consume();
            ptxCode += genPauseBlock(pauseBlockIndex++);
            ptxCode += "\n";
        }
    }
    ptxCode += sinks.consume();
    return ptxCode;
}

std::string GpuKernelGenerator::genFunctEnd(int loadOps, int storeOps, const KernelConfigMultiSeq& config) {
    const MultiSeqLayout layout = multiSeqLayout(config);
    std::string result;
    // Host allocations contain whole unrolled batches; change lane only on wrap.
    for (const auto& stream : {std::pair<std::string, int>{"LD", loadOps}, {"ST", storeOps}}) {
        if (stream.second == 0) continue;
        const auto& suffix = stream.first;
        const bool load = suffix == "LD";
        const std::string base = load ? "%2" : "%3";
        const std::string laneBytes = std::to_string(load ? layout.loadBytes : layout.storeBytes);
        const std::string slotBytes = std::to_string(load ? layout.loadSlotBytes : layout.storeSlotBytes);
        auto emit = [&](const std::string& instruction) { result += "        \"" + instruction + ";\"\n"; };
        emit("add.u64 number" + suffix + ", number" + suffix + ", " + std::to_string(stream.second));
        emit("setp.ge.u64 number" + suffix + "Pred, number" + suffix + ", sections" + suffix);
        emit("@number" + suffix + "Pred mov.u64 number" + suffix + ", 0");
        emit("@number" + suffix + "Pred add.u64 elemOffset" + suffix + ", elemOffset" + suffix + ", " + laneBytes);
        emit("setp.ge.u64 lane" + suffix + "Pred, elemOffset" + suffix + ", " + slotBytes);
        emit("@lane" + suffix + "Pred mov.u64 elemOffset" + suffix + ", 0");
        emit("@number" + suffix + "Pred add.u64 threadPointer" + suffix + ", " + base + ", elemOffset" + suffix);
    }
    result += R"(
        "@startFlagPred st.global.u32 [startFlagPointer], 1;"
        "@startFlagPred mov.pred startFlagPred, 0;"
        "ld.global.cv.u32 endFlagValue, [endFlagPointer];"
        "setp.ne.u32 endFlagPred, endFlagValue, endFlagTrue;"
        "mov.u64 firstNop, 0;"
        "@endFlagPred bra cache_line_loop;"
        "workload_done:"
        "mov.u64 %0, loopAccum;"
        "mov.u64 %1, loadedElem;"
        "}"
        : "=l"(loopAccum), "=l"(loadedElem)
        : "l"(threadPointer_LD), "l"(threadPointer_ST), "l"(pauseValue), "l"(loadStrideBytes),
          "l"(storeStrideBytes), "l"(endFlagPointer), "l"(startFlagPointer), "l"(sectionsLD),
          "l"(sectionsST), "l"(firstPause)
        : "memory"
    );
    if (threadIdx.x == 0) atomicAdd(result, static_cast<int>(loopAccum + loadedElem));
})";
    return result;
}
