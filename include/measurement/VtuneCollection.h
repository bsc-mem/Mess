#pragma once
#include "measurement/VtuneCounterParser.h"
#include "BenchmarkConfig.h"
#include "SystemInfo.h"
#include <memory>

namespace measurement::vtune {

class CollectionSession {
public:
    CollectionSession();
    ~CollectionSession();
    CollectionSession(const CollectionSession&);
    CollectionSession& operator=(const CollectionSession&) = delete;
    VtuneSample collect(const std::string& binary, const BandwidthCounterSelection& selection,
        double seconds, int helper_cpu, int verbosity);
    std::vector<VtuneSample> collect_batch(const std::string& binary,
        const BandwidthCounterSelection& selection, double seconds, int loops,
        int helper_cpu, int verbosity);
    VtuneSample profile(const std::string& binary, const BandwidthCounterSelection& selection,
        const std::string& command, int verbosity);
    bool probe_available(int verbosity);
    std::string setup_guidance();
    std::string locate_binary();
    bool process_elevated();
    int helper_cpu(const system_info&, const BenchmarkConfig&, int cores);
    double window_seconds(double requested_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::unique_ptr<CollectionSession> make_collection_session();
bool probe_available(int verbosity);
std::string setup_guidance();
std::string locate_binary();
bool process_elevated();
}
