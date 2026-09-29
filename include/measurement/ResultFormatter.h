/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 */

#ifndef RESULT_FORMATTER_H
#define RESULT_FORMATTER_H

#include "Measurement.h"
#include "architecture/BandwidthCounterStrategy.h"

#include <string>
#include <sys/types.h>

namespace measurement {

std::string format_bandwidth_output(const BenchmarkResult& result,
                                    const BandwidthCounterSelection& selection,
                                    int bw_bytes_per_cas);

std::string format_latency_output(const BenchmarkResult& result,
                                  pid_t ptrchase_pid,
                                  bool instruction_sampling);


}

#endif
