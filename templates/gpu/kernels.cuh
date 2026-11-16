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

#ifndef MESS_GPU_KERNELS_CUH
#define MESS_GPU_KERNELS_CUH

#include <cstdint>

#define CACHE_LINE 128
#define dataType uint64_t

struct line {
    struct line *next;
    uint8_t pad[CACHE_LINE - 8];
};

struct ChaseSample {
    uint64_t cursor;
    uint64_t accesses;
    uint64_t cycles;
    uint64_t ns;
};

struct Flags {
    alignas(CACHE_LINE) int start;
    alignas(CACHE_LINE) int end;
    alignas(CACHE_LINE) unsigned chaserSm;
};

struct MessArgs {
    struct line* chase;
    ChaseSample* sample;
    uint64_t windowNs;
    uint64_t settleNs;
    Flags* flags;
    dataType** streamsLD;
    uint64_t loadSlotBytes;
    dataType** streamsST;
    uint64_t storeSlotBytes;
    uint64_t pause;
    uint64_t sectionsLD;
    uint64_t sectionsST;
    int* result;
};

using MessLaunch = void (*)(const MessArgs& args, int blocks, int threads);
using ResidentBlocks = int (*)(int threads);

struct WorkloadVariant {
    const char* mode;
    int ratio;
    uint64_t loadOps;
    uint64_t storeOps;
    MessLaunch launch;
    ResidentBlocks residentBlocks;
};

template <class Kernel>
int resident_blocks(Kernel kernel, int threads) {
    int blocks = 0;
    return cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, kernel, threads, 0) == cudaSuccess ? blocks : 0;
}

#include "loopGPU.h"

static __device__ uint64_t global_timer() {
    uint64_t ns;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(ns));
    return ns;
}

static __device__ void pointer_chase(struct line* chase, ChaseSample* sample, uint64_t window_ns) {
    uint64_t next = sample->cursor;
    uint64_t address = 0;
    uint64_t blocks = 0;
    uint64_t start_ns, end_ns;
    const long long start_clock = clock64();
    asm volatile(
        "{\n\t"
        ".reg .pred more;\n\t"
        ".reg .u64 deadline;\n\t"
        "mov.u64 %3, %%globaltimer;\n\t"
        "add.u64 deadline, %3, %6;\n\t"
        "chase_block:\n\t"
        GPU_PTR_CHASE_LOOP
        "add.u64 %1, %5, %0;\n\t"
        "add.u64 %2, %2, 1;\n\t"
        "mov.u64 %4, %%globaltimer;\n\t"
        "setp.lt.u64 more, %4, deadline;\n\t"
        "@more bra.uni chase_block;\n\t"
        "}"
        : "+l"(next), "+l"(address), "+l"(blocks), "=&l"(start_ns), "=&l"(end_ns)
        : "l"(chase), "l"(window_ns)
        : "memory");
    const long long end_clock = clock64();
    sample->cursor = next;
    sample->accesses = blocks * PTR_CHASE_LOOP_SIZE;
    sample->cycles = end_clock - start_clock;
    sample->ns = end_ns - start_ns;
}

static __device__ bool chase_on_this_sm(const MessArgs& args) {
    __shared__ bool onChaserSm, chaser;
    if (threadIdx.x == 0) {
        unsigned sm;
        asm volatile("mov.u32 %0, %%smid;" : "=r"(sm));
        const unsigned elected = atomicCAS(&args.flags->chaserSm, 0u, sm + 1);
        chaser = elected == 0;
        onChaserSm = chaser || elected == sm + 1;
    }
    __syncthreads();
    if (!onChaserSm) return false;
    if (chaser && threadIdx.x == 0) {
        while (*(volatile int*)&args.flags->start == 0) {}
        for (const uint64_t settled = global_timer() + args.settleNs; global_timer() < settled;) {}
        pointer_chase(args.chase, args.sample, args.windowNs);
        *(volatile int*)&args.flags->end = 1;
        __threadfence();
    }
    return true;
}

static __device__ uint64_t first_pause(const MessArgs& args, uint64_t thread) {
    return args.pause ? (thread * 0x9E3779B97F4A7C15ull >> 32) % args.pause : 0;
}

template <class Workload>
static __device__ void run_multiseq(const MessArgs& args) {
    const uint64_t threadPointer_LD = (uint64_t)args.streamsLD[blockIdx.x] + threadIdx.x * args.loadSlotBytes;
    const uint64_t threadPointer_ST = (uint64_t)args.streamsST[blockIdx.x] + threadIdx.x * args.storeSlotBytes;
    const uint64_t thread = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    const uint64_t firstPause = first_pause(args, thread);
    Workload::run(threadPointer_LD, threadPointer_ST, args.pause, blockDim.x * args.loadSlotBytes,
                  blockDim.x * args.storeSlotBytes, (uint64_t)&args.flags->end, (uint64_t)&args.flags->start,
                  args.sectionsLD, args.sectionsST, firstPause, args.result);
}

template <class Workload>
__global__ void mess(const MessArgs args) {
    if (chase_on_this_sm(args)) return;
    run_multiseq<Workload>(args);
}


#endif
