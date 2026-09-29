/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 */

#pragma once

#include <string>

namespace perf_stat_csv {

struct Row {
    double timestamp = 0.0;
    std::string scope;
    long long value = 0;
    double metric_value = 0.0;
    std::string metric_unit;
    bool has_metric_value = false;
    std::string event_name;
    bool has_timestamp = false;
};

bool parse_row(const std::string& line, bool expect_timestamp, Row& row);

long long normalize_row_value(const Row& row, int cache_line_size);

}
