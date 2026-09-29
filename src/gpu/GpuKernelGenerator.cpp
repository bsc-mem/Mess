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
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>

namespace {
constexpr int kPausePollThreshold = 256;
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

std::string GpuKernelGenerator::workload_file(int ratio_pct) {
    return "workload_" + std::to_string(ratio_pct) + ".cu";
}

std::string GpuKernelGenerator::generate_workload(int minTotalOps, int ratio_pct) {
    int totalOps = 0;
    int targetLoads = 0;
    int targetStores = 0;
    computeLoadStoreOps(minTotalOps, ratio_pct, totalOps, targetLoads, targetStores);

    const std::string ratio = std::to_string(ratio_pct);
    const std::string workload = "WorkloadRatio" + ratio;
    std::string result = "#include \"kernels.cuh\"\n\nstruct " + workload + " {\n";
    result += genFunctInit();
    result += loadStoreOpsGen(totalOps, targetLoads, targetStores);
    result += genFunctEnd(totalOps, targetLoads, targetStores);
    result += "\n};\n\nvoid launch_ratio_" + ratio + "(const MessArgs& args, int blocks, int threads) {\n"
              "    mess<" + workload + "><<<blocks, threads>>>(args);\n}\n\n"
              "int blocks_ratio_" + ratio + "(int threads) {\n"
              "    return resident_blocks(mess<" + workload + ">, threads);\n}\n";
    return result;
}

std::string GpuKernelGenerator::generate_workload_table(int minTotalOps, const std::vector<int>& ratios_pct, const std::vector<ExecutionMode>& modes) {
    std::string declarations;
    std::string entries;
    auto add = [&](const std::string& mode, const std::string& suffix, int ratio_pct) {
        int totalOps = 0;
        int loads = 0;
        int stores = 0;
        computeLoadStoreOps(minTotalOps, ratio_pct, totalOps, loads, stores);
        const std::string ratio = std::to_string(ratio_pct);
        declarations += "void launch_" + suffix + ratio + "(const MessArgs& args, int blocks, int threads);\n";
        declarations += "int blocks_" + suffix + ratio + "(int threads);\n";
        entries += "    {\"" + mode + "\", " + ratio + ", " + std::to_string(loads) + ", " + std::to_string(stores) +
                   ", launch_" + suffix + ratio + ", blocks_" + suffix + ratio + "},\n";
    };
    for (ExecutionMode mode : modes) {
        for (int ratio_pct : ratios_pct) {
            if (mode == ExecutionMode::MULTISEQUENTIAL) add("multiseq", "ratio_", ratio_pct);
        }
    }
    return "#include \"kernels.cuh\"\n\n" + declarations +
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
__device__ static void run(uint64_t elmSizeBytes, uint64_t threadPointer_LD, uint64_t threadPointer_ST,
        uint64_t pauseValue, uint64_t numElmPerThreadGroupLD, uint64_t numElmPerThreadGroupST,
        uint64_t endFlagPointer, uint64_t startFlagPointer, uint64_t sectionsLD,
        uint64_t sectionsST, uint64_t storeStrideBytes, uint64_t firstPause, int* result){

    uint64_t loopAccum = 0;
    uint64_t loadedElem = 0;

    asm volatile ( "{"

        ".reg .pred pausePred, linesPerSectPred, ElmPerThreadGroupSTPred, ElmPerThreadGroupLDPred, endFlagPred, startFlagPred, numberSTPred, numberLDPred;"
        ".reg .pred pollPred, endNowPred;"
        ".reg .u64 elmSizeBytes;"
        ".reg .u64 threadPointerLD;"
        ".reg .u64 threadPointerST;"
        ".reg .u64 loadedElem;"
        ".reg .u64 pauseValue, nopIter, loopAccum, firstNop;"
        ".reg .u64 numberLD, numberST;"
        ".reg .u64 numElmPerThreadGroupLD, ElmPerThreadGroupLDIter, elemOffsetLD;"
        ".reg .u64 numElmPerThreadGroupST, ElmPerThreadGroupSTIter, elemOffsetST;"
        ".reg .u64 endFlagPointer;"
        ".reg .u32 endFlagValue, endFlagTrue;"
        ".reg .u64 startFlagPointer;"
        ".reg .u64 sectionsLD, sectionsST, storeStrideBytes, groupBytesLD, groupBytesST;"
        "mov.u64 sectionsLD, %10;"
        "mov.u64 sectionsST, %11;"
        "mov.u64 storeStrideBytes, %12;"
        "mov.u64 firstNop, %13;"
        "mov.u64 loopAccum, 0;"
        "mov.u64 loadedElem, 0;"

        "mov.u64 elmSizeBytes, %2;"
        "mov.u64 threadPointerLD, %3;"
        "mov.u64 threadPointerST, %4;"
        "mov.u64 pauseValue, %5;"
        "mov.u64 numElmPerThreadGroupLD, %6;"
        "mov.u64 numElmPerThreadGroupST, %7;"
        "mov.u64 endFlagPointer, %8;"
        "mov.u64 startFlagPointer, %9;"
        "mul.lo.u64 groupBytesLD, numElmPerThreadGroupLD, elmSizeBytes;"
        "mul.lo.u64 groupBytesST, numElmPerThreadGroupST, elmSizeBytes;"

        "mov.u32 endFlagTrue, 1;"
        "mov.u32 endFlagValue, 0;"
        "mov.pred startFlagPred, 1;"
        "mov.pred endNowPred, 0;"
        "setp.ge.u64 pollPred, pauseValue, )" + std::to_string(kPausePollThreshold) + R"(;"

        "mov.u64 numberLD, 0;"
        "mov.u64 elemOffsetLD, 0;"
        "mov.u64 ElmPerThreadGroupLDIter, 0;"
        "mov.u64 numberST, 0;"
        "mov.u64 elemOffsetST, 0;"
        "mov.u64 ElmPerThreadGroupSTIter, 0;"

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
                ")" + label + R"(:"
                    "@pausePred add.u64 nopIter, nopIter, 1;"
                    "@pausePred setp.lt.u64 pausePred, nopIter, pauseValue;"
                    "@pausePred add.u64 loopAccum, loopAccum, nopIter;"
                    "@pausePred bra )" + label + R"(;"
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

std::string GpuKernelGenerator::loadStoreOpsGen(int totalOps, int targetLoads, int targetStores) {
    const std::vector<bool> operations = operationOrder(totalOps, targetLoads, targetStores);

    std::string ptxCode = "";
    int pauseBlockIndex = 0;

    const std::string LOAD_STR =
        "                \"ld.global.cv.u64 loadedElem, [threadPointerLD];\"\n"
        "                \"add.u64 threadPointerLD, threadPointerLD, loadedElem;\"\n";

    const std::string STORE_STR =
        "                \"st.global.wt.u64 [threadPointerST], 1024;\"\n"
        "                \"add.u64 threadPointerST, threadPointerST, storeStrideBytes;\"\n";

    for (size_t i = 0; i < operations.size(); ++i) {
        if (operations[i]) {
            ptxCode += LOAD_STR;
        } else {
            ptxCode += STORE_STR;
        }

        if ((i + 1) % 10 == 0) {
            ptxCode += genPauseBlock(pauseBlockIndex++);
            ptxCode += "\n";
        }
    }
    return ptxCode;
}

std::string GpuKernelGenerator::genFunctEnd(int, int loadOps, int storeOps) {
    std::string result;
    // Host allocations contain whole unrolled batches; change lane only on wrap.
    for (const auto& stream : {std::pair<std::string, int>{"LD", loadOps}, {"ST", storeOps}}) {
        if (stream.second == 0) continue;
        const auto& suffix = stream.first;
        const std::string base = suffix == "LD" ? "%3" : "%4";
        auto emit = [&](const std::string& instruction) { result += "        \"" + instruction + ";\"\n"; };
        emit("add.u64 number" + suffix + ", number" + suffix + ", " + std::to_string(stream.second));
        emit("setp.ge.u64 number" + suffix + "Pred, number" + suffix + ", sections" + suffix);
        emit("@number" + suffix + "Pred mov.u64 number" + suffix + ", 0");
        emit("@number" + suffix + "Pred add.u64 elemOffset" + suffix + ", elemOffset" + suffix + ", elmSizeBytes");
        emit("setp.ge.u64 ElmPerThreadGroup" + suffix + "Pred, elemOffset" + suffix + ", groupBytes" + suffix);
        emit("@ElmPerThreadGroup" + suffix + "Pred mov.u64 elemOffset" + suffix + ", 0");
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
        : "l"(elmSizeBytes), "l"(threadPointer_LD), "l"(threadPointer_ST), "l"(pauseValue),
          "l"(numElmPerThreadGroupLD), "l"(numElmPerThreadGroupST), "l"(endFlagPointer),
          "l"(startFlagPointer), "l"(sectionsLD), "l"(sectionsST), "l"(storeStrideBytes), "l"(firstPause)
        : "memory"
    );
    if (threadIdx.x == 0) atomicAdd(result, static_cast<int>(loopAccum + loadedElem));
})";
    return result;
}
