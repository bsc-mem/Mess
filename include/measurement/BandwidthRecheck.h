#pragma once

#include <cmath>
#include <algorithm>

namespace measurement {

struct BandwidthReading {
    long long read = 0;
    long long write = 0;
    double seconds = 0.0;
};

enum class BandwidthRecheckStatus { Accepted, Changed, Failed };

struct BandwidthRecheckResult {
    BandwidthRecheckStatus status = BandwidthRecheckStatus::Failed;
    BandwidthReading reading;
    double bandwidth = 0.0;
    int samples = 0;
};

inline bool bandwidth_changed(double baseline, double candidate, double noise_floor = 0.0) {
    return std::abs(candidate - baseline) > std::max(baseline * 0.05, noise_floor);
}

// Callbacks keep backend-specific conversion (CAS, bytes, flits) outside the
// acceptance policy and let tests feed realistic confirmation sequences.
template <typename Sample, typename Convert>
BandwidthRecheckResult confirm_bandwidth(
    double baseline, BandwidthReading first, Sample sample, Convert convert, double noise_floor = 0.0) {
    BandwidthRecheckResult result;
    result.reading = first;
    if (first.seconds <= 0.0 || first.read < 0 || first.write < 0) return result;
    result.samples = 1;
    result.bandwidth = convert(first);
    if (!bandwidth_changed(baseline, result.bandwidth, noise_floor)) {
        result.status = BandwidthRecheckStatus::Accepted;
        return result;
    }
    for (int i = 0; i < 2; ++i) {
        BandwidthReading next;
        if (!sample(next) || next.seconds <= 0.0 || next.read < 0 || next.write < 0) break;
        result.reading.read += next.read;
        result.reading.write += next.write;
        result.reading.seconds += next.seconds;
        ++result.samples;
    }
    // A single outlying observation cannot confirm stability or sustained drift.
    if (result.samples < 2) return result;
    result.bandwidth = convert(result.reading);
    result.status = bandwidth_changed(baseline, result.bandwidth, noise_floor)
        ? BandwidthRecheckStatus::Changed : BandwidthRecheckStatus::Accepted;
    return result;
}

}
