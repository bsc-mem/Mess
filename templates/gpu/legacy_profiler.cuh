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

#ifndef MESS_GPU_LEGACY_PROFILER_CUH
#define MESS_GPU_LEGACY_PROFILER_CUH

#include "profiler.cuh"
#include <cuda_runtime.h>
#include <cupti_events.h>
#include <cupti_metrics.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>

class LegacyProfiler : public Profiler {
    struct Event {
        cudaEvent_t handle = nullptr;
        Event() { CUDA_CHECK(cudaEventCreate(&handle)); }
        ~Event() { cudaEventDestroy(handle); }
    };

    struct Metric {
        CUpti_MetricID id = 0;
        CUpti_MetricValueKind kind{};
        std::vector<CUpti_EventID> events;
        bool raw = false;
        bool average = false;
        bool rate = false;
    };

public:
    LegacyProfiler(CUcontext context, int device, const std::vector<std::string>& names)
        : Profiler(names), device_(device) {
        std::vector<CUpti_EventID> events;
        for (const auto& name : names_) {
            Metric metric;
            std::string native = name;
            if (name == "dram__bytes_read.sum.per_second") native = "dram_read_throughput";
            if (name == "dram__bytes_write.sum.per_second") native = "dram_write_throughput";
            if (name == "sm__cycles_elapsed.avg" || name == "sm__cycles_elapsed.avg.per_second") {
                native = "elapsed_cycles_sm";
                metric.average = true;
                metric.rate = name == "sm__cycles_elapsed.avg.per_second";
            }
            const auto status = cuptiMetricGetIdFromName(device_, native.c_str(), &metric.id);
            if (status == CUPTI_ERROR_INVALID_METRIC_NAME) {
                metric.raw = true;
                metric.events.resize(1);
                checkCupti(cuptiEventGetIdFromName(device_, native.c_str(), metric.events.data()), name.c_str());
            } else {
                checkCupti(status, name.c_str());
                size_t size = sizeof(metric.kind);
                CUPTI_CHECK(cuptiMetricGetAttribute(metric.id, CUPTI_METRIC_ATTR_VALUE_KIND, &size, &metric.kind));
                uint32_t count = 0;
                CUPTI_CHECK(cuptiMetricGetNumEvents(metric.id, &count));
                metric.events.resize(count);
                size = count * sizeof(CUpti_EventID);
                CUPTI_CHECK(cuptiMetricEnumEvents(metric.id, &size, metric.events.data()));
            }
            for (auto event : metric.events)
                if (std::find(events.begin(), events.end(), event) == events.end()) events.push_back(event);
            metrics_.push_back(std::move(metric));
        }
        CUPTI_CHECK(cuptiSetEventCollectionMode(context, CUPTI_EVENT_COLLECTION_MODE_KERNEL));
        CUpti_EventGroupSets* sets = nullptr;
        CUPTI_CHECK(cuptiEventGroupSetsCreate(context, events.size() * sizeof(CUpti_EventID), events.data(), &sets));
        sets_.reset(sets);
        for (size_t pass = 0; pass < passes(); ++pass) {
            auto& set = sets_->sets[pass];
            for (uint32_t i = 0; i < set.numEventGroups; ++i) {
                uint32_t all = 1;
                CUPTI_CHECK(cuptiEventGroupSetAttribute(set.eventGroups[i], CUPTI_EVENT_GROUP_ATTR_PROFILE_ALL_DOMAIN_INSTANCES,
                                                       sizeof(all), &all));
            }
        }
    }

    size_t passes() const override { return sets_->numSets; }

    void begin_kernel() override {
        CUPTI_CHECK(cuptiEventGroupSetEnable(&sets_->sets[pass_]));
        active_ = true;
        CUDA_CHECK(cudaEventRecord(start_.handle));
    }

    void end_kernel() override {
        CUDA_CHECK(cudaEventRecord(stop_.handle));
        CUDA_CHECK(cudaEventSynchronize(stop_.handle));
        float milliseconds = 0;
        CUDA_CHECK(cudaEventElapsedTime(&milliseconds, start_.handle, stop_.handle));
        passNs_ += milliseconds * 1e6;
        const auto& set = sets_->sets[pass_];
        for (uint32_t i = 0; i < set.numEventGroups; ++i) {
            const auto group = set.eventGroups[i];
            CUpti_EventDomainID domain;
            uint32_t instances = 0, total = 0, count = 0;
            size_t size = sizeof(domain);
            CUPTI_CHECK(cuptiEventGroupGetAttribute(group, CUPTI_EVENT_GROUP_ATTR_EVENT_DOMAIN_ID, &size, &domain));
            size = sizeof(total);
            CUPTI_CHECK(cuptiDeviceGetEventDomainAttribute(device_, domain, CUPTI_EVENT_DOMAIN_ATTR_TOTAL_INSTANCE_COUNT, &size, &total));
            size = sizeof(instances);
            CUPTI_CHECK(cuptiEventGroupGetAttribute(group, CUPTI_EVENT_GROUP_ATTR_INSTANCE_COUNT, &size, &instances));
            size = sizeof(count);
            CUPTI_CHECK(cuptiEventGroupGetAttribute(group, CUPTI_EVENT_GROUP_ATTR_NUM_EVENTS, &size, &count));
            if (!instances || !total) throw std::runtime_error("CUPTI event domain has no instances");
            std::vector<CUpti_EventID> events(count);
            size = events.size() * sizeof(CUpti_EventID);
            CUPTI_CHECK(cuptiEventGroupGetAttribute(group, CUPTI_EVENT_GROUP_ATTR_EVENTS, &size, events.data()));
            std::vector<uint64_t> values(instances);
            for (auto event : events) {
                size = values.size() * sizeof(uint64_t);
                CUPTI_CHECK(cuptiEventGroupReadEvent(group, CUPTI_EVENT_READ_FLAG_NONE, event, &size, values.data()));
                for (size_t instance = 0; instance < values.size(); ++instance) {
                    const auto value = values[instance];
                    if (value == CUPTI_EVENT_OVERFLOW || value == CUPTI_EVENT_INVALID) {
                        char name[256]{};
                        size_t nameSize = sizeof(name) - 1;
                        cuptiEventGetAttribute(event, CUPTI_EVENT_ATTR_NAME, &nameSize, name);
                        throw std::runtime_error("CUPTI counter " + std::string(name) + " (event " + std::to_string(event) +
                            ", instance " + std::to_string(instance) + ") " +
                            (value == CUPTI_EVENT_OVERFLOW ? "overflowed" : "is invalid") +
                            " after " + std::to_string(milliseconds) + " ms");
                    }
                }
                passValues_[event] += std::accumulate(values.begin(), values.end(), 0.0) * total / instances;
                instances_[event] = total;
            }
        }
        CUPTI_CHECK(cuptiEventGroupSetDisable(&sets_->sets[pass_]));
        active_ = false;
    }

    std::vector<double> measure(const std::function<void(size_t)>& run) override {
        std::map<CUpti_EventID, uint64_t> values;
        double duration = 0;
        for (pass_ = 0; pass_ < passes(); ++pass_) {
            passValues_.clear();
            passNs_ = 0;
            auto& set = sets_->sets[pass_];
            try {
                run(pass_);
            } catch (...) {
                if (active_) cuptiEventGroupSetDisable(&set);
                active_ = false;
                throw;
            }
            if (passNs_ <= 0) throw std::runtime_error("CUPTI sample has no kernel duration");
            if (pass_ == 0) duration = passNs_;
            for (const auto& [event, value] : passValues_)
                values[event] = static_cast<uint64_t>(std::llround(value * duration / passNs_));
        }
        std::vector<double> result;
        for (auto& metric : metrics_) {
            if (metric.raw) {
                const auto event = metric.events.front();
                double value = values.at(event);
                if (metric.average) value /= instances_.at(event);
                if (metric.rate) value *= 1e9 / duration;
                result.push_back(value);
                continue;
            }
            std::vector<uint64_t> counts;
            for (auto event : metric.events) counts.push_back(values.at(event));
            CUpti_MetricValue value{};
            CUPTI_CHECK(cuptiMetricGetValue(device_, metric.id, metric.events.size() * sizeof(CUpti_EventID),
                                           metric.events.data(), counts.size() * sizeof(uint64_t),
                                           counts.data(), static_cast<uint64_t>(duration), &value));
            switch (metric.kind) {
                case CUPTI_METRIC_VALUE_KIND_DOUBLE: result.push_back(value.metricValueDouble); break;
                case CUPTI_METRIC_VALUE_KIND_UINT64: result.push_back(value.metricValueUint64); break;
                case CUPTI_METRIC_VALUE_KIND_INT64: result.push_back(value.metricValueInt64); break;
                case CUPTI_METRIC_VALUE_KIND_PERCENT: result.push_back(value.metricValuePercent); break;
                case CUPTI_METRIC_VALUE_KIND_THROUGHPUT: result.push_back(value.metricValueThroughput); break;
                case CUPTI_METRIC_VALUE_KIND_UTILIZATION_LEVEL: result.push_back(value.metricValueUtilizationLevel); break;
                default: throw std::runtime_error("unsupported CUPTI metric value kind");
            }
        }
        return result;
    }

private:
    CUdevice device_;
    std::vector<Metric> metrics_;
    std::unique_ptr<CUpti_EventGroupSets, decltype(&cuptiEventGroupSetsDestroy)> sets_{nullptr, cuptiEventGroupSetsDestroy};
    std::map<CUpti_EventID, double> passValues_;
    std::map<CUpti_EventID, uint32_t> instances_;
    size_t pass_ = 0;
    bool active_ = false;
    double passNs_ = 0;
    Event start_, stop_;
};

#endif
