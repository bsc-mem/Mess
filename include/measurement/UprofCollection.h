#pragma once
#include <algorithm>
#include <chrono>
#include <string>

namespace measurement::uprof {
// A one-second collection has no complete report interval; uProf needs two.
inline constexpr int kUprofMinDurationSeconds = 2;
inline constexpr int kUprofReportIntervalMs = 1000;
// Callers use the same effective duration when converting GB/s back to counts.
inline int uprof_collection_seconds(int requested) {
    return std::max(kUprofMinDurationSeconds, requested);
}

// Each invocation owns a temporary directory. Failed processes never yield CSV.
// A negative timeout selects the collection duration plus a 60-second allowance.
std::string run_collection(const std::string& binary, int duration_s, bool cumulative,
                           int interval_ms, std::string& error,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
} // namespace measurement::uprof
