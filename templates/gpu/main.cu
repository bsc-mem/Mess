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

#include <cuda.h>
#include <cuda_runtime.h>
#include <getopt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "workloads.cuh"
#include "range_profiler.cuh"
#if __has_include(<cupti_events.h>) && __has_include(<cupti_metrics.h>)
#include "legacy_profiler.cuh"
#endif

namespace {

__global__ void link_chase(struct line* chase, const uint64_t* order, uint64_t count) {
    for (uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; i < count;
         i += (uint64_t)gridDim.x * blockDim.x) {
        const uint64_t next = order[(i + 1) % count];
        chase[order[i]].next = chase + next;
    }
}

template <class T>
__global__ void fill(T* values, uint64_t count, T value) {
    for (uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; i < count;
         i += (uint64_t)gridDim.x * blockDim.x)
        values[i] = value;
}

constexpr uint64_t kAllocationGranularity = 2ull << 20;
constexpr uint64_t kWarmUpWindowNs = 1000000;
constexpr uint64_t kMemoryReserve = 512ull << 20;
constexpr uint64_t kMinChaseBytes = 256ull << 20;

struct Options {
    uint64_t chaseBytes = 0;
    uint64_t iterations = 0;
    uint64_t windowNs = 0;
    uint64_t settleNs = 0;
    uint64_t minKernels = 0;
    uint64_t maxNs = 0;
    double precision = 0.0;
    std::vector<std::string> metrics;
};

struct Burst {
    uint64_t kernels = 0;
    uint64_t accesses = 0;
    uint64_t cycles = 0;
    uint64_t ns = 0;
    double sum = 0.0;
    double sumSquares = 0.0;

    void add(const ChaseSample& sample) {
        const double latency = static_cast<double>(sample.ns) / sample.accesses;
        ++kernels;
        accesses += sample.accesses;
        cycles += sample.cycles;
        ns += sample.ns;
        sum += latency;
        sumSquares += latency * latency;
    }

    bool done(const Options& options) const {
        if (kernels < options.minKernels || kernels % 2) return false;
        const double mean = sum / kernels;
        const double variance = std::max(0.0, sumSquares / kernels - mean * mean);
        return ns >= options.maxNs || std::sqrt(variance / (kernels - 1)) <= options.precision * mean;
    }
};

uint64_t parseCount(const std::string& text) {
    size_t end = 0;
    const unsigned long long value = std::stoull(text, &end);
    if (end != text.size() || text.front() == '-') throw std::invalid_argument("invalid count: " + text);
    return value;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    int opt;
    const char* flags = "s:w:t:d:k:T:e:m:"
        ;
    while ((opt = getopt(argc, argv, flags)) != -1) {
        switch (opt) {
            case 's': options.chaseBytes = parseCount(optarg); break;
            case 'w': options.iterations = parseCount(optarg); break;
            case 't': options.windowNs = parseCount(optarg); break;
            case 'd': options.settleNs = parseCount(optarg); break;
            case 'k': options.minKernels = parseCount(optarg); break;
            case 'T': options.maxNs = parseCount(optarg); break;
            case 'e': options.precision = std::stod(optarg); break;
            case 'm': {
                std::istringstream list(optarg);
                for (std::string metric; std::getline(list, metric, ',');)
                    if (!metric.empty()) options.metrics.push_back(metric);
                break;
            }
            default: throw std::invalid_argument("unknown option");
        }
    }
    if (optind != argc || (options.chaseBytes && options.chaseBytes < CACHE_LINE) || !options.iterations || !options.windowNs ||
        options.minKernels < 2 || options.maxNs < options.windowNs || options.precision <= 0.0 || options.metrics.empty())
        throw std::invalid_argument("usage: main [-s <chase bytes>] -w <iterations> -t <kernel window ns> -d <settle ns> "
                                    "-k <min kernels> -T <max chase ns> -e <relative error> -m <metrics>");
    return options;
}

uint64_t chaseBytesFor(const Options& options) {
    if (options.chaseBytes) return options.chaseBytes;
    int l2Bytes = 0;
    CUDA_CHECK(cudaDeviceGetAttribute(&l2Bytes, cudaDevAttrL2CacheSize, 0));
    uint64_t bytes = kMinChaseBytes;
    while (bytes < 4ull * static_cast<uint64_t>(l2Bytes)) bytes *= 2;
    return bytes;
}

uint64_t roundUp(uint64_t value, uint64_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

uint64_t sectionsFor(uint64_t iterations, uint64_t operations) {
    return operations == 0 ? 1 : roundUp(iterations, operations);
}

std::vector<uint64_t> chaseOrder(uint64_t count) {
    std::vector<uint64_t> order(count);
    for (uint64_t i = 0; i < count; ++i) order[i] = i;
    std::mt19937_64 rng(0);
    for (uint64_t i = count - 1; i > 0; --i) {
        std::uniform_int_distribution<uint64_t> pick(0, i);
        std::swap(order[i], order[pick(rng)]);
    }
    return order;
}

template <class T>
class DeviceBuffer {
public:
    explicit DeviceBuffer(uint64_t count) { CUDA_CHECK(cudaMalloc(&data_, count * sizeof(T))); }
    ~DeviceBuffer() { cudaFree(data_); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    T* get() const { return data_; }

private:
    T* data_ = nullptr;
};

struct Streams {
    Streams(int count, uint64_t bytes)
        : stride(roundUp(bytes, kAllocationGranularity) / sizeof(dataType)),
          values(count * stride), table(count) {
        std::vector<dataType*> pointers(count);
        for (int i = 0; i < count; ++i) pointers[i] = values.get() + i * stride;
        CUDA_CHECK(cudaMemcpy(table.get(), pointers.data(), count * sizeof(dataType*), cudaMemcpyHostToDevice));
    }

    uint64_t stride;
    DeviceBuffer<dataType> values;
    DeviceBuffer<dataType*> table;
};


const WorkloadVariant& findWorkload(const std::string& mode, int ratio) {
    for (const auto& workload : kWorkloads)
        if (workload.ratio == ratio && mode == workload.mode) return workload;
    throw std::invalid_argument("no " + mode + " workload compiled for ratio " + std::to_string(ratio));
}


uint64_t maxSections(uint64_t iterations, uint64_t WorkloadVariant::*operations) {
    uint64_t result = 1;
    for (const auto& workload : kWorkloads) result = std::max(result, sectionsFor(iterations, workload.*operations));
    return result;
}

struct Grid {
    int threads = 0;
    int blocksPerSm = 0;
    int blocks(const cudaDeviceProp& device) const { return device.multiProcessorCount * blocksPerSm; }
};

Grid residentGrid(const cudaDeviceProp& device) {
    Grid best;
    for (int threads = device.maxThreadsPerBlock; threads >= 128; threads /= 2) {
        int blocksPerSm = device.maxThreadsPerMultiProcessor / threads;
        for (const auto& workload : kWorkloads) blocksPerSm = std::min(blocksPerSm, workload.residentBlocks(threads));
        if (blocksPerSm * threads > best.blocksPerSm * best.threads) best = {threads, blocksPerSm};
    }
    if (!best.blocksPerSm) throw std::runtime_error("no workload kernel fits on a streaming multiprocessor");
    return best;
}

uint64_t streamBytes(int blocks, uint64_t bytes) {
    return blocks * roundUp(bytes, kAllocationGranularity);
}

void checkMemory(const cudaDeviceProp& device, const Grid& grid, const Options& options) {
    const int blocks = grid.blocks(device);
    uint64_t needed = kMemoryReserve + roundUp(options.chaseBytes, kAllocationGranularity) +
                      streamBytes(blocks, grid.threads * MESS_LOAD_SLOT_BYTES * maxSections(options.iterations, &WorkloadVariant::loadOps)) +
                      streamBytes(blocks, grid.threads * MESS_STORE_SLOT_BYTES * maxSections(options.iterations, &WorkloadVariant::storeOps));
    size_t free = 0, total = 0;
    CUDA_CHECK(cudaMemGetInfo(&free, &total));
    if (needed > free)
        throw std::runtime_error("GPU memory: need " + std::to_string(needed >> 20) + " MiB, only " +
                                 std::to_string(free >> 20) + " MiB free");
}

class Workload {
public:
    Workload(const cudaDeviceProp& device, const Grid& grid, const Options& options)
        : sms_(device.multiProcessorCount), threads_(grid.threads),
          blocks_(grid.blocks(device)),
          loadSlotBytes_(MESS_LOAD_SLOT_BYTES),
          storeSlotBytes_(MESS_STORE_SLOT_BYTES),
          iterations_(options.iterations),
          settleNs_(options.settleNs),
          lines_(options.chaseBytes / CACHE_LINE),
          loads_(blocks_, threads_ * loadSlotBytes_ * maxSections(iterations_, &WorkloadVariant::loadOps)),
          stores_(blocks_, threads_ * storeSlotBytes_ * maxSections(iterations_, &WorkloadVariant::storeOps)),
          chase_(lines_), sample_(1), flags_(1), result_(1)
          {
        const int initBlocks = device.multiProcessorCount * 8;
        using LoadWord = std::conditional_t<MESS_LOAD_BYTES == 4, uint32_t, uint64_t>;
        fill<<<initBlocks, threads_>>>(reinterpret_cast<LoadWord*>(loads_.values.get()),
                                       blocks_ * loads_.stride * sizeof(dataType) / sizeof(LoadWord),
                                       static_cast<LoadWord>(threads_ * loadSlotBytes_));
        CUDA_CHECK(cudaGetLastError());

        const std::vector<uint64_t> order = chaseOrder(lines_);
        DeviceBuffer<uint64_t> deviceOrder(lines_);
        CUDA_CHECK(cudaMemcpy(deviceOrder.get(), order.data(), lines_ * sizeof(uint64_t), cudaMemcpyHostToDevice));
        link_chase<<<initBlocks, threads_>>>(chase_.get(), deviceOrder.get(), lines_);
        CUDA_CHECK(cudaGetLastError());

        const ChaseSample start{(uint64_t)chase_.get()};
        CUDA_CHECK(cudaMemcpy(sample_.get(), &start, sizeof(start), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemset(result_.get(), 0, sizeof(int)));
        resetFlags();
    }

    void launch(Profiler* profiler, const WorkloadVariant& workload, uint64_t pause, uint64_t windowNs
                ) {
        const MessArgs args{chase_.get(), sample_.get(), windowNs, settleNs_, flags_.get(),
                            loads_.table.get(), loadSlotBytes_, stores_.table.get(), storeSlotBytes_, pause,
                            sectionsFor(iterations_, workload.loadOps),
                            sectionsFor(iterations_, workload.storeOps), result_.get(),
                            };
        if (profiler) profiler->begin_kernel();
        workload.launch(args, blocks_, threads_);
        CUDA_CHECK(cudaGetLastError());
        if (profiler) profiler->end_kernel();
        CUDA_CHECK(cudaDeviceSynchronize());
    }

    void warmUp() {
        for (const auto& workload : kWorkloads) {
            launch(nullptr, workload, 0, kWarmUpWindowNs);
            collect();
        }
    }

    int blocks() const { return blocks_; }
    int threads() const { return threads_; }

    ChaseSample collect() {
        ChaseSample sample{};
        CUDA_CHECK(cudaMemcpy(&sample, sample_.get(), sizeof(sample), cudaMemcpyDeviceToHost));
        resetFlags();
        if (sample.accesses == 0 || sample.ns == 0 || sample.cycles == 0)
            throw std::runtime_error("pointer chase produced no timing");
        return sample;
    }

private:
    void resetFlags() {
        const Flags cleared{};
        CUDA_CHECK(cudaMemcpy(flags_.get(), &cleared, sizeof(Flags), cudaMemcpyHostToDevice));
    }

    int sms_;
    int threads_;
    int blocks_;
    uint64_t loadSlotBytes_;
    uint64_t storeSlotBytes_;
    uint64_t iterations_;
    uint64_t settleNs_;
    uint64_t lines_;
    Streams loads_;
    Streams stores_;
    DeviceBuffer<struct line> chase_;
    DeviceBuffer<ChaseSample> sample_;
    DeviceBuffer<Flags> flags_;
    DeviceBuffer<int> result_;
};

}

int main(int argc, char** argv) {
    try {
        Options options = parseOptions(argc, argv);
        CUDA_CHECK(cudaSetDevice(0));
        CUDA_CHECK(cudaFree(nullptr));
        CUcontext context = nullptr;
        if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context) throw std::runtime_error("no CUDA context");
        cudaDeviceProp device{};
        CUDA_CHECK(cudaGetDeviceProperties(&device, 0));
        int memoryClockKHz = 0, busWidthBits = 0;
        CUDA_CHECK(cudaDeviceGetAttribute(&memoryClockKHz, cudaDevAttrMemoryClockRate, 0));
        CUDA_CHECK(cudaDeviceGetAttribute(&busWidthBits, cudaDevAttrGlobalMemoryBusWidth, 0));

        options.chaseBytes = chaseBytesFor(options);
        const Grid grid = residentGrid(device);
        checkMemory(device, grid, options);
        Workload workload(device, grid, options);
        workload.warmUp();
        std::unique_ptr<Profiler> profiler;
        if (device.major >= 7) {
            profiler = std::make_unique<RangeProfiler>(context, 0, options.metrics);
        } else {
#if __has_include(<cupti_events.h>) && __has_include(<cupti_metrics.h>)
            profiler = std::make_unique<LegacyProfiler>(context, 0, options.metrics);
#else
            throw std::runtime_error("pre-Volta GPUs require CUDA Toolkit 12.6 through 12.9 with legacy CUPTI APIs");
#endif
        }

        std::printf("READY sms=%d blocks=%d threads=%d passes=%zu memory_clock_mhz=%d bus_width_bits=%d chase_bytes=%llu workloads=",
                    device.multiProcessorCount, workload.blocks(), workload.threads(), profiler->passes(),
                    memoryClockKHz / 1000, busWidthBits, (unsigned long long)options.chaseBytes);
        for (size_t i = 0; i < std::size(kWorkloads); ++i)
            std::printf("%s%s:%d", i ? "," : "", kWorkloads[i].mode, kWorkloads[i].ratio);
        std::printf("\n");
        std::fflush(stdout);

        for (std::string request; std::getline(std::cin, request);) {
            std::istringstream fields(request);
            std::string mode, ratioText, pauseText, extra;
            if (!(fields >> mode >> ratioText >> pauseText))
                throw std::invalid_argument("expected '<mode> <ratio> <pause>', got '" + request + "'");
            if (fields >> extra) throw std::invalid_argument("unexpected GPU sample arguments");
            const WorkloadVariant& target = findWorkload(mode, static_cast<int>(parseCount(ratioText)));
            const uint64_t pause = parseCount(pauseText);
            Burst burst;
            const std::vector<double> values = profiler->measure([&](size_t pass) {
                Burst current;
                do {
                    workload.launch(profiler.get(), target, pause, options.windowNs
                                    );
                    current.add(workload.collect());
                } while (!current.done(options));
                if (pass == 0) burst = current;
            });
            std::printf("SAMPLE ratio=%d pause=%llu kernels=%llu accesses=%llu cycles=%llu ns=%llu", target.ratio,
                        (unsigned long long)pause, (unsigned long long)burst.kernels, (unsigned long long)burst.accesses,
                        (unsigned long long)burst.cycles, (unsigned long long)burst.ns);
            for (size_t i = 0; i < values.size(); ++i)
                std::printf(" %s=%.17g", profiler->metrics()[i].c_str(), values[i]);
            std::printf("\n");
            std::fflush(stdout);
        }
        return 0;
    } catch (const std::exception& error) {
        std::printf("ERROR %s\n", error.what());
        std::fflush(stdout);
        return 1;
    }
}
