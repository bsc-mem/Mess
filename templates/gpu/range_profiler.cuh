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

#ifndef MESS_GPU_RANGE_PROFILER_CUH
#define MESS_GPU_RANGE_PROFILER_CUH

#include <cuda.h>
#include <cupti_profiler_host.h>
#include <cupti_profiler_target.h>
#include <cupti_range_profiler.h>
#include <cupti_target.h>
#include <cstdint>
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

class RangeProfiler {
    static constexpr size_t kMaxRanges = 4;

public:
    RangeProfiler(CUcontext context, int device, const std::vector<std::string>& metrics)
        : names_(metrics) {
        for (const auto& name : names_) metrics_.push_back(name.c_str());

        CUpti_Profiler_Initialize_Params initialize{CUpti_Profiler_Initialize_Params_STRUCT_SIZE};
        CUPTI_CHECK(cuptiProfilerInitialize(&initialize));
        initialized_ = true;

        CUpti_Device_GetChipName_Params chip{CUpti_Device_GetChipName_Params_STRUCT_SIZE};
        chip.deviceIndex = device;
        CUPTI_CHECK(cuptiDeviceGetChipName(&chip));

        CUpti_Profiler_GetCounterAvailability_Params availability{
            CUpti_Profiler_GetCounterAvailability_Params_STRUCT_SIZE};
        availability.ctx = context;
        CUPTI_CHECK(cuptiProfilerGetCounterAvailability(&availability));
        std::vector<uint8_t> availabilityImage(availability.counterAvailabilityImageSize);
        availability.pCounterAvailabilityImage = availabilityImage.data();
        CUPTI_CHECK(cuptiProfilerGetCounterAvailability(&availability));

        CUpti_Profiler_Host_Initialize_Params host{CUpti_Profiler_Host_Initialize_Params_STRUCT_SIZE};
        host.profilerType = CUPTI_PROFILER_TYPE_RANGE_PROFILER;
        host.pChipName = chip.pChipName;
        host.pCounterAvailabilityImage = availabilityImage.data();
        CUPTI_CHECK(cuptiProfilerHostInitialize(&host));
        host_ = host.pHostObject;

        CUpti_Profiler_Host_ConfigAddMetrics_Params add{CUpti_Profiler_Host_ConfigAddMetrics_Params_STRUCT_SIZE};
        add.pHostObject = host_;
        add.ppMetricNames = metrics_.data();
        add.numMetrics = metrics_.size();
        CUPTI_CHECK(cuptiProfilerHostConfigAddMetrics(&add));

        CUpti_Profiler_Host_GetConfigImageSize_Params size{
            CUpti_Profiler_Host_GetConfigImageSize_Params_STRUCT_SIZE};
        size.pHostObject = host_;
        CUPTI_CHECK(cuptiProfilerHostGetConfigImageSize(&size));
        config_.resize(size.configImageSize);
        CUpti_Profiler_Host_GetConfigImage_Params image{CUpti_Profiler_Host_GetConfigImage_Params_STRUCT_SIZE};
        image.pHostObject = host_;
        image.pConfigImage = config_.data();
        image.configImageSize = config_.size();
        CUPTI_CHECK(cuptiProfilerHostGetConfigImage(&image));

        CUpti_Profiler_Host_GetNumOfPasses_Params passes{CUpti_Profiler_Host_GetNumOfPasses_Params_STRUCT_SIZE};
        passes.pConfigImage = config_.data();
        passes.configImageSize = config_.size();
        CUPTI_CHECK(cuptiProfilerHostGetNumOfPasses(&passes));
        passes_ = passes.numOfPasses;

        CUpti_RangeProfiler_Enable_Params enable{CUpti_RangeProfiler_Enable_Params_STRUCT_SIZE};
        enable.ctx = context;
        CUPTI_CHECK(cuptiRangeProfilerEnable(&enable));
        target_ = enable.pRangeProfilerObject;

        CUpti_RangeProfiler_GetCounterDataSize_Params data{
            CUpti_RangeProfiler_GetCounterDataSize_Params_STRUCT_SIZE};
        data.pRangeProfilerObject = target_;
        data.pMetricNames = metrics_.data();
        data.numMetrics = metrics_.size();
        data.maxNumOfRanges = kMaxRanges;
        data.maxNumRangeTreeNodes = kMaxRanges;
        CUPTI_CHECK(cuptiRangeProfilerGetCounterDataSize(&data));
        counterData_.resize(data.counterDataSize);
    }

    ~RangeProfiler() {
        if (target_) {
            CUpti_RangeProfiler_Disable_Params disable{CUpti_RangeProfiler_Disable_Params_STRUCT_SIZE};
            disable.pRangeProfilerObject = target_;
            cuptiRangeProfilerDisable(&disable);
        }
        if (host_) {
            CUpti_Profiler_Host_Deinitialize_Params host{CUpti_Profiler_Host_Deinitialize_Params_STRUCT_SIZE};
            host.pHostObject = host_;
            cuptiProfilerHostDeinitialize(&host);
        }
        if (initialized_) {
            CUpti_Profiler_DeInitialize_Params deinitialize{CUpti_Profiler_DeInitialize_Params_STRUCT_SIZE};
            cuptiProfilerDeInitialize(&deinitialize);
        }
    }

    RangeProfiler(const RangeProfiler&) = delete;
    RangeProfiler& operator=(const RangeProfiler&) = delete;

    size_t passes() const { return passes_; }
    const std::vector<std::string>& metrics() const { return names_; }

    template <class Run>
    std::vector<double> measure(Run run) {
        CUpti_RangeProfiler_CounterDataImage_Initialize_Params reset{
            CUpti_RangeProfiler_CounterDataImage_Initialize_Params_STRUCT_SIZE};
        reset.pRangeProfilerObject = target_;
        reset.pCounterData = counterData_.data();
        reset.counterDataSize = counterData_.size();
        CUPTI_CHECK(cuptiRangeProfilerCounterDataImageInitialize(&reset));

        CUpti_RangeProfiler_SetConfig_Params config{CUpti_RangeProfiler_SetConfig_Params_STRUCT_SIZE};
        config.pRangeProfilerObject = target_;
        config.pConfig = config_.data();
        config.configSize = config_.size();
        config.pCounterDataImage = counterData_.data();
        config.counterDataImageSize = counterData_.size();
        config.range = CUPTI_UserRange;
        config.replayMode = CUPTI_UserReplay;
        config.maxRangesPerPass = kMaxRanges;
        config.numNestingLevels = 1;
        config.minNestingLevel = 1;
        config.passIndex = 0;
        config.targetNestingLevel = 1;
        CUPTI_CHECK(cuptiRangeProfilerSetConfig(&config));

        bool submitted = false;
        for (size_t pass = 0; !submitted; ++pass) {
            CUpti_RangeProfiler_Start_Params start{CUpti_RangeProfiler_Start_Params_STRUCT_SIZE};
            start.pRangeProfilerObject = target_;
            CUPTI_CHECK(cuptiRangeProfilerStart(&start));
            CUpti_RangeProfiler_PushRange_Params push{CUpti_RangeProfiler_PushRange_Params_STRUCT_SIZE};
            push.pRangeProfilerObject = target_;
            push.pRangeName = "mess";
            CUPTI_CHECK(cuptiRangeProfilerPushRange(&push));
            run(pass);
            CUpti_RangeProfiler_PopRange_Params pop{CUpti_RangeProfiler_PopRange_Params_STRUCT_SIZE};
            pop.pRangeProfilerObject = target_;
            CUPTI_CHECK(cuptiRangeProfilerPopRange(&pop));
            CUpti_RangeProfiler_Stop_Params stop{CUpti_RangeProfiler_Stop_Params_STRUCT_SIZE};
            stop.pRangeProfilerObject = target_;
            CUPTI_CHECK(cuptiRangeProfilerStop(&stop));
            submitted = stop.isAllPassSubmitted;
        }

        CUpti_RangeProfiler_DecodeData_Params decode{CUpti_RangeProfiler_DecodeData_Params_STRUCT_SIZE};
        decode.pRangeProfilerObject = target_;
        CUPTI_CHECK(cuptiRangeProfilerDecodeData(&decode));

        CUpti_RangeProfiler_GetCounterDataInfo_Params info{
            CUpti_RangeProfiler_GetCounterDataInfo_Params_STRUCT_SIZE};
        info.pCounterDataImage = counterData_.data();
        info.counterDataImageSize = counterData_.size();
        CUPTI_CHECK(cuptiRangeProfilerGetCounterDataInfo(&info));
        if (info.numTotalRanges != 1)
            throw std::runtime_error("expected one profiled range, got " + std::to_string(info.numTotalRanges));

        std::vector<double> values(metrics_.size());
        CUpti_Profiler_Host_EvaluateToGpuValues_Params evaluate{
            CUpti_Profiler_Host_EvaluateToGpuValues_Params_STRUCT_SIZE};
        evaluate.pHostObject = host_;
        evaluate.pCounterDataImage = counterData_.data();
        evaluate.counterDataImageSize = counterData_.size();
        evaluate.ppMetricNames = metrics_.data();
        evaluate.numMetrics = metrics_.size();
        evaluate.rangeIndex = 0;
        evaluate.pMetricValues = values.data();
        CUPTI_CHECK(cuptiProfilerHostEvaluateToGpuValues(&evaluate));
        return values;
    }

private:
    std::vector<std::string> names_;
    std::vector<const char*> metrics_;
    std::vector<uint8_t> config_;
    std::vector<uint8_t> counterData_;
    CUpti_Profiler_Host_Object* host_ = nullptr;
    CUpti_RangeProfiler_Object* target_ = nullptr;
    size_t passes_ = 0;
    bool initialized_ = false;
};

#endif
