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

#include "measurement/bw_measurers/IOReportBandwidthMeasurer.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>

#ifdef __APPLE__

#include "measurement/BandwidthStabilizer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace {

typedef struct IOReportSubscriptionRefOpaque* IOReportSubscriptionRef;
typedef CFMutableDictionaryRef (*fn_copyall)(uint64_t, uint64_t);
typedef CFMutableDictionaryRef (*fn_copygroup)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
typedef IOReportSubscriptionRef (*fn_sub)(void*, CFMutableDictionaryRef, CFMutableDictionaryRef*, uint64_t, CFTypeRef);
typedef CFDictionaryRef (*fn_samples)(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
typedef CFDictionaryRef (*fn_delta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
typedef int (*fn_iter)(CFDictionaryRef, int (^)(CFDictionaryRef));
typedef CFStringRef (*fn_str)(CFDictionaryRef);
typedef int64_t (*fn_int)(CFDictionaryRef, int);
typedef int32_t (*fn_fmt)(CFDictionaryRef);

void scopy(CFStringRef cf, char* b, size_t n) {
    b[0] = 0;
    if (cf) CFStringGetCString(cf, b, n, kCFStringEncodingUTF8);
}

constexpr int kLaunchSettleMs = 250;

double monotonic_s() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

}

struct IOReportBandwidthMeasurer::IOReportState {
    void* lib = nullptr;
    fn_copyall IOReportCopyAllChannels = nullptr;
    fn_copygroup IOReportCopyChannelsInGroup = nullptr;
    fn_sub     IOReportCreateSubscription = nullptr;
    fn_samples IOReportCreateSamples = nullptr;
    fn_delta   IOReportCreateSamplesDelta = nullptr;
    fn_iter    IOReportIterate = nullptr;
    fn_str     gGroup = nullptr, gSub = nullptr, gName = nullptr, gUnit = nullptr;
    fn_int     gVal = nullptr;
    fn_fmt     gFmt = nullptr;

    CFMutableDictionaryRef subbed = nullptr;
    IOReportSubscriptionRef sub = nullptr;
    CFDictionaryRef prev = nullptr;
    double prev_t = 0.0;
    bool ready = false;
};

IOReportBandwidthMeasurer::IOReportBandwidthMeasurer(
    const BenchmarkConfig& config, const system_info& sys_info, const CPUCapabilities& caps,
    MeasurementStorage* storage, TrafficGenProcessManager* traffic_gen_manager,
    std::function<std::vector<int>()> numa_resolver, ExecutionMode mode)
    : BandwidthMeasurer(config, sys_info, caps, storage, traffic_gen_manager,
                        std::move(numa_resolver), mode) {
    state_ = new IOReportState();
}

IOReportBandwidthMeasurer::~IOReportBandwidthMeasurer() {
    if (state_) {
        if (state_->prev) CFRelease(state_->prev);
        if (state_->subbed) CFRelease(state_->subbed);
        if (state_->lib) dlclose(state_->lib);
        delete state_;
        state_ = nullptr;
    }
}

bool IOReportBandwidthMeasurer::ensure_subscribed() const {
    IOReportState* s = state_;
    if (!s) return false;
    if (s->ready) return true;

    s->lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_NOW);
    if (!s->lib) {
        std::cerr << "ERROR: IOReport unavailable (cannot dlopen libIOReport.dylib): "
                  << dlerror() << std::endl;
        return false;
    }
    s->IOReportCopyAllChannels    = (fn_copyall)dlsym(s->lib, "IOReportCopyAllChannels");
    s->IOReportCopyChannelsInGroup = (fn_copygroup)dlsym(s->lib, "IOReportCopyChannelsInGroup");
    s->IOReportCreateSubscription = (fn_sub)dlsym(s->lib, "IOReportCreateSubscription");
    s->IOReportCreateSamples      = (fn_samples)dlsym(s->lib, "IOReportCreateSamples");
    s->IOReportCreateSamplesDelta = (fn_delta)dlsym(s->lib, "IOReportCreateSamplesDelta");
    s->IOReportIterate            = (fn_iter)dlsym(s->lib, "IOReportIterate");
    s->gGroup = (fn_str)dlsym(s->lib, "IOReportChannelGetGroup");
    s->gSub   = (fn_str)dlsym(s->lib, "IOReportChannelGetSubGroup");
    s->gName  = (fn_str)dlsym(s->lib, "IOReportChannelGetChannelName");
    s->gUnit  = (fn_str)dlsym(s->lib, "IOReportChannelGetUnitLabel");
    s->gFmt   = (fn_fmt)dlsym(s->lib, "IOReportChannelGetFormat");
    s->gVal   = (fn_int)dlsym(s->lib, "IOReportSimpleGetIntegerValue");

    if (!s->IOReportCopyAllChannels || !s->IOReportCreateSubscription ||
        !s->IOReportCreateSamples || !s->IOReportCreateSamplesDelta ||
        !s->IOReportIterate || !s->gGroup || !s->gSub || !s->gName || !s->gVal) {
        std::cerr << "ERROR: IOReport symbols missing from libIOReport.dylib." << std::endl;
        return false;
    }

    CFMutableDictionaryRef chans = s->IOReportCopyChannelsInGroup
        ? s->IOReportCopyChannelsInGroup(CFSTR("AMC Stats"), nullptr, 0, 0, 0) : nullptr;
    if (!chans) chans = s->IOReportCopyAllChannels(0, 0);
    if (!chans) {
        std::cerr << "ERROR: IOReportCopyAllChannels returned null." << std::endl;
        return false;
    }
    s->sub = s->IOReportCreateSubscription(nullptr, chans, &s->subbed, 0, nullptr);
    CFRelease(chans);
    if (!s->sub || !s->subbed) {
        std::cerr << "ERROR: IOReportCreateSubscription failed." << std::endl;
        return false;
    }

    s->prev = s->IOReportCreateSamples(s->sub, s->subbed, nullptr);
    s->prev_t = monotonic_s();
    s->ready = (s->prev != nullptr);
    return s->ready;
}

void IOReportBandwidthMeasurer::reset_subscription() const {
    IOReportState* s = state_;
    if (!s) return;
    if (s->prev) CFRelease(s->prev);
    if (s->subbed) CFRelease(s->subbed);
    if (s->sub) CFRelease(s->sub);
    s->prev = nullptr;
    s->subbed = nullptr;
    s->sub = nullptr;
    s->ready = false;
}

bool IOReportBandwidthMeasurer::restart_dcs_window() const {
    if (!ensure_subscribed()) return false;
    IOReportState* s = state_;
    CFDictionaryRef cur = s->IOReportCreateSamples(s->sub, s->subbed, nullptr);
    if (!cur) return false;
    CFRelease(s->prev);
    s->prev = cur;
    s->prev_t = monotonic_s();
    return true;
}

bool IOReportBandwidthMeasurer::read_dcs_delta(long long& rd_bytes, long long& wr_bytes,
                                               double& dt_seconds) const {
    rd_bytes = 0;
    wr_bytes = 0;
    dt_seconds = 0.0;
    if (!ensure_subscribed()) return false;
    IOReportState* s = state_;

    int interval_ms = static_cast<int>(sampling_interval_ms_);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    CFDictionaryRef cur = s->IOReportCreateSamples(s->sub, s->subbed, nullptr);
    if (!cur) return false;
    double tc = monotonic_s();

    CFDictionaryRef d = s->IOReportCreateSamplesDelta(s->prev, cur, nullptr);
    dt_seconds = tc - s->prev_t;
    CFRelease(s->prev);
    s->prev = cur;
    s->prev_t = tc;
    if (!d) return false;

    __block long long rd = 0, wr = 0;
    __block bool found_rd = false, found_wr = false;
    fn_str gGroup = s->gGroup, gSub = s->gSub, gName = s->gName, gUnit = s->gUnit;
    fn_int gVal = s->gVal;
    fn_fmt gFmt = s->gFmt;
    s->IOReportIterate(d, ^int(CFDictionaryRef ch) {
        char g[64], sg[64], cn[160], un[16];
        scopy(gGroup(ch), g, sizeof g);
        scopy(gSub(ch), sg, sizeof sg);
        scopy(gName(ch), cn, sizeof cn);
        scopy(gUnit ? gUnit(ch) : nullptr, un, sizeof un);
        if (gFmt && gFmt(ch) != 1) return 0;
        if (!strcmp(g, "AMC Stats") && !strcmp(sg, "Perf Counters") && !strcmp(un, "B")) {
            long long v = (long long)gVal(ch, 0);
            if (!strcmp(cn, "DCS RD")) { rd = v; found_rd = true; }
            else if (!strcmp(cn, "DCS WR")) { wr = v; found_wr = true; }
        }
        return 0;
    });
    CFRelease(d);

    if (!found_rd || !found_wr || rd + wr <= 0) {
        std::cerr << "    IOReport returned no DRAM traffic; resubscribing." << std::endl;
        reset_subscription();
        return false;
    }

    rd_bytes = rd;
    wr_bytes = wr;
    return true;
}

bool IOReportBandwidthMeasurer::sample_bandwidth(long long& cas_rd, long long& cas_wr,
                                                 double& elapsed,
                                                 const std::vector<int>&) const {
    long long rd = 0, wr = 0;
    double dt = 0.0;
    if (!restart_dcs_window() || !read_dcs_delta(rd, wr, dt) || dt <= 0.0) return false;
    cas_rd = rd;
    cas_wr = wr;
    elapsed = dt;
    return true;
}

bool IOReportBandwidthMeasurer::wait_for_stabilization(int& samples_taken, long long& last_cas_rd,
                                                       long long& last_cas_wr, double& last_elapsed,
                                                       int pause, int ratio, bool fast_resume,
                                                       std::function<void()> on_sample) {
    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;

    if (!ensure_subscribed()) {
        return false;
    }

    int relaunch_attempts = 0;
    if (!ensure_traffic_gen_running(ratio, pause, relaunch_attempts)) return false;

    double theoretical_peak_gb_s = achievable_peak_gbps(false);
    if (theoretical_peak_gb_s <= 0) theoretical_peak_gb_s = 10.0;

    BandwidthStabilizer stabilizer = make_stabilizer(theoretical_peak_gb_s, pause, fast_resume ? 5 : 7);

    const int interval_ms = std::max(1, static_cast<int>(sampling_interval_ms_));
    const int OVERALL_TIMEOUT_SECONDS = 60;
    const int max_samples = static_cast<int>((OVERALL_TIMEOUT_SECONDS * 1000.0) / interval_ms);

    long long rd = 0, wr = 0;
    double dt = 0.0;
    if (!fast_resume) std::this_thread::sleep_for(std::chrono::milliseconds(kLaunchSettleMs));
    restart_dcs_window();

    int total_samples = 0;
    int failed_reads = 0;
    bool success = false;
    long long final_rd = 0, final_wr = 0;
    double final_elapsed = interval_ms / 1000.0;

    while (total_samples + failed_reads < max_samples) {
        if (!read_dcs_delta(rd, wr, dt) || dt <= 0.0) {
            ++failed_reads;
            continue;
        }
        total_samples++;

        double bw_gb_s = bandwidth_gbps(rd, wr, dt);
        final_rd = rd;
        final_wr = wr;
        final_elapsed = dt;

        StabilizationResult result = stabilizer.add_sample(rd, wr, bw_gb_s, dt);
        if (on_sample) on_sample();

        if (result == StabilizationResult::ZOMBIE_DETECTED) {
            if (verbose2) std::cout << "    [ZOMBIE] TrafficGen died, aborting stabilization" << '\n';
            samples_taken = total_samples;
            return false;
        }

        if (verbose3) {
            double total = static_cast<double>(rd + wr);
            double read_ratio = (total > 0) ? (static_cast<double>(rd) / total) : 0.0;
            std::cout << "      [Sample " << total_samples << "] AMC bytes rd/wr: " << rd << "/" << wr;
            stabilizer.print_status(read_ratio, bw_gb_s, true);
        }

        if (stabilizer.is_stable()) {
            success = true;
            break;
        }
    }

    if (success) {
        long long agg_rd = 0, agg_wr = 0;
        double agg_elapsed = 0.0;
        const auto& samples = stabilizer.get_samples();
        double bw_sum = 0.0;
        double bw_sum_sq = 0.0;
        for (const auto& sample : samples) {
            agg_rd += sample.cas_rd;
            agg_wr += sample.cas_wr;
            agg_elapsed += sample.elapsed_s;
            bw_sum += sample.bw_gb_s;
            bw_sum_sq += sample.bw_gb_s * sample.bw_gb_s;
        }
        if (samples.size() > 1) {
            const double n = static_cast<double>(samples.size());
            const double variance = std::max(0.0, (bw_sum_sq - bw_sum * bw_sum / n) / (n - 1.0));
            stability_noise_floor_gbps_ = std::max(stability_noise_floor_gbps_, 2.0 * std::sqrt(variance));
        }
        last_cas_rd = agg_rd;
        last_cas_wr = agg_wr;
        last_elapsed = agg_elapsed;
        if (verbose3) {
            std::cout << "      [BW AGGREGATE] Using " << samples.size()
                      << " stable samples (" << last_elapsed << "s total)" << '\n';
        }
        samples_taken = total_samples;
        return true;
    }

    last_cas_rd = final_rd;
    last_cas_wr = final_wr;
    last_elapsed = final_elapsed;
    samples_taken = total_samples;
    return false;
}

#else

IOReportBandwidthMeasurer::IOReportBandwidthMeasurer(
    const BenchmarkConfig& config, const system_info& sys_info, const CPUCapabilities& caps,
    MeasurementStorage* storage, TrafficGenProcessManager* traffic_gen_manager,
    std::function<std::vector<int>()> numa_resolver, ExecutionMode mode)
    : BandwidthMeasurer(config, sys_info, caps, storage, traffic_gen_manager,
                        std::move(numa_resolver), mode) {}

IOReportBandwidthMeasurer::~IOReportBandwidthMeasurer() = default;

bool IOReportBandwidthMeasurer::ensure_subscribed() const { return false; }

void IOReportBandwidthMeasurer::reset_subscription() const {}

bool IOReportBandwidthMeasurer::restart_dcs_window() const { return false; }

bool IOReportBandwidthMeasurer::read_dcs_delta(long long&, long long&, double&) const { return false; }

bool IOReportBandwidthMeasurer::sample_bandwidth(long long&, long long&, double&,
                                                 const std::vector<int>&) const {
    return false;
}

bool IOReportBandwidthMeasurer::wait_for_stabilization(int&, long long&, long long&, double&,
                                                       int, int, bool, std::function<void()>) {
    return false;
}

#endif
