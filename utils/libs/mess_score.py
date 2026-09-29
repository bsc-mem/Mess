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

"""Mess Score — absolute, hardware-agnostic memory-subsystem quality metric.

For every R/W ratio curve the algorithm:
  1. Normalises axes:
       x_norm = BW / BW_theoretical        (0 … E, where E = max-measured / theoretical)
       y_norm = Latency / Latency_base      (1 at lead-off, rises with load)
  2. Integrates the excess area above the ideal y = 1 line:
       Area_excess = ∫₀ᴱ (y_norm(x) − 1) dx     (trapezoidal rule)
     The integration follows the points in their natural load ordering
     (sorted by descending pause).  Because the curve can fold back
     (BW decreasing at very high load), dx can be negative for
     backward-moving segments — the signed trapezoidal sum accounts
     for this automatically.
  3. Computes a per-ratio score:
       Score_RW = E / (1 + Area_excess) × 100
  4. Combines per-ratio scores into an overall score using a
     weighted geometric mean (equal weights for now).
"""

import json
import math
import numpy as np


# ---------------------------------------------------------------------------
# Per-ratio score
# ---------------------------------------------------------------------------

def compute_ratio_score(bw_raw, lat_raw, bw_theoretical):
    """Compute the Mess Score for a single R/W ratio curve.

    Parameters
    ----------
    bw_raw : array-like
        Raw bandwidth values (GB/s) in their natural load order
        (sorted by descending pause — low load first, high load last).
        The curve may be non-monotonic (BW can fold back at high load).
    lat_raw : array-like
        Corresponding raw latency values (ns), same ordering.
    bw_theoretical : float
        Theoretical peak bandwidth (GB/s) for this memory configuration.

    Returns
    -------
    dict or None
        ``{efficiency, area_excess, score}`` on success, *None* when the
        inputs are too short or the theoretical BW is invalid.
    """
    bw = np.asarray(bw_raw, dtype=float)
    lat = np.asarray(lat_raw, dtype=float)

    if len(bw) < 2 or bw_theoretical <= 0:
        return None

    # --- base latency = minimum measured latency (lead-off) ----------------
    lat_base = np.min(lat)
    if lat_base <= 0:
        return None

    # --- normalise axes ----------------------------------------------------
    x_norm = bw / bw_theoretical              # 0 … E
    y_norm = lat / lat_base                   # 1 at lead-off, rises

    # --- efficiency limit --------------------------------------------------
    efficiency = float(np.max(x_norm))        # E = max_measured / theoretical
    if efficiency <= 0:
        return None

    # --- sort by normalized BW (ascending) ---------------------------------
    order = np.argsort(x_norm)
    x_norm = x_norm[order]
    y_norm = y_norm[order]

    # --- excess area via trapezoidal rule ----------------------------------
    # By sorting by bandwidth, the high-latency fold-back points are integrated
    # with positive dx, which naturally increases the excess area and penalizes
    # the score for backpressure.
    excess = np.maximum(y_norm - 1.0, 0.0)
    area_excess = float(np.trapezoid(excess, x_norm))

    # --- score -------------------------------------------------------------
    score = (efficiency / (1.0 + area_excess)) * 100.0

    return {
        "efficiency": round(efficiency, 6),
        "area_excess": round(area_excess, 6),
        "score": round(score, 4),
    }


# ---------------------------------------------------------------------------
# Aggregate score across ratios
# ---------------------------------------------------------------------------

def compute_mess_score(dfs_rw, config):
    """Compute per-ratio and overall Mess Scores.

    Parameters
    ----------
    dfs_rw : dict[int/float, DataFrame]
        Curve DataFrames keyed by read percentage.  Each frame must
        contain ``bandwidth_mean`` and ``latency_mean`` columns (raw
        per-point averages) and a ``pause`` column for load ordering.
        Falls back to ``bandwidth_smooth``/``latency_smooth`` if the
        raw columns are absent.
    config : dict
        Plotter configuration (parsed from ``plotter.txt``).  Used to
        derive the theoretical peak bandwidth.

    Returns
    -------
    dict
        ``{per_ratio: {ratio: {efficiency, area_excess, score}},
           overall_score: float,
           bw_theoretical_gbs: float}``
        or ``None`` if the score cannot be computed.
    """
    from .visualizer import _compute_theoretical_max_bw

    bw_theoretical = _compute_theoretical_max_bw(config)
    if bw_theoretical <= 0:
        return None

    per_ratio = {}
    for rw_pct, df in sorted(dfs_rw.items()):
        # Prefer raw averaged data; fall back to smoothed if unavailable.
        bw_col = "bandwidth_mean" if "bandwidth_mean" in df.columns else "bandwidth_smooth"
        lat_col = "latency_mean" if "latency_mean" in df.columns else "latency_smooth"

        if bw_col not in df.columns or lat_col not in df.columns:
            continue

        # Sort by descending pause → natural load order (low load first).
        if "pause" in df.columns:
            df_sorted = df.sort_values("pause", ascending=False)
        else:
            df_sorted = df

        result = compute_ratio_score(
            df_sorted[bw_col].values,
            df_sorted[lat_col].values,
            bw_theoretical,
        )
        if result is not None:
            per_ratio[int(rw_pct)] = result

    if not per_ratio:
        return None

    # --- weighted geometric mean (equal weights) ---------------------------
    scores = [v["score"] for v in per_ratio.values()]
    n = len(scores)
    weights = [1.0 / n] * n

    log_sum = sum(w * math.log(max(s, 1e-12)) for w, s in zip(weights, scores))
    overall = math.exp(log_sum)
    if overall > 100:
        print("Warning: bandwidth exceeds theoretical, review results or system detection.")

    return {
        "per_ratio": per_ratio,
        "overall_score": round(overall, 4),
        "bw_theoretical_gbs": round(bw_theoretical, 4),
    }


# ---------------------------------------------------------------------------
# I/O helpers
# ---------------------------------------------------------------------------

def save_mess_score(score_dict, output_path):
    """Write the Mess Score result to a JSON file."""
    with open(output_path, "w") as f:
        json.dump(score_dict, f, indent=2)


def format_mess_score_summary(score_dict):
    """Return a human-readable multi-line summary for terminal output."""
    if score_dict is None:
        return "Mess Score: N/A (theoretical BW unavailable)"

    lines = []
    lines.append(f"Mess Score (overall): {score_dict['overall_score']:.2f}")
    lines.append(f"  Theoretical BW: {score_dict['bw_theoretical_gbs']:.2f} GB/s")
    for rw, info in sorted(score_dict["per_ratio"].items()):
        lines.append(
            f"  Ratio {rw:3d}%: score={info['score']:.2f}  "
            f"E={info['efficiency']:.4f}  "
            f"A_excess={info['area_excess']:.4f}"
        )
    return "\n".join(lines)
