# Copyright (c) 2026, Barcelona Supercomputing Center
# Contact: mess             [at] bsc [dot] es
#          victor.xirau     [at] bsc [dot] es
#          mariana.carmin   [at] bsc [dot] es
#          pau.diazcuesta   [at] bsc [dot] es
#          petar.radojkovic [at] bsc [dot] es
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#
#     * Neither the name of the copyright holder nor the names
#       of its contributors may be used to endorse or promote products
#       derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Saved direct timing and isolated compatibility for historical cycle-based data."""
import math


def read_cpu_frequency_ghz(config):
    """Read a saved cycles-to-nanoseconds conversion; never invent a frequency."""
    try:
        frequency = float(config['CPU_FREQ'])
    except (KeyError, ValueError, TypeError) as error:
        raise ValueError('Missing or invalid saved CPU frequency') from error
    if not math.isfinite(frequency) or frequency <= 0.0:
        raise ValueError('Missing or invalid saved CPU frequency')
    return frequency


def read_sampler_frequency_ghz(measurements, config):
    """A pooled cycle CSV requires one calibration shared by all its repetitions."""
    if not any('point_cpu_freq_ghz' in row for row in measurements):
        return read_cpu_frequency_ghz(config)  # Historical sampling records.
    frequencies = [read_cpu_frequency_ghz({'CPU_FREQ': row.get('point_cpu_freq_ghz')})
                   for row in measurements]
    frequency = frequencies[0]
    if any(not math.isclose(value, frequency, rel_tol=1e-9) for value in frequencies):
        raise ValueError('Sampler CSV combines different calibrations; plot saved latency summaries instead')
    return frequency


def resolve_pointer_chase_timing(cycles, wall_seconds, config):
    """Return (total nanoseconds, GHz), following the dataset's timing provenance."""
    source = config.get('CPU_FREQ_SOURCE', '')
    if source == 'startup_probe':
        frequency = read_cpu_frequency_ghz(config)
        return cycles / frequency, frequency
    if source == 'wall_clock':
        return wall_seconds * 1e9, 0.0
    frequency = _legacy_cpu_frequency_ghz(cycles, wall_seconds, config)
    return cycles / frequency, frequency


def _legacy_cpu_frequency_ghz(cycles, seconds_user, config):

    def _as_float(key, default=0.0):
        try:
            return float(config.get(key, default))
        except (ValueError, TypeError):
            return default

    measured = (cycles / seconds_user) / 1e9 if seconds_user > 0 else 0.0
    base_ghz = _as_float('CPU_BASE_FREQ', 0.0)
    max_ghz = _as_float('CPU_MAX_FREQ', 0.0)
    cfg_ghz = _as_float('CPU_FREQ', 0.0)
    if base_ghz <= 0.0:
        base_ghz = cfg_ghz
    if max_ghz <= 0.0:
        max_ghz = cfg_ghz

    lo = base_ghz * 0.7 if base_ghz > 0.0 else 0.0
    hi = max_ghz * 1.1 if max_ghz > 0.0 else 0.0

    in_band = measured > 0.0 and measured >= lo and (hi <= 0.0 or measured <= hi)
    if in_band:
        return measured
    if cfg_ghz > 0.0:
        return cfg_ghz
    if base_ghz > 0.0:
        return base_ghz
    if max_ghz > 0.0:
        return max_ghz
    return measured if measured > 0.0 else 3.0



def saved_direct_latency_ns(metrics):
    """The recorded latency is authoritative; invalid values must not fall back."""
    if 'latency_direct_ns' not in metrics:
        return None
    value = metrics['latency_direct_ns']
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError('Invalid saved direct latency')
    return value


def point_timing_alternatives(cycles, accesses, metrics, config):
    """Preserve saved direct latency and derive separate raw timing alternatives."""
    result = dict(metrics)
    direct = saved_direct_latency_ns(metrics)
    if accesses <= 0.0:
        return result
    if cycles > 0.0 and config.get('CPU_FREQ_SOURCE') == 'startup_probe':
        try:
            result['latency_startup_raw_ns'] = cycles / read_cpu_frequency_ghz(config) / accesses
        except ValueError:
            if direct is None:
                raise
    point_frequency = metrics.get('point_cpu_freq_ghz', 0.0)
    if math.isfinite(point_frequency) and point_frequency > 0.0 and cycles > 0.0:
        result['latency_point_probe_raw_ns'] = cycles / point_frequency / accesses
    elapsed_ns = metrics.get('pointer_chase_elapsed_ns', 0.0)
    if math.isfinite(elapsed_ns) and elapsed_ns > 0.0:
        # Accesses already includes the number of bursts. Do not divide twice.
        result['latency_elapsed_ns'] = elapsed_ns / accesses
        if direct is None:
            result['latency_direct_ns'] = elapsed_ns / accesses
        result['latency_direct_clock'] = 'wall'
    thread_ns = metrics.get('pointer_chase_thread_ns', 0.0)
    if math.isfinite(thread_ns) and thread_ns > 0.0:
        result['latency_thread_ns'] = thread_ns / accesses
        if elapsed_ns > 0.0:
            result['on_cpu_fraction'] = thread_ns / elapsed_ns
        else:
            result['latency_direct_clock'] = 'thread'
            # Current thread-only records save a TLB-corrected direct value.
            # Older diagnostic records contain elapsed_ns and save raw wall time.
            result['latency_direct_tlb_corrected'] = direct is not None
            if direct is None:
                result['latency_direct_ns'] = thread_ns / accesses
    enabled = metrics.get('cycles_enabled_ns', 0.0)
    running = metrics.get('cycles_running_ns', 0.0)
    if enabled > 0.0:
        result['cycle_counter_coverage'] = running / enabled
    return result
