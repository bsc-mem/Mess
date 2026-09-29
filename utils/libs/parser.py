import os
import re
import pandas as pd

from .latency_timing import read_cpu_frequency_ghz, resolve_pointer_chase_timing, point_timing_alternatives, saved_direct_latency_ns



_TLB_EVENT_TOKEN_RE = re.compile(r'\b(r[0-9a-fA-F]+)(:u)?\b')

_ARM_PAGEWALK_SLOT1 = {'r2d', 'r34'}
_AMD_PAGEWALK_SLOT1 = {'rf045'}


def _strip_perf_suffix(label):
    return label[:-2] if label.endswith(':u') else label


def _classify_tlb_formula(slot1_label, slot2_label):
    """Pick the latency formula tag based on the events programmed in each slot.

    Structural decision only -- the per-walk latency (a numeric calibration
    constant) is read separately from plotter.txt's PAGE_WALK_LAT_NS.
    """
    s1 = _strip_perf_suffix(slot1_label or '').lower()
    _ = _strip_perf_suffix(slot2_label or '').lower()

    if s1 in _ARM_PAGEWALK_SLOT1:
        return 'count_derived'
    if s1 in _AMD_PAGEWALK_SLOT1:
        return 'count_direct'

    # Intel default: slot1 = page-walk *cycles* (e.g. r1008, r1012), slot2 =
    # STLB hit count (e.g. r2008, r2012).
    return 'intel_cycles'


def _compute_tlb_overhead_ns(slot1, slot2, freq_ghz, stlb_hit_latency_ns,
                             formula_tag, page_walk_latency_ns):
    """Mirror of BandwidthCounterStrategy::computeTlbOverheadNs (see C++)."""
    if formula_tag == 'intel_cycles':
        pagewalk_ns = (slot1 / freq_ghz) if freq_ghz > 0.0 else 0.0
        return pagewalk_ns + stlb_hit_latency_ns * slot2

    if formula_tag == 'count_derived':
        stlb_hits = max(0.0, slot2 - slot1)
    else:  # count_direct
        stlb_hits = slot2

    pagewalk_ns = page_walk_latency_ns * slot1 if page_walk_latency_ns > 0.0 else 0.0
    return pagewalk_ns + stlb_hit_latency_ns * stlb_hits

def curves_to_json_compact(
    dfs_rw,
    output_json: str,
    bw_col: str = "bandwidth_smooth",
    lat_col: str = "latency_smooth",
) -> None:
    """Write the plotted curves ({ratio: dataframe}) as compact JSON.

    Rows keep the order in which they are drawn (pause-sweep order), so the
    JSON mirrors exactly the curves rendered in memory_curves.pdf.
    """
    groups = {}
    for ratio, df in dfs_rw.items():
        if bw_col not in df.columns or lat_col not in df.columns:
            continue
        sub = df[[bw_col, lat_col]].dropna()
        if sub.empty:
            continue
        groups[int(round(float(ratio)))] = sub.values.tolist()

    with open(output_json, "w") as f:
        f.write("{\n")
        for i, (grp, pairs) in enumerate(sorted(groups.items())):
            f.write(f'    "{grp}": [\n')
            for j, (bw, lat) in enumerate(pairs):
                comma = "," if j < len(pairs) - 1 else ""
                f.write(f"        [{bw}, {lat}]{comma}\n")
            end_comma = "," if i < len(groups) - 1 else ""
            f.write(f"    ]{end_comma}\n")
        f.write("}\n")



def _extract_counter_value(parts, cast=int):
    """Extract a counter value from a perf-style line's whitespace-split tokens.

    Handles two layouts:
      * multi-socket: ``S0  <count>  <value>  ...`` (e.g. perf-stat --per-socket)
      * single-socket: ``<value>  <name>  ...``
    Returns ``None`` if the value cannot be parsed.
    """
    try:
        if len(parts) >= 4 and parts[0] == 'S0':
            return cast(parts[2])
        if len(parts) >= 2:
            return cast(parts[0])
    except (ValueError, IndexError):
        pass
    return None


def _detect_imc_scale_factor(filepath, cache_line_size):
    try:
        dirpath = os.path.dirname(filepath)
        zero_files = [f for f in os.listdir(dirpath) if f.endswith('_0.txt')]
        if not zero_files:
            return cache_line_size

        zero_filepath = os.path.join(dirpath, zero_files[0])
        with open(zero_filepath, 'r') as f:
            lines = f.readlines()

        rd_val = None
        wr_val = None
        runtime = None
        for line in lines:
            if 'S1' in line:
                continue
            line_lower = line.lower()
            if ('uncore_imc' in line_lower or 'imc_dram' in line_lower
                    or 'uprof_umc' in line_lower):
                val = _extract_counter_value(line.split())
                if val is not None:
                    if 'data_reads' in line_lower:
                        rd_val = val
                    elif 'data_writes' in line_lower:
                        wr_val = val
            elif 'seconds time elapsed' in line_lower:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        runtime = float(parts[0])
                    except ValueError:
                        pass
                break

        if rd_val is not None and wr_val is not None and runtime is not None and runtime > 0:
            total_counts = rd_val + wr_val
            if (total_counts / runtime) > 2.0e9:
                return 1.0
            else:
                return cache_line_size
    except Exception:
        pass
    return cache_line_size


def _issued_read_fraction(rd_percentage_from_filename):
    if rd_percentage_from_filename is None:
        return 1.0
    return max(0.0, min(1.0, rd_percentage_from_filename / 100.0))


def parse_bandwidth_file(filepath, config, rd_percentage_from_filename=None):
    try:
        try:
            cache_line_size_init = int(config.get('CACHE_LINE_SIZE', 64))
        except (ValueError, TypeError):
            cache_line_size_init = 64
        imc_scale_factor = _detect_imc_scale_factor(filepath, cache_line_size_init)

        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        rd_counts = []
        wr_counts = []
        combined_counts = []
        runtimes = []
        pcm_measurements = []
        current_pcm_read_mbps = None
        current_pcm_write_mbps = None
        current_pcm_total_mbps = None
        counter_type = None
        file_has_bus_access = any('bus_access' in line.lower() for line in lines)
        

        for line in lines:
            if 'S1' in line:
                continue
            
            line_lower = line.lower()

            if 'pcm_cxl_read_mbps' in line_lower or 'pcm_cxl_write_mbps' in line_lower or 'pcm_cxl_total_mbps' in line_lower:
                value = _extract_counter_value(line.split(), cast=float)
                if value is not None:
                    if 'pcm_cxl_read_mbps' in line_lower:
                        current_pcm_read_mbps = value
                    elif 'pcm_cxl_write_mbps' in line_lower:
                        current_pcm_write_mbps = value
                    elif 'pcm_cxl_total_mbps' in line_lower:
                        current_pcm_total_mbps = value
                continue

            is_bus_rd = 'bus_access' in line_lower and ('rd' in line_lower or 'read' in line_lower)
            is_bus_wr = 'bus_access' in line_lower and ('wr' in line_lower or 'write' in line_lower)
            is_amd_combined = 'dram_channel_data_controller' in line_lower
            is_upi_rx = ('rxl_flits' in line_lower and 'all_data' in line_lower) or \
                        ('data_from_remote_dram' in line_lower) or \
                        ('amd_df' in line_lower and ('remote' in line_lower))
            is_upi_tx = ('txl_flits' in line_lower and 'all_data' in line_lower)
            is_cas_rd = ('cas_count' in line_lower and ('rd' in line_lower or 'read' in line_lower)) or \
                        ('amd_umc' in line_lower and ('rd' in line_lower or 'read' in line_lower)) or \
                        ('data_from_local_dram' in line_lower)
            is_cas_wr = ('cas_count' in line_lower and ('wr' in line_lower or 'write' in line_lower)) or \
                        ('amd_umc' in line_lower and ('wr' in line_lower or 'write' in line_lower))
            is_imc = ('uncore_imc' in line_lower or 'imc_dram' in line_lower
                      or 'uprof_umc' in line_lower)
            is_imc_rd = is_imc and 'data_reads' in line_lower
            is_imc_wr = is_imc and 'data_writes' in line_lower
            is_grace_rd = 'cmem_rd' in line_lower or 'remote_socket_rd_data' in line_lower
            is_grace_wr = 'cmem_wr' in line_lower or 'remote_socket_wr_total_bytes' in line_lower
            is_gpu_rd = 'dram__bytes_read.sum.per_second' in line_lower
            is_gpu_wr = 'dram__bytes_write.sum.per_second' in line_lower
            is_a64fx_rd = 'a64fx_read_effective' in line_lower
            is_a64fx_wr = 'a64fx_write_effective' in line_lower
            is_amc_rd = 'amc.dcs_rd' in line_lower
            is_amc_wr = 'amc.dcs_wr' in line_lower
            is_runtime_line = 'seconds time elapsed' in line

            if file_has_bus_access and not (is_bus_rd or is_bus_wr or is_runtime_line):
                continue

            counter_dispatch = (
                (is_bus_rd,       'BUS_ACCESS',   rd_counts),
                (is_bus_wr,       'BUS_ACCESS',   wr_counts),
                (is_amd_combined, 'AMD_COMBINED', combined_counts),
                (is_upi_rx,       'UPI_FLITS',    rd_counts),
                (is_upi_tx,       'UPI_FLITS',    wr_counts),
                (is_cas_rd,       'CAS_COUNT',    rd_counts),
                (is_cas_wr,       'CAS_COUNT',    wr_counts),
                (is_imc_rd,       'IMC_COUNT',    rd_counts),
                (is_imc_wr,       'IMC_COUNT',    wr_counts),
                (is_grace_rd,     'NVIDIA_GRACE', rd_counts),
                (is_grace_wr,     'NVIDIA_GRACE', wr_counts),
                (is_gpu_rd,       'GPU_NCU',      rd_counts),
                (is_gpu_wr,       'GPU_NCU',      wr_counts),
                (is_a64fx_rd,     'A64FX',        rd_counts),
                (is_a64fx_wr,     'A64FX',        wr_counts),
                (is_amc_rd,       'APPLE_AMC',    rd_counts),
                (is_amc_wr,       'APPLE_AMC',    wr_counts),
            )

            matched = False
            for matched_flag, tag, target in counter_dispatch:
                if not matched_flag:
                    continue
                if counter_type is None:
                    counter_type = tag
                if tag == 'GPU_NCU':
                    parts = line.split()
                    if len(parts) >= 4 and parts[0] == 'S0':
                        try:
                            target.append(float(parts[2].replace(',', '')))
                        except ValueError:
                            pass
                    elif len(parts) >= 2:
                        try:
                            target.append(float(parts[0].replace(',', '')))
                        except ValueError:
                            pass
                else:
                    value = _extract_counter_value(line.split())
                    if value is not None:
                        target.append(value)
                matched = True
                break

            if matched:
                continue

            if is_runtime_line:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        runtimes.append(float(parts[0]))
                    except ValueError:
                        pass
                if (current_pcm_total_mbps is not None) or \
                   (current_pcm_read_mbps is not None and current_pcm_write_mbps is not None):
                    has_split_pcm = current_pcm_read_mbps is not None and current_pcm_write_mbps is not None
                    read_mbps = current_pcm_read_mbps if current_pcm_read_mbps is not None else 0.0
                    write_mbps = current_pcm_write_mbps if current_pcm_write_mbps is not None else 0.0
                    total_mbps = current_pcm_total_mbps if current_pcm_total_mbps is not None else (read_mbps + write_mbps)
                    if has_split_pcm:
                        rd_percentage_actual = (read_mbps / total_mbps) if total_mbps > 0.0 else 0.0
                    else:
                        rd_percentage_actual = _issued_read_fraction(rd_percentage_from_filename)
                    try:
                        runtime_value = float(parts[0])
                    except (ValueError, IndexError):
                        runtime_value = 0.0
                    pcm_measurements.append({
                        'bandwidth': total_mbps / 1000.0,
                        'rd_percentage_actual': rd_percentage_actual,
                        'bandwidth_counter_combined': not has_split_pcm,
                        'runtime': runtime_value
                    })
                    current_pcm_read_mbps = None
                    current_pcm_write_mbps = None
                    current_pcm_total_mbps = None
        
        if pcm_measurements:
            return pcm_measurements
        
        if counter_type == 'AMD_COMBINED' and combined_counts and runtimes:
            min_len = min(len(combined_counts), len(runtimes))
            measurements = []
            for i in range(min_len):
                total = combined_counts[i]
                runtime = runtimes[i]
                
                if runtime <= 0:
                    continue
                
                rd_percentage_actual = _issued_read_fraction(rd_percentage_from_filename)
                
                try:
                    cache_line_size = int(config.get('CACHE_LINE_SIZE', 64))
                except (ValueError, TypeError):
                    cache_line_size = 64

                bandwidth = total * cache_line_size / runtime / 1000 / 1000 / 1000
                
                measurements.append({
                    'bandwidth': bandwidth,
                    'rd_percentage_actual': rd_percentage_actual,
                    'bandwidth_counter_combined': True,
                    'runtime': runtime
                })
            
            return measurements
        
        min_len = min(len(rd_counts), len(wr_counts), len(runtimes))
        
        if min_len > 0:
            measurements = []
            for i in range(min_len):
                rd = rd_counts[i]
                wr = wr_counts[i]
                runtime = runtimes[i]
                
                if runtime <= 0:
                    continue
                
                total_counts = rd + wr
                rd_percentage_actual = (rd / total_counts) if total_counts > 0 else 0
                
                try:
                    cache_line_size = int(config.get('CACHE_LINE_SIZE', 64))
                except (ValueError, TypeError):
                    cache_line_size = 64

                if counter_type == 'UPI_FLITS':
                    try:
                        scaling_factor = float(config.get('UPI_SCALING_FACTOR', 9.0))
                    except (ValueError, TypeError):
                        scaling_factor = 9.0
                    bandwidth = (total_counts / scaling_factor) * cache_line_size / runtime / 1000 / 1000 / 1000
                elif counter_type == 'NVIDIA_GRACE':
                    # NVIDIA Grace: Reads are 32B chunks, Writes are bytes
                    bandwidth = (rd * 32.0 + wr) / runtime / 1000 / 1000 / 1000
                    rd = rd*32.0
                    total_counts = (rd) + wr
                    rd_percentage_actual = (rd / total_counts) if total_counts > 0 else 0
                elif counter_type == 'GPU_NCU':
                    # GPU_NCU provides values directly in bytes per second
                    bandwidth = total_counts / 1000 / 1000 / 1000
                elif counter_type == 'IMC_COUNT':
                    bandwidth = total_counts * imc_scale_factor / runtime / 1000 / 1000 / 1000
                elif counter_type == 'APPLE_AMC':
                    try:
                        bytes_per_count = int(config.get('CACHE_LINE_SIZE', 1))
                    except (ValueError, TypeError):
                        bytes_per_count = 1
                    bandwidth = total_counts * bytes_per_count / runtime / 1000 / 1000 / 1000
                else:
                    bandwidth = total_counts * cache_line_size / runtime / 1000 / 1000 / 1000
                
                measurements.append({
                    'bandwidth': bandwidth,
                    'rd_percentage_actual': rd_percentage_actual,
                    'runtime': runtime
                })
            
            return measurements
        else:
            pass
    except Exception as e:
        pass
    
    return None

PEBS_LATENCY_METRICS = ('mean_ns', 'iqr_mean_ns', 'min_ns', 'median_ns', 'p90_ns', 'p95_ns', 'p99_ns', 'p99_9_ns', 'max_ns')

def parse_latency_file(filepath, config):
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        measurements = []
        
        is_pebs = any('mean_ns' in line for line in lines)
        if is_pebs:
            current = {}
            for line in lines:
                stripped = line.strip()
                if not stripped:
                    continue
                if 'seconds time elapsed' in stripped:
                    parts = stripped.split()
                    if parts:
                        try:
                            current['seconds'] = float(parts[0])
                        except ValueError:
                            pass
                    if 'iqr_mean_ns' in current:
                        current.setdefault('latency', current['iqr_mean_ns'])
                        measurements.append(current.copy())
                    elif 'mean_ns' in current:
                        current.setdefault('latency', current['mean_ns'])
                        measurements.append(current.copy())
                    current = {}
                    continue
                parts = stripped.split()
                if len(parts) >= 2:
                    try:
                        value = float(parts[0])
                    except ValueError:
                        continue
                    metric = parts[1]
                    if metric == 'samples':
                        current['samples'] = int(value)
                    elif metric in PEBS_LATENCY_METRICS:
                        current[metric] = value
                    elif metric.endswith('_cycles') or metric == 'point_cpu_freq_ghz':
                        current[metric] = value
            return measurements if measurements else None

        i = 0
        while i < len(lines):
            line = lines[i].strip()
            
            if not line or 'S1' in line:
                i += 1
                continue
                
            if any(token in line for token in ['cycles:u', 'instructions:u', 'cycles', 'instructions'])\
               or _TLB_EVENT_TOKEN_RE.search(line):
                cycles = None
                instructions = None
                tlb_values = []   # [(value, label)] in file order: slot1 then slot2
                accesses = None
                seconds_user = None
                seconds_cpu = None
                timing_metrics = {}

                j = i
                while j < len(lines):
                    block_line = lines[j].strip()
                    # End a malformed/incomplete block before consuming the next one.
                    if j > i and (block_line.startswith("Performance counter stats") or
                                  (cycles is not None and re.search(r"\bcycles(?::u)?$", block_line))):
                        break
                    parts = block_line.split()
                    if len(parts) == 2 and parts[1] in (
                            'point_cpu_freq_ghz', 'pointer_chase_elapsed_ns',
                            'pointer_chase_bursts', 'latency_direct_ns', 'pointer_chase_thread_ns',
                            'cycles_enabled_ns', 'cycles_running_ns'):
                        timing_metrics[parts[1]] = float(parts[0])
                        j += 1
                        continue

                    if 'cycles:u' in block_line or ('cycles' in block_line and 'instructions' not in block_line):
                        parts = block_line.split()
                        for part in parts:
                            try:
                                cycles = int(part)
                                break
                            except ValueError:
                                continue

                    elif 'instructions:u' in block_line or 'instructions' in block_line:
                        parts = block_line.split()
                        for part in parts:
                            try:
                                instructions = int(part)
                                break
                            except ValueError:
                                continue

                    elif 'accesses' in block_line:
                        parts = block_line.split()
                        for part in parts:
                            try:
                                accesses = int(part)
                                break
                            except ValueError:
                                continue

                    elif 'seconds thread cpu time' in block_line:
                        parts = block_line.split()
                        for part in parts:
                            try:
                                seconds_cpu = float(part)
                                break
                            except ValueError:
                                continue

                    elif 'seconds time elapsed' in block_line:
                        parts = block_line.split()
                        for part in parts:
                            try:
                                seconds_user = float(part)
                                break
                            except ValueError:
                                continue

                    else:
                        tlb_match = _TLB_EVENT_TOKEN_RE.search(block_line)
                        if tlb_match and len(tlb_values) < 2:
                            label = tlb_match.group(1)
                            for part in block_line.split():
                                try:
                                    tlb_values.append((int(part), label))
                                    break
                                except ValueError:
                                    continue

                    j += 1
                    if 'seconds time elapsed' in block_line:
                        break

                direct_latency = saved_direct_latency_ns(timing_metrics)
                elapsed_seconds = seconds_cpu if seconds_cpu is not None and seconds_cpu > 0 else seconds_user
                if direct_latency is not None or (elapsed_seconds is not None and elapsed_seconds > 0):
                    tlb1miss, tlb1_label = (tlb_values[0] if len(tlb_values) >= 1 else (0, ''))
                    tlb2miss, tlb2_label = (tlb_values[1] if len(tlb_values) >= 2 else (0, ''))
                    if accesses is None:
                        accesses = instructions if instructions is not None else 0

                    if accesses <= 0 and direct_latency is None:
                        j = i + 1
                        i = j
                        continue

                    try:
                        tlb_hit_latency_ns = float(config.get('TLB_NS', 10.0))
                    except (ValueError, TypeError):
                        tlb_hit_latency_ns = 10.0
                    formula_tag = _classify_tlb_formula(tlb1_label, tlb2_label)
                    try:
                        page_walk_lat_ns = float(config.get('PAGE_WALK_LAT_NS', -1.0))
                    except (ValueError, TypeError):
                        page_walk_lat_ns = -1.0

                    counter_latency = None
                    has_startup_calibration = config.get('CPU_FREQ_SOURCE') == 'startup_probe'
                    if cycles is not None and cycles > 0 and accesses > 0 and (
                            direct_latency is None or has_startup_calibration):
                        try:
                            total_ns, cpu_freq_ghz = resolve_pointer_chase_timing(
                                cycles, seconds_user, config)
                            tlb_overhead_ns = _compute_tlb_overhead_ns(
                                tlb1miss, tlb2miss, cpu_freq_ghz, tlb_hit_latency_ns,
                                formula_tag, page_walk_lat_ns)
                            counter_latency = max(0.0, total_ns - tlb_overhead_ns) / accesses
                        except (ValueError, TypeError):
                            if direct_latency is None:
                                raise
                    elif direct_latency is None and accesses > 0 and elapsed_seconds is not None:
                        counter_latency = elapsed_seconds * 1e9 / accesses

                    # A saved direct measurement already has units of ns/access.
                    # It needs neither frequency conversion nor TLB subtraction.
                    latency = direct_latency if direct_latency is not None else counter_latency
                    alternatives = point_timing_alternatives(
                        cycles or 0, accesses, timing_metrics, config)
                    point_frequency = timing_metrics.get('point_cpu_freq_ghz', 0.0)
                    if point_frequency > 0 and cycles is not None and cycles > 0 and accesses > 0:
                        point_overhead = _compute_tlb_overhead_ns(
                            tlb1miss, tlb2miss, point_frequency, tlb_hit_latency_ns,
                            formula_tag, page_walk_lat_ns)
                        alternatives['latency_point_probe_ns'] = max(
                            0.0, cycles / point_frequency - point_overhead) / accesses
                    measurements.append({
                        **alternatives,
                        'latency_startup_ns': counter_latency,
                        'latency': latency,
                        'cycles': cycles if cycles is not None else 0,
                        'instructions': instructions if instructions is not None else 0,
                        'accesses': accesses,
                        'tlb1miss': tlb1miss,
                        'tlb2miss': tlb2miss
                    })

                i = j
            else:
                i += 1
        
        if measurements:
            return measurements
        else:
            return None
    except Exception as e:
        return None
    return None

def parse_bwlat_file(filepath, config):
    try:
        data_points = []
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    parts = line.split()
                    if len(parts) >= 2:
                        try:
                            bw_mbps = float(parts[0])
                            latency_cycles = float(parts[1])
                            
                            bw_gbps = bw_mbps / 1000.0
                            
                            cpu_freq_ghz = read_cpu_frequency_ghz(config)
                            latency_ns = latency_cycles / cpu_freq_ghz
                            
                            data_points.append({
                                'bandwidth': bw_gbps,
                                'latency': latency_ns
                            })
                        except (ValueError, IndexError):
                            continue
        
        if data_points:
            return data_points
        else:
            pass
    except Exception as e:
        pass
    
    return None

def parse_filename(filename):
    parts = filename.replace('.txt', '').split('_')
    
    if filename.startswith('bwlat_') and len(parts) >= 2:
        try:
            rd_percentage = int(parts[1])
            return rd_percentage, 0
        except (ValueError, IndexError):
            pass
    elif len(parts) >= 3:
        try:
            rd_percentage = int(parts[1])
            pause = int(parts[2])
            return rd_percentage, pause
        except (ValueError, IndexError) as e:
            pass
    
    return None, None
