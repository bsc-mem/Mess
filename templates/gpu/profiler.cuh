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

#ifndef MESS_GPU_PROFILER_CUH
#define MESS_GPU_PROFILER_CUH

#include <cuda_runtime.h>
#include <cupti.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

inline void checkCupti(CUptiResult status, const char* call) {
    if (status == CUPTI_SUCCESS) return;
    const char* message = nullptr;
    cuptiGetResultString(status, &message);
    std::string error = std::string(call) + ": " + (message ? message : "unknown CUPTI error");
    if (status == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES)
        error += " (GPU performance counters are restricted to administrators; see NVreg_RestrictProfilingToAdminUsers)";
    throw std::runtime_error(error);
}

#define CUPTI_CHECK(call) checkCupti((call), #call)

inline void checkCuda(cudaError_t status, const char* call) {
    if (status != cudaSuccess) throw std::runtime_error(std::string(call) + ": " + cudaGetErrorString(status));
}

#define CUDA_CHECK(call) checkCuda((call), #call)

class Profiler {
public:
    explicit Profiler(const std::vector<std::string>& names) : names_(names) {}
    virtual ~Profiler() = default;
    virtual size_t passes() const = 0;
    const std::vector<std::string>& metrics() const { return names_; }
    virtual std::vector<double> measure(const std::function<void(size_t)>& run) = 0;
    virtual void begin_kernel() {}
    virtual void end_kernel() {}

protected:
    std::vector<std::string> names_;
};

#endif
