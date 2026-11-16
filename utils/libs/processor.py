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

import os
import sys
import numpy as np
import pandas as pd
from scipy.signal import savgol_filter
from .parser import parse_bandwidth_file, parse_latency_file, parse_filename, iter_bwlat_points

def _file_contains_pcm_cxl_counters(filepath):
    try:
        with open(filepath, 'r') as f:
            for line in f:
                lower = line.lower()
                if 'pcm_cxl_read_mbps' in lower or 'pcm_cxl_write_mbps' in lower or 'pcm_cxl_total_mbps' in lower:
                    return True
    except OSError:
        return False
    return False

def process_data(measuring_dir):
    config_path = os.path.join(measuring_dir, 'plotter.txt')
    
    if os.path.exists(config_path):
        with open(config_path, 'r') as f:
            config = dict(x.rstrip().split("=", 1) for x in f if '=' in x and not x.startswith('#'))
    else:
        print(f"Error: plotter.txt not found in {measuring_dir}")
        print("Please run generate_code first to create the configuration file")
        sys.exit(1)
    
    bw_data = []
    is_intel_pcm = False
    bw_dir = os.path.join(measuring_dir, 'bw')
    if os.path.exists(bw_dir):
        files = os.listdir(bw_dir)
        for filename in files:
            if filename.startswith('bw_') and filename.endswith('.txt'):
                filepath = os.path.join(bw_dir, filename)
                if not is_intel_pcm and _file_contains_pcm_cxl_counters(filepath):
                    is_intel_pcm = True
                rd_percentage, pause = parse_filename(filename)
                
                if rd_percentage is not None:
                    measurements = parse_bandwidth_file(filepath, config, rd_percentage_from_filename=rd_percentage)
                    if measurements:
                        for measurement in measurements:
                            measurement.update({
                                'rd_percentage': rd_percentage,
                                'pause': pause
                            })
                            bw_data.append(measurement)
    
    lat_data = []
    lat_dir = os.path.join(measuring_dir, 'lat')
    if os.path.exists(lat_dir):
        files = os.listdir(lat_dir)
        for filename in files:
            if filename.startswith('lat_') and filename.endswith('.txt'):
                filepath = os.path.join(lat_dir, filename)
                rd_percentage, pause = parse_filename(filename)
                
                if rd_percentage is not None:
                    measurements = parse_latency_file(filepath, config)
                    if measurements:
                        for measurement in measurements:
                            measurement.update({
                                'rd_percentage': rd_percentage,
                                'pause': pause
                            })
                            lat_data.append(measurement)
    
    for rd_percentage, pause, point in iter_bwlat_points(measuring_dir, config):
        bw_data.append({'bandwidth': point['bandwidth'], 'rd_percentage': rd_percentage, 'pause': pause})
        lat_data.append({'latency': point['latency'], 'rd_percentage': rd_percentage, 'pause': pause})

    df_bw = pd.DataFrame(bw_data) if bw_data else pd.DataFrame()
    df_lat = pd.DataFrame(lat_data) if lat_data else pd.DataFrame()

    if is_intel_pcm:
        config['IS_INTEL_PCM'] = 'true'

    return df_bw, df_lat, config


def _effective_read_fraction_for_combined_counters(issued_pct):
    """Infer read fraction when only total bandwidth is available.

    With write-allocate traffic, an issued write contributes one read-for-
    ownership transaction plus one write transaction. Therefore an issued
    50:50 mix maps to read transactions (50 + 50) over total transactions
    (50 + 2*50), i.e. 67:33.
    """
    issued = np.asarray(issued_pct, dtype=float) / 100.0
    issued = np.clip(issued, 0.0, 1.0)
    return 1.0 / (2.0 - issued)


def calculate_mean_curves(df_bw, df_lat, step, mode):
    step = float(step)
    if df_bw.empty or df_lat.empty:
        return pd.DataFrame()

    df_bw = df_bw.copy()
    df_lat = df_lat.copy()

    if mode == 'perread':
        read_label = 'read_pct_rounded'

        actual_pct = df_bw['rd_percentage_actual'] * 100
        if 'bandwidth_counter_combined' in df_bw.columns:
            combined_mask = df_bw['bandwidth_counter_combined'].fillna(False).astype(bool)
            if combined_mask.any():
                print("Warning: single bandwidth counter detected; default correction mode "
                      "infers read/write ratio with write-allocate traffic "
                      "(50:50 -> 67:33). Use --perkernel to keep issued ratios.")
                actual_pct.loc[combined_mask] = (
                    _effective_read_fraction_for_combined_counters(
                        df_bw.loc[combined_mask, 'rd_percentage']
                    ) * 100.0
                )
                df_bw.loc[combined_mask, 'rd_percentage_actual'] = actual_pct.loc[combined_mask] / 100.0
        df_bw[read_label] = (np.round(actual_pct / step) * step).astype(int)
        df_bw[read_label] = df_bw[read_label].clip(0, 100)

        mapping = (df_bw[['rd_percentage', 'pause', read_label]]
                   .drop_duplicates(subset=['rd_percentage', 'pause'])
                   .set_index(['rd_percentage', 'pause'])[read_label])

        df_lat[read_label] = df_lat.set_index(['rd_percentage', 'pause']).index.map(mapping)

        if df_lat[read_label].isna().any():
            n_missing = df_lat[read_label].isna().sum()
            print(f"Warning: {n_missing} rows in df_lat could not be mapped to read_pct_rounded.")

        df_lat = df_lat.dropna(subset=[read_label])

    else:
        read_label = 'rd_percentage' if mode == 'perkernel' else 'read_pct_rounded'
        reads = df_bw['rd_percentage']
        df_bw[read_label] = (np.round(reads / step) * step).astype(int)
        df_bw[read_label] = df_bw[read_label].clip(0, 100)
        if read_label == 'rd_percentage':
            if 'rd_percentage' in df_lat.columns:
                df_lat['rd_percentage'] = (np.round(df_lat['rd_percentage'] / step) * step).astype(int)
                df_lat['rd_percentage'] = df_lat['rd_percentage'].clip(0, 100)
        else:
            mapping = (df_bw[['rd_percentage', 'pause', read_label]]
                       .drop_duplicates(subset=['rd_percentage', 'pause'])
                       .set_index(['rd_percentage', 'pause'])[read_label])
            df_lat[read_label] = df_lat.set_index(['rd_percentage', 'pause']).index.map(mapping)
            df_lat = df_lat.dropna(subset=[read_label])

    bw_mean = df_bw.groupby([read_label, 'pause']).agg({
        'bandwidth': ['mean', 'std'],
        'rd_percentage_actual': 'mean'   # keep the true average actual %
    }).reset_index()

    pebs_lat_cols = [c for c in ('mean_ns', 'min_ns', 'median_ns', 'p90_ns', 
                                 'p95_ns', 'p99_ns', 'p99_9_ns', 'max_ns') 
                     if c in df_lat.columns]

    lat_agg = {'latency': ['mean', 'std']}
    for c in pebs_lat_cols:
        lat_agg[c] = 'mean'

    lat_mean = df_lat.groupby([read_label, 'pause']).agg(lat_agg).reset_index()

    bw_mean.columns = [read_label, 'pause', 'bandwidth_mean', 'bandwidth_std', 'rd_percentage_actual_mean']
    lat_mean.columns = [read_label, 'pause', 'latency_mean', 'latency_std'] + pebs_lat_cols

    df = bw_mean.merge(lat_mean, on=[read_label, 'pause'], how='inner')

    df = df.sort_values(by=[read_label, 'bandwidth_mean'], 
                        ascending=[True, False]).reset_index(drop=True)

    return df

GENTLE_SAVGOL_WINDOW = 5
GENTLE_SAVGOL_POLY = 2


def _hampel_repair(y, window=3, n_sigmas=3.0):
    """Replace isolated spikes with the local median (Hampel filter).

    Only points that deviate from their neighbourhood by more than ``n_sigmas``
    robust standard deviations are replaced, so the real curve shape — including
    the knee — is preserved while measurement outliers are removed.
    """
    y = np.asarray(y, dtype=float)
    n = len(y)
    if n < 2 * window + 1:
        return y

    out = y.copy()
    scale = 1.4826  # MAD -> std for a normal distribution
    for i in range(n):
        lo = max(0, i - window)
        hi = min(n, i + window + 1)
        seg = y[lo:hi]
        med = np.median(seg)
        mad = scale * np.median(np.abs(seg - med))
        if mad > 0 and abs(y[i] - med) > n_sigmas * mad:
            out[i] = med
    return out


def _gentle_savgol(values):
    """Light Savitzky-Golay smoothing that adapts the window to short curves."""
    n = len(values)
    window = min(GENTLE_SAVGOL_WINDOW, n if n % 2 else n - 1)
    if window < GENTLE_SAVGOL_POLY + 2 or window < 3:
        return np.asarray(values, dtype=float)
    return savgol_filter(np.asarray(values, dtype=float), window_length=window,
                         polyorder=GENTLE_SAVGOL_POLY)


def smooth_curves(df, mode,
                  apply_smoothing=True,
                  apply_corrections=True,
                  min_y_change_pct=5.0):
    """Build one curve per read/write ratio with a gentle, shape-preserving clean.

    Every ratio present in ``df`` is kept (no whole-curve dropping). When
    ``apply_corrections`` is set, isolated measurement spikes are repaired with a
    Hampel filter; when ``apply_smoothing`` is set, a short Savitzky-Golay window
    denoises the curve. Neither step inserts synthetic anchor points or flattens
    the knee, so the curve shape stays faithful to the raw data.
    """
    if mode == 'perread':
        read_label = 'read_pct_rounded'
    else:
        read_label = 'rd_percentage'

    if read_label not in df.columns:
        fallback_label = 'read_pct_rounded' if read_label == 'rd_percentage' else 'rd_percentage'
        if fallback_label in df.columns:
            read_label = fallback_label
        else:
            return {}

    if df.empty:
        return {}

    dfs_rw = {}

    for rd_pct in sorted(df[read_label].unique()):
        df_rw = df[df[read_label] == rd_pct].copy()

        if df_rw.empty:
            continue

        if 'bandwidth' in df_rw.columns:
            bw_col = 'bandwidth'
            lat_col = 'latency'
        else:
            bw_col = 'bandwidth_mean'
            lat_col = 'latency_mean'

        df_rw = df_rw.sort_values(bw_col).reset_index(drop=True)

        x = df_rw[bw_col].to_numpy(dtype=float)
        y = df_rw[lat_col].to_numpy(dtype=float)

        if apply_corrections:
            y = _hampel_repair(y)

        if apply_smoothing and len(df_rw) >= GENTLE_SAVGOL_WINDOW:
            try:
                x = _gentle_savgol(x)
                y = _gentle_savgol(y)
            except (ValueError, Exception) as e:
                print(f"Smoothing failed for read_pct={rd_pct}: {e}")
                x = df_rw[bw_col].to_numpy(dtype=float)
                y = df_rw[lat_col].to_numpy(dtype=float)

        df_rw['bandwidth_smooth'] = x
        df_rw['latency_smooth'] = y
        dfs_rw[rd_pct] = df_rw

    return dfs_rw

def filter_incomplete_ratios(df, read_label, threshold_ratio=0.30,
                             original_label="rd_percentage_actual_mean",
                             mode='perread', min_points=4):
    """Drop read/write ratios that are too sparse to form a curve.

    This early pass only removes buckets with too few samples. Bandwidth-span
    checks happen after large-gap cleanup, because a bucket's valid high-BW
    endpoint is local to that ratio, not the dataset-wide maximum.
    """
    if df.empty:
        return df

    if read_label not in df.columns:
        return df

    counts = df.groupby(read_label).size()
    if counts.empty:
        return df

    ratios_to_keep = [ratio for ratio, count in counts.items() if count >= min_points]
    if not ratios_to_keep:
        return df

    return df[df[read_label].isin(ratios_to_keep)]
