#!/usr/bin/env python3
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

import argparse
import glob
import json
import os
import re

import pandas as pd
import numpy as np
from scipy.signal import savgol_filter

from libs.parser import curves_to_json_compact, bwlat_files, iter_bwlat_points
from libs.processor import process_data, calculate_mean_curves, filter_incomplete_ratios
from libs.mess_score import compute_mess_score
from libs.visualizer import plot_curves, plot_combined_curves, check_smooth_curves, configure_plot_style, filter_large_bw_jumps, save_gradient_legends_figure



COLORMAPS = ("Blues", "Reds", "Greens", "Oranges", "Purples", "Greys")

PEBS_LATENCY_SPECS = (
        ("mean_ns", "latency_mean.pdf", "Mean Latency [ns]"),
        ("min_ns", "latency_min.pdf", "Min Latency [ns]"),
        ("median_ns", "latency_median.pdf", "Median Latency [ns]"),
        ("p90_ns", "latency_p90.pdf", "P90 Latency [ns]"),
        ("p95_ns", "latency_p95.pdf", "P95 Latency [ns]"),
        ("p99_ns", "latency_p99.pdf", "P99 Latency [ns]"),
        ("p99_9_ns", "latency_p99_9.pdf", "P99.9 Latency [ns]"),
        ("max_ns", "latency_max.pdf", "Max Latency [ns]"),
        )

PEBS_PERCENTILE_OVERLAY_SPECS = (
        ("p99_ns", "Reds", "p99"),
        ("p95_ns", "Greens", "p95"),
        ("median_ns", "Blues", "p50"),
        )


def _with_latency_column(dfs_rw, metric_col):
    return {rw: df.assign(latency_smooth=df[metric_col]) for rw, df in dfs_rw.items() if metric_col in df.columns}


def _plot_percentile_curves(config, dfs_rw, processed_dir, cmap_name,
        draw_dots=False, draw_lines=True, legend_rw_min=None):
    """Generate separate BW-latency PDFs for each PEBS percentile."""
    sample_df = next(iter(dfs_rw.values()), None)
    if sample_df is None:
        return

    for metric_col, filename, y_label in PEBS_LATENCY_SPECS:
        if metric_col not in sample_df.columns:
            continue

        modified_dfs = _with_latency_column(dfs_rw, metric_col)
        if modified_dfs:
            plot_curves(
                    config,
                    modified_dfs,
                    os.path.join(processed_dir, filename),
                    cmap_name,
                    ignore_guards=True,
                    apply_corrections=False,
                    y_label=y_label,
                    draw_dots=draw_dots,
                    draw_lines=draw_lines,
                    legend_rw_min=legend_rw_min,
                    )


def _percentile_overlay_max_lat(dfs_rw):
    """Max latency across the p99/p95/p50 overlay columns for one dataset."""
    sample_df = next(iter(dfs_rw.values()), None)
    if sample_df is None:
        return 0.0
    cols = [c for c, _, _ in PEBS_PERCENTILE_OVERLAY_SPECS if c in sample_df.columns]
    max_lat = 0.0
    for df in dfs_rw.values():
        for col in cols:
            if col in df.columns:
                series = df[col].dropna()
                if not series.empty:
                    max_lat = max(max_lat, series.max())
    return max_lat


def _plot_percentile_overlay(config, dfs_rw, processed_dir, measuring_dir,
        limit_lat, draw_dots=False, draw_lines=True,
        legend_rw_min=None):
    """Overlay p99/p95/p50 latency bands for a single dataset on one chart.

    Reuses the combined-curves flow: each percentile becomes its own pseudo
    dataset (one colormap band of per-read-ratio curves), so the processing is
    identical to combined_memory_curves.pdf. p99 -> Reds, p95 -> Greens,
    p50 (median) -> Blues.
    """
    sample_df = next(iter(dfs_rw.values()), None)
    if sample_df is None:
        return

    label = os.path.basename(os.path.normpath(measuring_dir))
    overlay_datasets = []

    for metric_col, cmap_name, pct_label in PEBS_PERCENTILE_OVERLAY_SPECS:
        if metric_col not in sample_df.columns:
            continue

        modified_dfs = _with_latency_column(dfs_rw, metric_col)
        if modified_dfs:
            overlay_datasets.append((f'{label} {pct_label}', modified_dfs, config, cmap_name, measuring_dir))

    if len(overlay_datasets) < 2:
        return

    plot_combined_curves(
            overlay_datasets,
            os.path.join(processed_dir, "latency_percentiles.pdf"),
            limit_lat_override=limit_lat,
            ignore_guards=True,
            apply_corrections=False,
            draw_dots=draw_dots,
            draw_lines=draw_lines,
            legend_rw_min=legend_rw_min,
            )


def _parse_first_float(text):
    match = re.search(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)
    if not match:
        return None
    try:
        return float(match.group(0))
    except ValueError:
        return None


def _build_metric_curves(df_for_metrics, ratio_col, metric_col, apply_smoothing=False):
    """One bandwidth-vs-metric curve per read:write bucket, ordered by bandwidth.

    Like the memory curves, orphan points disconnected from the bucket's main
    curve are dropped and the raw values are plotted unless ``apply_smoothing``
    (--smooth) is set.
    """
    if df_for_metrics.empty or metric_col not in df_for_metrics.columns:
        return {}

    bw_col = next((c for c in ("bandwidth_mean", "bandwidth_smooth", "bandwidth") if c in df_for_metrics.columns), None)
    if bw_col is None:
        return {}

    required = [ratio_col, "pause", bw_col, metric_col]
    missing = [col for col in required if col not in df_for_metrics.columns]
    if missing:
        return {}

    if "source_row" in df_for_metrics.columns:
        required.append("source_row")
    dfs_metric = {}
    for rw in sorted(df_for_metrics[ratio_col].dropna().unique()):
        df_rw = df_for_metrics[df_for_metrics[ratio_col] == rw][required].copy()
        df_rw = df_rw.dropna(subset=[bw_col, metric_col])
        if df_rw.empty:
            continue

        df_rw = df_rw.sort_values(bw_col).reset_index(drop=True)
        df_rw = filter_large_bw_jumps(df_rw, bw_col=bw_col)
        if df_rw.empty:
            continue

        x = df_rw[bw_col].to_numpy(dtype=float)
        y = df_rw[metric_col].to_numpy(dtype=float)
        if apply_smoothing:
            x, y = _slight_savgol_smooth(x, y)
        df_rw["bandwidth_plot"] = x
        df_rw["metric_plot"] = y

        dfs_metric[rw] = df_rw

    return dfs_metric




def _collect_bwlat_rows(measuring_dir, config):
    return [{"bandwidth": point["bandwidth"], "latency": point["latency"], "rd_percentage": rd, "read_pct_rounded": rd, "pause": pause}
            for rd, pause, point in iter_bwlat_points(measuring_dir, config)]


def _build_curves_table(dfs_rw, read_label):
    return pd.concat([df.assign(**{read_label: key}) for key, df in dfs_rw.items()], ignore_index=True)


def _build_summary(df_for_csv):
    summary = {}
    if "bandwidth" in df_for_csv.columns:
        summary["bw_min"] = df_for_csv["bandwidth"].min()
        summary["bw_max"] = df_for_csv["bandwidth"].max()
    elif "bandwidth_mean" in df_for_csv.columns:
        summary["bw_min"] = df_for_csv["bandwidth_mean"].min()
        summary["bw_max"] = df_for_csv["bandwidth_mean"].max()

    if "latency" in df_for_csv.columns:
        summary["lat_min"] = df_for_csv["latency"].min()
        summary["lat_max"] = df_for_csv["latency"].max()
    elif "latency_mean" in df_for_csv.columns:
        summary["lat_min"] = df_for_csv["latency_mean"].min()
        summary["lat_max"] = df_for_csv["latency_mean"].max()

    return summary



def _load_metric_curves_from_processed(measuring_dir, apply_smoothing, include_low_ratios=False):
    processed_csv = os.path.join(measuring_dir, "processed", "memory_curves.csv")
    if not os.path.exists(processed_csv):
        return {}

    try:
        df_processed = pd.read_csv(processed_csv)
    except Exception:
        return {}

    ratio_col = "read_pct_rounded" if "read_pct_rounded" in df_processed.columns else "rd_percentage"
    if not include_low_ratios:
        df_processed = df_processed[df_processed[ratio_col] >= 50]
    metric_curves = {}
    return metric_curves



def _slight_savgol_smooth(x, y):
    """Apply a light Savitzky-Golay filter to a single (bw, lat) curve.

    Uses a short window (7) so the curve is only gently de-noised rather than
    heavily reshaped. Returns the inputs unchanged when there are too few points
    for the filter to be well defined.
    """
    n = len(x)
    if n < 5:
        return x, y

    window = min(7, n if n % 2 == 1 else n - 1)
    if window < 5:
        return x, y

    polyorder = min(3, window - 1)
    try:
        smooth_x, smooth_y = savgol_filter((x, y), window_length=window, polyorder=polyorder)
        return smooth_x, smooth_y
    except (ValueError, Exception):
        return x, y


def _find_aggregated_csv(measuring_dir):
    """Locate an already-aggregated bandwidth/latency CSV for the CSV-only path.

    Prefers ``processed/memory_curves.csv`` (the canonical processed output) and
    otherwise falls back to any ``*.csv`` sitting in the directory root.
    """
    candidates = [os.path.join(measuring_dir, "processed", "memory_curves.csv")]
    try:
        for name in sorted(os.listdir(measuring_dir)):
            if name.endswith(".csv"):
                candidates.append(os.path.join(measuring_dir, name))
    except OSError:
        pass
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def _aggregate_from_csv_fallback(measuring_dir, step, group_mode):
    """Rebuild the bucketed curve table from an aggregated CSV.

    Fallback when no raw measurement data (bw/lat/bwlat) is present: reads an
    already-aggregated CSV (e.g. a previous processed/memory_curves.csv or an
    MRDIMM sweep) and recomputes the read:write buckets — from the measured
    read fraction by default, or from the issued ``rd_percentage`` for
    --perkernel. Returns ``(df, read_label)`` or ``(None, None)``.
    """
    src_csv = _find_aggregated_csv(measuring_dir)
    if src_csv is None:
        print(f"\nWarning: no aggregated CSV found in {measuring_dir} to build curves")
        return None, None

    try:
        df = pd.read_csv(src_csv)
    except Exception as e:
        print(f"\nWarning: could not read {src_csv}: {e}")
        return None, None

    bw_col = next((c for c in ("bandwidth_mean", "bandwidth") if c in df.columns), None)
    lat_col = next((c for c in ("latency_mean", "latency") if c in df.columns), None)
    if bw_col is None or lat_col is None:
        missing = [name for name, col in (("bandwidth", bw_col), ("latency", lat_col)) if col is None]
        print(f"\nWarning: {src_csv} is missing column(s): {', '.join(missing)}")
        return None, None

    if "pause" not in df.columns:
        df["pause"] = 0

    step_v = float(step)
    if group_mode == "perkernel" and "rd_percentage" in df.columns:
        read_label = "rd_percentage"
        issued = pd.to_numeric(df["rd_percentage"], errors="coerce")
        df[read_label] = (issued / step_v).round() * step_v
    else:
        if group_mode == "perkernel":
            print(f"\nWarning: {os.path.basename(src_csv)} has no rd_percentage column; correcting read ratios instead")
        actual_col = next((c for c in ("rd_mapped_mean", "rd_percentage_actual", "rd_percentage_actual_mean") if c in df.columns), None)
        if actual_col is None:
            print(f"\nWarning: {src_csv} has no measured read-fraction column — cannot correct read ratios")
            return None, None
        read_label = "read_pct_rounded"
        actual_pct = pd.to_numeric(df[actual_col], errors="coerce") * 100.0
        df[read_label] = (actual_pct / step_v).round() * step_v

    df = df.dropna(subset=[read_label])
    if df.empty:
        print(f"\nWarning: could not derive read ratios from {src_csv}")
        return None, None
    df[read_label] = df[read_label].astype(int).clip(0, 100)

    if bw_col != "bandwidth_mean":
        df["bandwidth_mean"] = df[bw_col]
    if lat_col != "latency_mean":
        df["latency_mean"] = df[lat_col]

    print(f"\nNo raw data in {measuring_dir}: rebuilt read ratios from {os.path.basename(src_csv)}")
    return df, read_label

def _reorder_bw_zigzag(df, bw_col, tolerance_fraction=0.01, pad=10,
                       min_region_points=10, max_gap=2):
    """Sort by bandwidth only inside "zig zag" areas

    This function find these "zig zag" areas and order by banwdith. Remaining regions keep the per pause order.

    Returns ``(df, n_reordered_points)``.
    """
    if df.empty or bw_col not in df.columns or len(df) < 3:
        return df, 0

    bw = pd.to_numeric(df[bw_col], errors="coerce").to_numpy(dtype=float)
    if not np.isfinite(bw).all():
        return df, 0

    span = bw.max() - bw.min()
    if span <= 0:
        return df, 0

    steps = np.diff(bw)
    direction = 1.0 if steps.sum() >= 0 else -1.0
    backward = (steps * direction) < -(span * tolerance_fraction)
    if not backward.any():
        return df, 0

    # Group backward steps into regions, tolerating short forward runs inside.
    regions = []
    i = 0
    while i < len(backward):
        if not backward[i]:
            i += 1
            continue
        first = last = i
        gap = 0
        j = i + 1
        while j < len(backward):
            if backward[j]:
                last = j
                gap = 0
            else:
                gap += 1
                if gap > max_gap:
                    break
            j += 1
        regions.append((first, last))
        i = j

    n = len(bw)
    threshold = span * tolerance_fraction

    def _is_backward(a, b):
        return (b - a) * direction < -threshold

    # Expand each region by `pad` point-indices on both sides, then merge any
    # that now overlap. Without this, an earlier region's padding can eat
    # into a later region and leave it under min_region_points, so its points
    # fall through unsorted right where the zigzag actually is.
    spans = []
    for first, last in regions:
        # Step indices [first, last] span point indices [first, last + 1].
        start = max(0, first - pad)
        stop = min(n, last + 1 + pad)
        if spans and start <= spans[-1][1]:
            spans[-1] = (spans[-1][0], max(spans[-1][1], stop))
        else:
            spans.append((start, stop))

    # Sorting a block ascending (or descending) can leave its own endpoint a
    # backward step away from whatever untouched point sits right outside —
    # pull that point into the block instead of planting a fresh zigzag
    # right at the boundary.
    closed = []
    for start, stop in spans:
        block_max = bw[start:stop].max()
        block_min = bw[start:stop].min()
        while stop < n and _is_backward(block_max, bw[stop]):
            block_max = max(block_max, bw[stop])
            block_min = min(block_min, bw[stop])
            stop += 1
        while start > 0 and _is_backward(bw[start - 1], block_min):
            block_min = min(block_min, bw[start - 1])
            block_max = max(block_max, bw[start - 1])
            start -= 1
        if closed and start <= closed[-1][1]:
            closed[-1] = (closed[-1][0], max(closed[-1][1], stop))
        else:
            closed.append((start, stop))
    spans = closed

    order = np.arange(n)
    permutation = []
    cursor = 0
    reordered = 0

    for start, stop in spans:
        start = max(cursor, start)
        if stop - start < min_region_points:
            continue
        permutation.extend(order[cursor:start])
        block = order[start:stop]
        block = block[np.argsort(bw[block], kind="stable")]
        if direction < 0:
            block = block[::-1]
        permutation.extend(block)
        reordered += len(block)
        cursor = stop

    permutation.extend(order[cursor:])
    if reordered == 0:
        return df, 0
    return df.iloc[permutation].reset_index(drop=True), reordered

def _clean_latency(df, lat_col, window=9, n_sigma=3.0, min_scale_fraction=0.02,
                   max_removed_fraction=0.30, passes=2):
    """Drop latency points that spike above the local trend and then
    recover.
    """
    if df.empty or lat_col not in df.columns or len(df) < 5:
        return df, 0

    span = pd.to_numeric(df[lat_col], errors="coerce")
    span = span.max() - span.min()
    if not np.isfinite(span) or span <= 0:
        return df, 0

    n_before = len(df)

    for _ in range(max(1, passes)):
        n = len(df)
        win = min(window, n if n % 2 == 1 else n - 1)
        if win < 5:
            break

        lat = pd.to_numeric(df[lat_col], errors="coerce")
        local = lat.rolling(win, center=True, min_periods=3).median()
        dev = (lat - local).abs()
        mad = dev.rolling(win, center=True, min_periods=3).median()

        lat_vals = lat.to_numpy(dtype=float)
        scale = np.maximum(1.4826 * mad.to_numpy(dtype=float), span * min_scale_fraction)
        ratio = np.divide(dev.to_numpy(dtype=float), scale,
                          out=np.zeros(n), where=scale > 0)

        # min(lat[i+1:]) for every i; +inf past the end, so nothing can ever
        # look like it "recovers" from the sequence's own last point.
        suffix_min_after = np.r_[np.minimum.accumulate(lat_vals[::-1])[::-1][1:], np.inf]
        recovers = suffix_min_after < (lat_vals - scale * n_sigma)
        is_high = lat_vals > np.nan_to_num(local.to_numpy(dtype=float), nan=-np.inf)
        gated = np.where(is_high, recovers, True)

        outlier = (ratio > n_sigma) & gated
        outlier[0] = False
        outlier[-1] = False
        outlier |= ~np.isfinite(lat_vals)

        n_out = int(outlier.sum())
        if n_out == 0:
            break

        # Never a silent no-op: if the pass wants too much, take the worst
        # offenders up to the cap and leave the rest for a later run.
        allowed = int(max_removed_fraction * n)
        if n_out > allowed:
            if allowed < 1:
                break
            worst = np.argsort(np.where(outlier, ratio, -np.inf))[::-1][:allowed]
            capped = np.zeros(n, dtype=bool)
            capped[worst] = True
            outlier &= capped

        df = df[~outlier].reset_index(drop=True)

    n_removed = n_before - len(df)
    return df, n_removed




def _build_plot_curves(df_for_csv, read_label, apply_smoothing=False,
                       apply_completion_filter=True,
                       min_span_fraction=0.30,
                       min_points=4):
    """One curve per read:write bucket, holding exactly the values to draw.

    Connects each bucket's points in pause-sweep order (descending pause =
    ascending offered load), drops orphan idle-noise clusters disconnected from
    the bucket's main curve, and — only when --smooth is given — applies a light
    Savitzky-Golay pass. The drawn values live in bandwidth_smooth/latency_smooth.
    """
    if df_for_csv.empty or read_label not in df_for_csv.columns:
        return {}

    bw_col = next((c for c in ("bandwidth_mean", "bandwidth") if c in df_for_csv.columns), None)
    lat_col = next((c for c in ("latency_mean", "latency") if c in df_for_csv.columns), None)
    if bw_col is None or lat_col is None:
        return {}

    sort_col = "pause" if "pause" in df_for_csv.columns else bw_col
    valid_bw = df_for_csv[bw_col].dropna()
    global_span = valid_bw.max() - valid_bw.min() if not valid_bw.empty else 0.0

    dfs_rw = {}
    for ratio in sorted(df_for_csv[read_label].dropna().unique()):
        group = df_for_csv[df_for_csv[read_label] == ratio].copy()
        group = group.dropna(subset=[bw_col, lat_col])
        if group.empty:
            continue
        # Stray points that only round into this bucket at the idle end of the
        # sweep form an orphan cluster separated by a large BW gap from the
        # bucket's main curve; drop the disconnected side.
        group = filter_large_bw_jumps(group, bw_col=bw_col)
        if group.empty:
            continue
        if apply_completion_filter and global_span > 0:
            bw = group[bw_col].dropna()
            if len(bw) < min_points:
                continue
            # After orphan-cluster removal, reject only buckets that collapsed
            # into a narrow fragment. Do not require every ratio to reach the
            # dataset-wide max BW: each ratio has its own natural high-BW end.
            if (bw.max() - bw.min()) < global_span * min_span_fraction:
                continue

        group = group.sort_values(sort_col, ascending=False).reset_index(drop=True)
        group, n_removed = _clean_latency(group, lat_col)

        bw_vals = group[bw_col].to_numpy(dtype=float)
        lat_vals = group[lat_col].to_numpy(dtype=float)
        if apply_smoothing:
            bw_vals, lat_vals = _slight_savgol_smooth(bw_vals, lat_vals)
        group["bandwidth_smooth"] = bw_vals
        group["latency_smooth"] = lat_vals
        # Only extrapolate down to the y-axis when the bucket's own lowest
        # sample is actually near-idle; a bucket whose cheapest point is
        # already deep in the loaded region (e.g. it only has high-pause
        # samples) has no business being anchored at bandwidth 0 — doing so
        # fabricated a flat/diagonal line clear across the plot at that
        # point's (unrelated) latency.
        near_idle = global_span > 0 and bw_vals.min() <= global_span * 0.15
        if len(bw_vals) and bw_vals.min() > 0 and near_idle:
            pos = int(bw_vals.argmin())
            last5 = group["latency_smooth"].to_numpy()[np.argsort(bw_vals)[:5]]
            stable = len(last5) == 5 and (last5.max() - last5.min()) / last5.mean() <= 0.05
            if stable:
                anchor = group.iloc[[pos]].copy()
                anchor["bandwidth_smooth"] = 0.0
                group = pd.concat([group.iloc[:pos], anchor, group.iloc[pos:]], ignore_index=True)
        dfs_rw[ratio] = group
    return dfs_rw


def process_directory(measuring_dir, step, group_mode, cmap_name="Blues",
                      ignore_guards=False, apply_smoothing=False,
                      include_low_ratios=False, draw_dots=False, json_power=False,
                      ):
    """Process one measurement directory: bucket, filter, plot, write CSV/JSON."""
    if not os.path.exists(measuring_dir):
        return None, None, None

    processed_dir = os.path.join(measuring_dir, "processed")
    os.makedirs(processed_dir, exist_ok=True)
    draw_lines = not draw_dots

    df_bw, df_lat, config = process_data(measuring_dir)

    has_bw_lat_dirs = os.path.exists(os.path.join(measuring_dir, "bw")) and os.path.exists(os.path.join(measuring_dir, "lat"))
    has_bwlat_files = bool(bwlat_files(measuring_dir))

    if df_bw.empty or df_lat.empty:
        df_for_csv, read_label = _aggregate_from_csv_fallback(measuring_dir, step, group_mode)
        if df_for_csv is None:
            return None, None, None
    elif has_bwlat_files and not has_bw_lat_dirs:
        read_label = "read_pct_rounded"
        df_for_csv = pd.DataFrame(_collect_bwlat_rows(measuring_dir, config))
        if df_for_csv.empty:
            return None, None, None
    else:
        read_label = "read_pct_rounded" if group_mode == "perread" else "rd_percentage"
        df_for_csv = calculate_mean_curves(df_bw, df_lat, step, group_mode)
        if df_for_csv.empty:
            return None, None, None
        if read_label not in df_for_csv.columns:
            fallback_label = "read_pct_rounded" if read_label == "rd_percentage" else "rd_percentage"
            if fallback_label in df_for_csv.columns:
                read_label = fallback_label

    if not ignore_guards:
        df_for_csv = filter_incomplete_ratios(df_for_csv, read_label, mode=group_mode)

    dfs_rw = _build_plot_curves(df_for_csv, read_label,
                                apply_smoothing=apply_smoothing,
                                apply_completion_filter=not ignore_guards)
    if not dfs_rw:
        return None, None, None

    # The CSV keeps every bucket so any run can be re-plotted later.
    df_curves_all = _build_curves_table(dfs_rw, read_label)

    smooth_ok, problematic = check_smooth_curves(df_curves_all, gap_fraction=0.4, ignore_guards=ignore_guards)
    if not smooth_ok and problematic:
        print(f"\nWarning: large bandwidth gaps in ratio bucket(s) {problematic} of {os.path.basename(os.path.normpath(measuring_dir))}")


    csv_path = os.path.join(processed_dir, "memory_curves.csv")
    df_curves_all.to_csv(csv_path, index=False)

    legend_rw_min = 0 if group_mode == "perkernel" or include_low_ratios else None

    plot_curves(
        config,
        dfs_rw,
        os.path.join(processed_dir, "memory_curves.pdf"),
        cmap_name,
        ignore_guards=True,
        apply_corrections=False,
        draw_dots=draw_dots,
        draw_lines=draw_lines,
        legend_rw_min=legend_rw_min,
    )

    _plot_percentile_curves(
        config, dfs_rw, processed_dir, cmap_name,
        draw_dots=draw_dots,
        draw_lines=draw_lines,
        legend_rw_min=legend_rw_min,
    )

    json_path = os.path.join(processed_dir, "memory_curves.json")
    power_data = None
    curves_to_json_compact(dfs_rw, json_path, power_data=power_data)

    # Drop legend files from previous runs (rewritten below).
    stale_patterns = ("*.legends.*",)
    for pattern in stale_patterns:
        for stale in glob.glob(os.path.join(processed_dir, pattern)):
            try:
                os.remove(stale)
            except OSError:
                pass

    rw_min = legend_rw_min if legend_rw_min is not None else (0 if any(float(rw) < 50 for rw in dfs_rw) else 50)
    save_gradient_legends_figure(os.path.join(processed_dir, "curves"), [(None, cmap_name)], rw_min,
                                 )


    summary = _build_summary(df_for_csv)
    score = compute_mess_score(dfs_rw, config)
    if score is not None:
        summary["mess_score"] = score["overall_score"]
    return dfs_rw, config, summary






def main():
    parser = argparse.ArgumentParser(
            description="Generate memory bandwidth-latency roofline plots",
            formatter_class=argparse.ArgumentDefaultsHelpFormatter,
            )
    parser.add_argument("directories", nargs="+", help="One or more measurement directories")
    parser.add_argument(
        "--step",
        choices=["1", "2", "4", "5", "10", "50"],
        default="1",
        help="Plotting step: plot curves in steps of 1, 2, 4, 5, 10 or 50",
    )
    parser.add_argument(
        "--progress",
        action="store_true",
        help="Ignore validity checks and plot in-progress curves",
    )
    parser.add_argument(
        "--smooth",
        action="store_true",
        help="Apply a subtle Savitzky-Golay smoothing to the plotted curves (off by default)",
    )
    parser.add_argument(
        "--dots",
        action="store_true",
        help="Plot the raw points as dots instead of connected curves; also plots "
             "write-dominant ratios (below 50:50), widening the ratio legend to 100:0-0:100",
    )
    parser.add_argument(
        "--perkernel",
        action="store_true",
        help="Group curves by the issued read ratio instead of correcting to the measured one",
    )

    args = parser.parse_args()
    json_power = False
    configure_plot_style()

    directories = [os.path.abspath(directory) for directory in args.directories]
    group_mode = "perkernel" if args.perkernel else "perread"
    ignore_guards = args.progress
    draw_dots = args.dots
    draw_lines = not draw_dots
    apply_smoothing = args.smooth and not args.dots
    if args.smooth and args.dots:
        print("Warning: --smooth is ignored with --dots (raw dots are never smoothed)")
    include_low_ratios = args.dots
    legend_rw_min = 0 if group_mode == "perkernel" or include_low_ratios else None

    datasets = []
    metric_datasets = []
    summary_list = []

    print("Processing directories...", end="", flush=True)

    for index, measuring_dir in enumerate(directories):
        print(".", end="", flush=True)
        dataset_cmap = COLORMAPS[index % len(COLORMAPS)]
        dfs_rw, config, summary = process_directory(
            measuring_dir,
            args.step,
            group_mode,
            dataset_cmap,
            ignore_guards=ignore_guards,
            apply_smoothing=apply_smoothing,
            include_low_ratios=include_low_ratios,
            draw_dots=draw_dots,
            json_power=json_power,
        )
        if not dfs_rw:
            continue

        label = os.path.basename(os.path.normpath(measuring_dir))
        datasets.append((label, dfs_rw, config, dataset_cmap, measuring_dir))
        metric_curves = _load_metric_curves_from_processed(
            measuring_dir,
            apply_smoothing,
            include_low_ratios=include_low_ratios,
        )
        metric_datasets.append((label, metric_curves, config, dataset_cmap, measuring_dir))

        if summary:
            summary["label"] = label
            summary["dir"] = measuring_dir
            summary_list.append(summary)

    print(" Done!")

    if not datasets:
        return

    # Generate the p99/p95/p50 latency overlay per dataset using a single shared
    # Y limit across all datasets so they can be compared directly.
    overlay_max_lat = 0.0
    for _, dfs_rw, _, _, _ in datasets:
        overlay_max_lat = max(overlay_max_lat, _percentile_overlay_max_lat(dfs_rw))
    overlay_limit = max(900.0, overlay_max_lat * 1.1) if overlay_max_lat > 0 else 900.0

    for _, dfs_rw, cfg, _, measuring_dir in datasets:
        _plot_percentile_overlay(
            cfg,
            dfs_rw,
            os.path.join(measuring_dir, "processed"),
            measuring_dir,
            overlay_limit,
            draw_dots=draw_dots,
            draw_lines=draw_lines,
            legend_rw_min=legend_rw_min,
        )


    if len(datasets) > 1:
        first_dir = directories[0]
        plot_combined_curves(
            datasets,
            os.path.join(first_dir, "processed", "combined_memory_curves.pdf"),
            ignore_guards=True,
            apply_corrections=False,
            draw_dots=draw_dots,
            draw_lines=draw_lines,
            legends_output_base=os.path.join(first_dir, "processed", "combined"),
            legend_rw_min=legend_rw_min,
        )


    print("\n\n===========================================================================================================")
    print("                                             PLOTTING SUMMARY")
    print("===========================================================================================================")
    print(f"{'Directory':<30} | {'Bandwidth Range (GB/s)':<25} | {'Latency Range (ns)':<25} | {'Mess Score (0-100)':<18}")
    print("-" * 107)

    for item in summary_list:
        bw_range = f"{item.get('bw_min', 0):.2f} - {item.get('bw_max', 0):.2f}"
        lat_range = f"{item.get('lat_min', 0):.1f} - {item.get('lat_max', 0):.1f}"
        mess = f"{item['mess_score']:.2f}" if item.get("mess_score") is not None else "N/A"
        print(f"{item['label']:<30} | {bw_range:<25} | {lat_range:<25} | {mess:<18}")
        rel_dir = os.path.relpath(item["dir"], os.getcwd())
        print(f"   -> {rel_dir}/processed")

    if len(datasets) > 1:
        first_dir = directories[0]
        rel_first_dir = os.path.relpath(first_dir, os.getcwd())
        print("-" * 107)
        print("Combined plot saved to:")
        print(f"   -> {rel_first_dir}/processed/combined_memory_curves.pdf")

    print("===========================================================================================================\n")


if __name__ == "__main__":
    main()
