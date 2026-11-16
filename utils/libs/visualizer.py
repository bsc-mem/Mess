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
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import warnings
import numpy as np
from matplotlib.colors import Normalize

fontSizeMess = 24
curveLineWidth = 1.0
refLineWidth = 2.0
markerSize = 4
figSize = (16, 9)
gradientLegendBoxSize = (0.25, 0.03)


def configure_plot_style():
    global fontSizeMess, curveLineWidth, refLineWidth, markerSize
    global figSize, gradientLegendBoxSize
    fontSizeMess = 38
    curveLineWidth = 2.0
    refLineWidth = 3.5
    markerSize = 8
    figSize = (16, 9)
    gradientLegendBoxSize = (0.34, 0.028)


def _legend_fontsize():
    return fontSizeMess - 4


def _center_ylabels_on_figure(fig):
    try:
        fig.canvas.draw()
    except Exception:
        return

    fig_h = fig.bbox.height
    for ax in fig.axes:
        label = ax.yaxis.label
        if not label.get_text():
            continue
        pos = ax.get_position()
        ax_h = pos.height * fig_h
        if ax_h <= 0:
            continue
        # x is in points (labelpad); keep it and only retarget the vertical anchor
        # so the label's centre lands on the figure's vertical middle.
        x_pts = label.get_position()[0]
        yfrac = (fig_h / 2.0 - pos.y0 * fig_h) / ax_h
        label.set_position((x_pts, yfrac))


def _save_main_figure(fig, output_path):
    fig.set_size_inches(figSize)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        fig.tight_layout()
    # Tall y limits + large y-labels can clip the trailing "[ns]" without extra left room.
    fig.subplots_adjust(left=max(fig.subplotpars.left, 0.10))
    _center_ylabels_on_figure(fig)
    save_kw = dict(bbox_inches='tight', dpi=300, pad_inches=0.15)
    fig.savefig(output_path, **save_kw)
    fig.savefig(os.path.splitext(output_path)[0] + '.png', **save_kw)
    plt.close(fig)


def _is_intel_pcm_config(config):
    if not isinstance(config, dict):
        return False

    explicit_marker = str(config.get("IS_INTEL_PCM", "")).strip().lower()
    if explicit_marker in ("1", "true", "yes", "y"):
        return True

    for key in ("MEASURER", "BW_TOOL", "BW_MEASURER", "COUNTER_SOURCE"):
        value = str(config.get(key, "")).strip().lower()
        if "pcm" in value:
            return True

    for key in ("READ_EVENT", "WRITE_EVENT", "READ_EVENTS", "WRITE_EVENTS", "BW_EVENT"):
        value = str(config.get(key, "")).strip().lower()
        if "pcm_cxl/" in value or "pcm_cxl_" in value:
            return True

    return False

def calculate_color(rw, cmap_name='Blues', rw_min=50.0):
    """Map RW ratio to Blues using legacy intensity bounds (no near-white curves)."""
    cmap = plt.colormaps[cmap_name]

    span = 100.0 - float(rw_min)
    if span <= 0:
        return cmap(0.2)
    rw_clipped = max(float(rw_min), min(100.0, float(rw)))
    # Keep the same direction as legend labels: 100 on the left, rw_min on the right.
    c = 0.2 + ((100.0 - rw_clipped) / span) * 0.8

    return cmap(c)




def _curve_color(config, ratio, fallback_cmap='Blues', rw_min=50.0):
    return calculate_color(ratio, fallback_cmap, rw_min)




def _combined_curve_draw_order(datasets, legacy_reverse=False):
    return reversed(datasets) if legacy_reverse else datasets


def _derive_rw_min(ratios):
    """Ratio-legend lower bound: widen to 0:100 when write-dominant curves are drawn."""
    return 0 if any(float(rw) < 50 for rw in ratios) else 50


def _rd_wr_legend_label(rd_pct):
    rd = int(round(rd_pct))
    return f'RD:WR {rd}:{100 - rd}'


def _draw_gradient_legend_bar(cbar_ax, colors, rw_min=50, rw_max=100, fontsize=12):
    """Draw colormap bar + RD:WR end labels into an existing axes."""
    cmap = plt.get_cmap(colors)
    norm = Normalize(vmin=rw_min, vmax=rw_max)
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    cbar = plt.colorbar(sm, cax=cbar_ax, orientation='horizontal')
    cbar.outline.set_visible(False)
    cbar.ax.set_xticks([])
    cbar_ax.text(0.01, 0.5, _rd_wr_legend_label(rw_max), va='center', ha='right',
                 transform=cbar_ax.transAxes, fontsize=fontsize - 3, clip_on=False)
    cbar_ax.text(1.00, 0.5, _rd_wr_legend_label(rw_min), va='center', ha='left',
                 transform=cbar_ax.transAxes, fontsize=fontsize - 3, clip_on=False)




def add_gradient_legend(ax, colors, rw_min=50, rw_max=100, box_position=(0.05, 0.95), box_size=None, fontsize=12):
    """Add a horizontal gradient legend (colormap bar + RD:WR end labels) on `ax`."""
    if box_size is None:
        box_size = gradientLegendBoxSize
    bbox = ax.get_position()
    fig_x = bbox.x0 + box_position[0] * bbox.width
    fig_y = bbox.y0 + box_position[1] * bbox.height
    fig_width = box_size[0] * bbox.width
    fig_height = box_size[1] * bbox.height
    cbar_ax = ax.figure.add_axes([fig_x, fig_y, fig_width, fig_height])
    _draw_gradient_legend_bar(cbar_ax, colors, rw_min, rw_max, fontsize)


def _legend_only_layout_inches(n_legends, legend_fontsize, with_titles=True):
    pad_in = 0.12
    bar_width_in = 5.5
    bar_height_in = max(0.26, (legend_fontsize / 72.0) * 0.72)
    title_height_in = (legend_fontsize / 72.0) * 1.1 if with_titles else 0.0
    title_gap_in = 0.05 if with_titles else 0.0
    row_h = title_height_in + title_gap_in + bar_height_in
    row_gap_in = max(0.10, row_h * 0.18)
    stack_h = n_legends * row_h + max(0, n_legends - 1) * row_gap_in
    total_w = bar_width_in + 2 * pad_in
    total_h = stack_h + 2 * pad_in
    return (
            total_w,
            total_h,
            pad_in,
            bar_width_in,
            bar_height_in,
            title_height_in,
            title_gap_in,
            row_h,
            row_gap_in,
            )


def _combined_legend_entries(datasets):
    return [(label, cmap_name) for label, _, _, cmap_name, _ in datasets]


def save_gradient_legends_figure(output_base, legend_entries, rw_min=50,
                                 filename_suffix="",
                                 ):
    """Save RD:WR gradient legend(s) as standalone <output_base>.legends.pdf/.png.

    ``legend_entries`` is a list of ``(title, cmap_name)`` rows: a single row
    with ``title=None`` gives the shared legend for a dataset's individual
    charts; one titled row per dataset gives the combined-chart legend.
    """
    if not legend_entries:
        return


    max_legends_per_page = 12
    if len(legend_entries) > max_legends_per_page:
        for page_index, start in enumerate(
                range(0, len(legend_entries), max_legends_per_page), start=1):
            page_entries = legend_entries[start:start + max_legends_per_page]
            save_gradient_legends_figure(
                    f"{output_base}.page{page_index}",
                    page_entries,
                    rw_min,
                    filename_suffix=filename_suffix,
                    )
        return

    legend_pdf = output_base + '.legends' + filename_suffix + '.pdf'
    legend_png = output_base + '.legends' + filename_suffix + '.png'

    legend_fs = _legend_fontsize()
    n = len(legend_entries)
    with_titles = any(title for title, _ in legend_entries)
    layout = _legend_only_layout_inches(n, legend_fs, with_titles)
    total_w, total_h, pad_in, bar_w, bar_h, title_h, title_gap, row_h, row_gap = layout

    fig = plt.figure(figsize=(total_w, total_h))
    legend_axes = []
    title_artists = []
    x_center = (pad_in + bar_w / 2.0) / total_w

    for i, (dataset_name, cmap_name) in enumerate(legend_entries):
        row_top_in = total_h - pad_in - i * (row_h + row_gap)
        bar_bottom_in = row_top_in - row_h + title_h + title_gap
        left_frac = pad_in / total_w
        bottom_frac = bar_bottom_in / total_h
        width_frac = bar_w / total_w
        height_frac = bar_h / total_h
        cbar_ax = fig.add_axes([left_frac, bottom_frac, width_frac, height_frac])
        _draw_gradient_legend_bar(cbar_ax, cmap_name, rw_min, 100, legend_fs)
        legend_axes.append(cbar_ax)

        if dataset_name:
            title_y = (bar_bottom_in + bar_h + title_gap + title_h / 2.0) / total_h
            title_artists.append(
                    fig.text(
                        x_center,
                        title_y,
                        dataset_name,
                        ha='center',
                        va='center',
                        fontsize=legend_fs,
                        color='black',
                        clip_on=False,
                        )
                    )

    save_kw = dict(
            dpi=300,
            bbox_inches='tight',
            bbox_extra_artists=legend_axes + title_artists,
            pad_inches=0.06,
            )
    fig.savefig(legend_pdf, facecolor='white', **save_kw)
    fig.savefig(legend_png, transparent=True, **save_kw)
    plt.close(fig)


def filter_clustered_points(
    df,
    bw_tolerance_gb=2.0,
    min_latency_increase_percent=5.0,
    bw_col='bandwidth_smooth',
    lat_col='latency_smooth'
):
    """
    Two-region point decimation:
    - Flat region  (latency within min_latency_increase_percent of the base): keep by
      BW spacing only so the low-BW shape is faithfully represented.
    - Knee/saturation region (latency has risen beyond the threshold): require both
      sufficient BW increase AND sufficient latency increase to thin out the dense
      cluster near saturation.
    """
    if df.empty or len(df) <= 1:
        return df

    if bw_col not in df.columns or lat_col not in df.columns:
        return df

    df = df.sort_values(bw_col).reset_index(drop=True)

    threshold = min_latency_increase_percent / 100.0
    base_lat = df.iloc[0][lat_col]   # lowest-BW point carries the baseline latency

    keep_idx = [0]
    last_bw = df.iloc[0][bw_col]
    last_lat = base_lat

    for i in range(1, len(df)):
        current_bw = df.iloc[i][bw_col]
        current_lat = df.iloc[i][lat_col]

        bw_ok = (current_bw - last_bw) >= bw_tolerance_gb

        # Flat region: latency has not yet risen meaningfully from the base
        in_flat = (base_lat > 0 and
                   (current_lat - base_lat) / base_lat < threshold)

        if in_flat:
            # BW spacing alone is sufficient — preserve the flat curve shape
            if bw_ok:
                keep_idx.append(i)
                last_bw = current_bw
                last_lat = current_lat
        else:
            # Knee/saturation: thin out by requiring both BW and latency to grow
            lat_ok = (last_lat > 0 and current_lat > last_lat and
                      (current_lat - last_lat) / last_lat >= threshold)
            if bw_ok and lat_ok:
                keep_idx.append(i)
                last_bw = current_bw
                last_lat = current_lat

    return df.iloc[keep_idx].reset_index(drop=True)


def filter_large_bw_jumps(df, bw_col='bandwidth_smooth', gap_fraction=0.4):
    """
    Drop the smaller BW segment separated by a large gap.

    Raw-mode cleanup: removes spurious low-BW points (e.g. ~0 GB/s) that would
    otherwise draw a diagonal line into the main curve.
    """
    if df.empty or len(df) <= 1:
        return df
    if bw_col not in df.columns:
        return df

    df = df.sort_values(bw_col).reset_index(drop=True)

    while len(df) > 1:
        bw = df[bw_col].to_numpy(dtype=float)
        gaps = np.diff(bw)
        span = bw[-1] - bw[0]
        if span <= 0:
            return df

        max_allowed = span * gap_fraction
        max_gap_idx = int(np.argmax(gaps))
        if gaps[max_gap_idx] <= max_allowed:
            return df

        left = df.iloc[:max_gap_idx + 1]
        right = df.iloc[max_gap_idx + 1:]
        if len(right) > len(left):
            df = right
        elif len(left) > len(right):
            df = left
        else:
            df = right if right[bw_col].max() >= left[bw_col].max() else left
        df = df.reset_index(drop=True)

    return df


def is_valid_curve(df,
                   min_points=10,
                   max_cluster_fraction=0.4,
                   gap_fraction=0.5,        # NEW: max allowed jump in GB/s
                   bw_column='bandwidth_mean',
                   ignore_guards=False):

    if ignore_guards:
        return True, "valid (forced)"

    if bw_column not in df.columns:
        if 'bandwidth_smooth' in df.columns:
            bw_column = 'bandwidth_smooth'
        elif 'bandwidth_mean' in df.columns:
            bw_column = 'bandwidth_mean'
        elif 'bandwidth' in df.columns:
            bw_column = 'bandwidth'
        else:
            return False, "missing_bandwidth_column"

    bw = df[bw_column].dropna()

    bw_sorted = np.sort(bw)
    gaps = np.diff(bw_sorted)
    max_gap = gaps.max() if len(gaps) > 0 else 0
    span = bw.max() - bw.min()
    max_horizontal_gap = span * gap_fraction
    

    if max_gap > max_horizontal_gap:
        return False, f"large_jump ({max_gap:.1f} > {max_horizontal_gap} GB/s)"


    return True, "valid"


def _compute_theoretical_max_bw(config):
    try:
        if _is_intel_pcm_config(config):
            return 72.0

        nvlink_bw_str = config.get("NVLINK_BW_GB_S", None)
        nvlink_bw = float(nvlink_bw_str) if nvlink_bw_str is not None else None

        if nvlink_bw is not None and nvlink_bw == 0:
            max_bw = 0
        elif nvlink_bw is not None and nvlink_bw > 0:
            max_bw = nvlink_bw
        else:
            binding = config.get("MEMORY_BINDING", "local").strip().lower()
            if binding == "remote":
                upi_freq = float(config.get("UPI_FREQ", 16))
                n_lanes = float(config.get("N_DATA_LANES", 20))
                flit_bit = float(config.get("FLIT_BIT", 80))
                data_flit_bit = float(config.get("DATA_FLIT_BIT", 64))
                n_upi_channels = float(config.get("N_UPI_CHANNELS", 4))

                if data_flit_bit > 0 and flit_bit > 0:
                    max_bw = 2.0 * (upi_freq * n_lanes * (data_flit_bit / flit_bit) * n_upi_channels) / 8.0
                else:
                    max_bw = 0
            else:
                freq_str = config.get("MEM_FREQ", "")
                if freq_str and "Could not detect" not in freq_str:
                    freq = float(freq_str.split()[0])
                else:
                    freq = 0

                channels = float(config.get("N_CHANNELS", 2))
                bus_width = float(config.get("BUS_WIDTH", 64))

                if bus_width > 0 and freq > 0:
                    max_bw = (bus_width / 8) * freq * channels / 1000.0
                else:
                    max_bw = 0
    except (ValueError, AttributeError):
        max_bw = 0

    return max_bw


_MEMORY_SIGNATURE_KEYS = (
        'MEMORY_BINDING',
        'MEM_FREQ',
        'N_CHANNELS',
        'BUS_WIDTH',
        'UPI_FREQ',
        'N_DATA_LANES',
        'FLIT_BIT',
        'DATA_FLIT_BIT',
        'N_UPI_CHANNELS',
        'NVLINK_BW_GB_S',
        )

_MEMORY_SYSTEM_REF_CMAPS = ('Blues', 'Greens')


def _memory_system_signature(config):
    """Keys from plotter.txt that determine theoretical memory bandwidth."""
    return tuple(str(config.get(key, '')).strip() for key in _MEMORY_SIGNATURE_KEYS)


def _datasets_share_memory_config(datasets):
    signatures = [_memory_system_signature(cfg) for _, _, cfg, _, _ in datasets]
    return len(set(signatures)) <= 1


def _memory_system_signature_ranks(datasets):
    """Rank unique memory configs by theoretical max BW (0 = highest)."""
    sig_to_bw = {}
    for _, _, cfg, _, _ in datasets:
        sig = _memory_system_signature(cfg)
        if sig not in sig_to_bw:
            sig_to_bw[sig] = _compute_theoretical_max_bw(cfg)
    sorted_sigs = sorted(sig_to_bw.keys(), key=lambda sig: sig_to_bw[sig], reverse=True)
    return {sig: rank for rank, sig in enumerate(sorted_sigs)}


def _memory_system_ref_cmap(config, sig_ranks):
    rank = sig_ranks.get(_memory_system_signature(config), 0)
    return _MEMORY_SYSTEM_REF_CMAPS[min(rank, len(_MEMORY_SYSTEM_REF_CMAPS) - 1)]


def _prepare_combined_datasets_for_memory_compare(datasets):
    """Remap curve colormaps when comparing different memory systems.

    Highest theoretical BW → Blues, lower → Greens.  Unchanged for the regular
    case of datasets sharing the same plotter.txt memory settings.
    """
    if _datasets_share_memory_config(datasets):
        return datasets

    sig_ranks = _memory_system_signature_ranks(datasets)
    return [
            (label, dfs_rw, cfg, _memory_system_ref_cmap(cfg, sig_ranks), measuring_dir)
            for label, dfs_rw, cfg, _cmap_name, measuring_dir in datasets
            ]


def _combined_theoretical_max_bw_specs(datasets):
    """Build max-BW reference line specs for combined plots.

    When every dataset shares the same memory plotter.txt settings, return a
    single neutral line.  When memory systems differ, return one coloured line
    per unique config: highest theoretical BW in blue, lower in green (so the
    reference colours do not clash with red/blue curve colormaps).
    """
    if _datasets_share_memory_config(datasets):
        max_bw = max(_compute_theoretical_max_bw(cfg) for _, _, cfg, _, _ in datasets)
        if max_bw <= 0:
            return []
        return [(max_bw, calculate_color(75), f'Max. theoretical BW = {int(max_bw)} GB/s')]

    sig_ranks = _memory_system_signature_ranks(datasets)
    unique = []
    seen = set()
    for _, _, cfg, _, _ in datasets:
        sig = _memory_system_signature(cfg)
        if sig in seen:
            continue
        seen.add(sig)
        max_bw = _compute_theoretical_max_bw(cfg)
        if max_bw > 0:
            unique.append((sig_ranks[sig], max_bw))

    if not unique:
        return []

    unique.sort(key=lambda item: item[0])
    specs = []
    for rank, max_bw in unique:
        cmap_name = _MEMORY_SYSTEM_REF_CMAPS[min(rank, len(_MEMORY_SYSTEM_REF_CMAPS) - 1)]
        color = calculate_color(75, cmap_name)
        specs.append((max_bw, color, f'Max. theoretical BW = {int(max_bw)} GB/s'))
    return specs


def _should_draw_combined_max_bw_line(datasets, max_bw, global_max_bw):
    is_any_nvidia_pmu = any(
            cfg.get("USE_NVIDIA_PMU", "False").lower() in ('true', '1', 't')
            for _, _, cfg, _, _ in datasets)
    any_nvlink_bw = any(float(cfg.get("NVLINK_BW_GB_S", 0)) > 0 for _, _, cfg, _, _ in datasets)
    if max_bw <= 0 or (global_max_bw > max_bw * 1.5):
        return False
    return not (is_any_nvidia_pmu and not any_nvlink_bw)


def _draw_combined_theoretical_max_bw(ax, datasets, limit_bw, y_top, max_bw_val, global_max_bw, *, label_x_offset_frac=0.005,
                                      use_bbox=True, linewidth_scale=1.5):
    """Draw one or more theoretical max-BW vertical reference lines.

    Returns True when at least one line and label were drawn, so callers can
    keep other top-of-chart labels clear of them.
    """
    if not _should_draw_combined_max_bw_line(datasets, max_bw_val, global_max_bw):
        return False

    specs = _combined_theoretical_max_bw_specs(datasets)
    if not specs:
        return False

    label_fs = fontSizeMess - 4
    text_kw = dict(
            verticalalignment='top',
            fontsize=label_fs,
            zorder=10,
            )
    if use_bbox:
        text_kw['bbox'] = dict(facecolor='white', edgecolor='none', alpha=1.0, pad=2)

    for max_bw, color, _label in specs:
        ax.axvline(
                x=max_bw,
                color=color,
                linewidth=refLineWidth * linewidth_scale,
                linestyle=':',
                zorder=10,
                )

    if len(specs) == 1:
        max_bw, color, label = specs[0]
        label_color = 'black' if _datasets_share_memory_config(datasets) else color
        ax.text(
                x=max_bw - limit_bw * label_x_offset_frac,
                y=y_top * 0.95,
                s=label,
                color=label_color,
                horizontalalignment='right',
                **text_kw,
                )
        return True

    # Several memory systems: anchor labels in the top-right corner so long
    # strings do not overlap when the vertical reference lines are close.
    anchor_x = limit_bw * (1.0 - label_x_offset_frac) if label_x_offset_frac else limit_bw * 0.99
    y_step = max(0.08, (label_fs / y_top) * 1.15) if y_top > 0 else 0.08
    for i, (_max_bw, color, label) in enumerate(specs):
        ax.text(
                x=anchor_x,
                y=y_top * (0.97 - i * y_step),
                s=label,
                color=color,
                horizontalalignment='right',
                **text_kw,
                )
    return True


def _draw_max_bw_line(ax, config, max_bw, limit_bw, y_top, cmap_name, global_max_bw=0):
    """Vertical theoretical max-BW reference line, same look on every chart.

    Returns True when the line and its label were drawn, so callers can keep
    other top-of-chart labels clear of the label strip.
    """
    is_nvidia_pmu = config.get("USE_NVIDIA_PMU", "False").lower() in ('true', '1', 't')
    nvlink_bw = float(config.get("NVLINK_BW_GB_S", 0))
    # Do not draw the line for NVIDIA counters if NVLINK_BW is not specified, or if max_bw is clearly wrong (e.g. CPU peak for GPU)
    if max_bw <= 0 or (is_nvidia_pmu and nvlink_bw == 0) or (global_max_bw > max_bw * 1.5):
        return False
    ax.axvline(x=max_bw, color=calculate_color(75, cmap_name), linewidth=refLineWidth * 1.5, linestyle=':', zorder=10)
    ax.text(x=max_bw - limit_bw * 0.005, y=y_top * 0.95,
            s=f'Max. theoretical BW = {int(max_bw)} GB/s',
            horizontalalignment='right', verticalalignment='top',
            fontsize=fontSizeMess - 4, zorder=10,
            bbox=dict(facecolor='white', edgecolor='none', alpha=1.0, pad=2))
    return True


def _bw_limit(max_bw, global_max_bw, override=None):
    if override is not None:
        return override
    if max_bw > 0:
        return max(max_bw, global_max_bw) * 1.1
    return global_max_bw * 1.1 if global_max_bw > 0 else 321


def _line_style(draw_dots, draw_lines):
    dots = draw_dots or not draw_lines
    return {'linewidth': curveLineWidth if draw_lines else 0, 'marker': 'o' if dots else None, 'markersize': markerSize if dots else None}


def _memory_curve_lat_limit(global_max_lat, floor=700.0):
    """Default Y limit for memory latency curves: floor, or 20% above peak if higher."""
    if global_max_lat > 0:
        return max(floor, global_max_lat * 1.2)
    return floor




def plot_curves(config, dfs_rw, output_path='memory_curves.pdf', cmap_name='Blues',
                limit_bw_override=None, limit_lat_override=None, ignore_guards=False,
                apply_corrections=True, y_label=None, draw_dots=False,
                draw_lines=True,
                legend_rw_min=None):
    """Plot bandwidth-latency curves and save to PDF"""
    if not dfs_rw:
        print("No data to plot")
        return

    fig, ax = plt.subplots(1, 1)
    
    max_bw = _compute_theoretical_max_bw(config)
    
    ax.set_xlabel('Used Memory bandwidth [GB/s]', fontsize=fontSizeMess + 3)
    ax.set_ylabel(y_label or 'Memory access latency [ns]', fontsize=fontSizeMess + 3)

    sorting_list = []
    processed_dfs = {}

    global_max_bw = 0.0
    global_max_lat = 0.0
    
    for rw, df in dfs_rw.items():
        if not df.empty:
            if apply_corrections and draw_lines:
                df_clean = filter_clustered_points(df.copy(), 1.0, bw_col='bandwidth_smooth')
            else:
                df_clean = df.copy()
            processed_dfs[rw] = df_clean
            
            if 'bandwidth_smooth' in df_clean.columns and not df_clean['bandwidth_smooth'].empty:
                global_max_bw = max(global_max_bw, df_clean['bandwidth_smooth'].max())
            if 'latency_smooth' in df_clean.columns and not df_clean['latency_smooth'].empty:
                global_max_lat = max(global_max_lat, df_clean['latency_smooth'].max())
                
            sort_value = df_clean['bandwidth_smooth'].max() 
            sorting_list.append((rw, sort_value))

    limit_bw = _bw_limit(max_bw, global_max_bw, limit_bw_override)

    limit_lat = limit_lat_override
    if limit_lat is None:
        limit_lat = _memory_curve_lat_limit(global_max_lat)

    ax.set_xlim([0, limit_bw])
    ax.set_ylim([0, limit_lat])

    style = _line_style(draw_dots, draw_lines)

    rw_min = legend_rw_min if legend_rw_min is not None else _derive_rw_min(processed_dfs)

    guarded = apply_corrections and not ignore_guards
    sorted_ratios = [rw for rw, _ in sorted(sorting_list, key=lambda x: x[0])]
    for rw in sorted_ratios:
        df = processed_dfs[rw]
        if df.empty:
            continue
        if guarded and not is_valid_curve(df, min_points=5, max_cluster_fraction=0.70, gap_fraction=0.7)[0]:
            continue
        ax.plot(df['bandwidth_smooth'], df['latency_smooth'], color=_curve_color(config, rw, cmap_name, rw_min), label=f'Rd:Wr {rw}:0', **style)

    _draw_max_bw_line(ax, config, max_bw, limit_bw, limit_lat, cmap_name, global_max_bw=global_max_bw)


    ax.tick_params(axis='both', labelsize=fontSizeMess)

    _save_main_figure(fig, output_path)




def plot_combined_curves(datasets, output_path='combined_curves.pdf',
                         limit_bw_override=None, limit_lat_override=None,
                         ignore_guards=False, apply_corrections=True,
                         draw_dots=False, draw_lines=True,
                         legends_output_base=None, legend_rw_min=None):
    """
    Plot multiple datasets on the same figure.
    datasets: list of tuples (label, dfs_rw, config, cmap_name)

    When ``legends_output_base`` is set, one standalone legends figure with a
    titled RD:WR bar per dataset is saved there; it covers every combined
    chart, so no per-chart legend files are written.
    """
    if not datasets:
        print("No datasets to plot")
        return

    use_original_order = True
    if use_original_order:
        datasets = _prepare_combined_datasets_for_memory_compare(datasets)
        datasets = sorted(
            datasets,
            key=lambda d: max(
                (df['bandwidth_smooth'].max() for _, df in d[1].items()
                if not df.empty and 'bandwidth_smooth' in df.columns and not df['bandwidth_smooth'].empty),
                default=0.0,
            ),
            reverse=True,
        )

    fig, ax = plt.subplots(1, 1)

    max_bw = 0.0
    global_max_bw = 0.0
    global_max_lat = 0.0
    for _, dfs_rw, cfg, _, _ in datasets:
        max_bw = max(max_bw, _compute_theoretical_max_bw(cfg))
        for _, df in dfs_rw.items():
            if df.empty:
                continue
            if 'bandwidth_smooth' in df.columns and not df['bandwidth_smooth'].empty:
                global_max_bw = max(global_max_bw, df['bandwidth_smooth'].max())
            if 'latency_smooth' in df.columns and not df['latency_smooth'].empty:
                global_max_lat = max(global_max_lat, df['latency_smooth'].max())

    limit_bw = _bw_limit(max_bw, global_max_bw, limit_bw_override)

    limit_lat = limit_lat_override
    if limit_lat is None:
        limit_lat = _memory_curve_lat_limit(global_max_lat)

    ax.set_xlim([0, limit_bw])
    ax.set_ylim([0, limit_lat])
    ax.set_xlabel('Used Memory bandwidth [GB/s]', fontsize=fontSizeMess + 3)
    ax.set_ylabel('Memory access latency [ns]', fontsize=fontSizeMess + 3)

    style = _line_style(draw_dots, draw_lines)

    rw_min = legend_rw_min if legend_rw_min is not None else _derive_rw_min(rw for _, dfs_rw, _, _, _ in datasets for rw in dfs_rw)

    guarded = apply_corrections and not ignore_guards
    for _, dfs_rw, config, cmap_name, _ in _combined_curve_draw_order(datasets):
        for rw in sorted(dfs_rw.keys()):
            df = dfs_rw[rw]
            if df.empty:
                continue
            df = filter_clustered_points(df.copy(), 5.0) if apply_corrections and draw_lines else df.copy()
            if guarded and not is_valid_curve(df, min_points=5, max_cluster_fraction=0.70, gap_fraction=0.5)[0]:
                continue
            ax.plot(df['bandwidth_smooth'], df['latency_smooth'], color=_curve_color(config, rw, cmap_name, rw_min), alpha=1.0, **style)

    _draw_combined_theoretical_max_bw(ax, datasets, limit_bw, limit_lat, max_bw, global_max_bw)


    ax.tick_params(axis='both', labelsize=fontSizeMess)

    _save_main_figure(fig, output_path)

    if legends_output_base:
        save_gradient_legends_figure(legends_output_base, _combined_legend_entries(datasets), rw_min)




def check_smooth_curves(df_rw,
                       gap_fraction=0.4,
                       bw_column='bandwidth_mean',
                       group_column='read_pct_rounded',
                       ignore_guards=False):

    if ignore_guards:
        return True, "valid (forced)"

    if bw_column not in df_rw.columns:
        if 'bandwidth_smooth' in df_rw.columns:
            bw_column = 'bandwidth_smooth'
        elif 'bandwidth_mean' in df_rw.columns:
            bw_column = 'bandwidth_mean'
        elif 'bandwidth' in df_rw.columns:
            bw_column = 'bandwidth'
        else:
            return False, "missing_bandwidth_column"

    smooth = True
    problematic_groups = []
    if 'read_pct_rounded' not in df_rw.columns: 
        group_column="rd_percentage"
    for read_pct, group in df_rw.groupby(group_column):

        bw = group[bw_column].dropna()

        if len(bw) < 2:
            continue  # nothing to compare

        bw_sorted = np.sort(bw)

        gaps = np.diff(bw_sorted)
        max_gap = gaps.max()
        span = bw_sorted.max() - bw_sorted.min()

        if span == 0:
            continue

        max_allowed_gap = span * gap_fraction

        if max_gap > max_allowed_gap:
            smooth = False
            problematic_groups.append(read_pct)

    return smooth, problematic_groups
