#pragma once
#include <string>
#include <vector>

namespace measurement::uprof {
// uProf reports GB/s; Mess wants line counts. One CAS is one cache line.
struct UprofSample {
    double read_gbps  = 0.0;
    double write_gbps = 0.0;
    bool   valid      = false;
};

int count_df_sections(const std::string& text);
UprofSample parse_cumulative(const std::string& text);
std::vector<UprofSample> parse_timeseries(const std::string& text);
} // namespace measurement::uprof
