# Changelog

## [2.1.0] - 29-09-2026
_September 29th 2026_

### Added

- **Consumer Platform Support**: Expanded Mess to consumer laptops, desktops and workstations, alongside server systems:
  - **Apple Silicon Macs**: Native macOS bandwidth measurement through IOReport, without an additional counter driver.
  - **Windows x86-64**: Intel VTune, Intel client IMC and AMD uProf bandwidth backends, with vendor tools and Administrator access.
  - **Linux PCs**: Improved backend discovery and fallback for supported Intel and AMD systems with accessible bandwidth counters.
- **Mess GUI**: Desktop interface for configuring runs, launching bandwidth-latency curves and inspecting results. Installers for Linux, macOS (Apple Silicon) and Windows will be available on the [Mess GUI download page](https://memory.bsc.es/downloads/mess-gui/).
- **NVIDIA GPU Benchmark**: Generate GPU bandwidth-latency curves with `--gpu`, configurable read/write ratios and adaptive pause discovery. Requires the NVIDIA driver and CUDA Toolkit 12.6+ with `nvcc` and CUPTI; GPU kernels are compiled at runtime and DRAM counters are collected through CUPTI.
- **Automatic Traffic-Array Calibration**: Select traffic-generator array sizes during kernel generation using measured bandwidth, last-level cache capacity and memory limits, with a fallback when calibration cannot complete.
- **Mess Score**: Plotter summaries now include a score combining bandwidth efficiency and latency growth under load, normalized against theoretical bandwidth and baseline latency.

### Improved

- **Instruction-Latency Sampling**: Expanded the optional `--inst-lat` workflow for measuring latency directly from traffic-generator loads using Intel PEBS or ARM SPE on supported Linux systems, with latency statistics and percentile plots. Improved memory-source filtering for HBM and CXL measurements.
- **ISA Tuning and Assembly Generation**:
  - Expanded scalar and vector kernel choices, including SSE2, AVX, AVX2 and AVX-512 on x86, and native/paired NEON modes on ARM.
  - Improved SVE handling with runtime vector-length selection, maximum-length and fixed-width modes, and vector-length-aware pointer updates.
  - Extended `AddressingPolicy` and `StoreLinePolicy` tuning for address generation and ISA-native or cache-line-sized store operations.
- **Hardware Detection**: Added AMD Zen 5 bandwidth-counter discovery and improved CPU identification, last-level cache capacity detection across cache slices, and Apple Silicon memory-configuration detection.
- **Bandwidth Backends**:
  - **Intel VTune**: Improved discovery, counter aggregation, report parsing and collection fallback; available on Linux and supported Windows systems.
  - **Intel PCM / CXL**: Improved detection of CXL memory-node binding and automatic PCM selection on Intel Linux systems. CXL bandwidth measurement uses the PCM backend and requires supported counters.
- **Adaptive Pause Discovery and Run Tiers**: Refined curve-guided sampling around transitions, with validation of suspicious bends. Select `--tier=lite`, `standard` or `detailed` for point budgets of 15, 50 or 200, or use `--point-count=N` for a custom budget.
- **Reworked Plotter**: Improved curve processing, smoothing and comparison across runs, with CSV/JSON data exports, PDF/PNG figures and instruction-sampling percentile plots.
- **Measurement Stability**: Stabilization now uses theoretical and achievable bandwidth estimates, accounting for worker count and memory placement, to scale warmup and noise thresholds. Improved low-throughput handling and traffic-generator health checks.
- **Code Maintainability and Portability**: Improved separation of platform services, measurement backends and parsers; more reliable process cleanup, bounded command collection, build resource discovery and error reporting.

### Fixed

- **Latency Timing**: Improved CPU-frequency calibration and consistent latency conversion between runtime measurements and the plotter, including elapsed-time fallback when cycle counters are unavailable.
- **Perf Interval Accounting**: Aligned interval counter values with their timestamps to avoid incorrect bandwidth normalization.
- **Curve Processing**: Preserved curves below 50% read ratio, improved latency-outlier and bandwidth-zigzag handling, and restricted zero-bandwidth anchors to stable near-idle data.
- **Build Paths**: Improved handling of paths containing spaces and generated resources when running from a different working directory.

### Compatibility

- CPU bandwidth measurement depends on counters supported by the processor, operating system and selected backend.
- macOS bandwidth measurement requires Apple Silicon. `--bind`, `--inst-lat` and `--add-counters` are unavailable on macOS and Windows.
- Windows backends are implemented; native Windows driver and benchmark-curve validation remains pending.

## [2.0.3] - 28-05-2026
_May 28th 2026_

### Added

- **New Bandwidth Measurers**:
  - **Intel PCM**: Added Intel PCM as a bandwidth measurement backend with CXL compatibility
  - **Expanded LIKWID**: Counter aggregation across multiple CPUs, automatic HBM sub-channel detection (SCHX), extra MBOX events, and full add-counters support
  - **Intel VTune**: Added as an alternative bandwidth measurement backend
- **Instruction Sampling**: New optional instruction sampling infrastructure for fine-grained execution analysis
- **Adaptive Curve-Guided Pause Discovery**: Automatically derives optimal pause timings from the bandwidth curve.

### Improved

- **Platform Support**:
  - **Expanded ISA Support**: Added support for additional ISAs within existing architectures
  - **ARM Systems**: Bandwidth and latency measurements are now significantly more reliable
  - **SubNUMA / HBM Latency**: Improved latency measurements on SubNUMA and HBM systems
  - **CXL Compatibility**: Full support for CXL-attached memory via Intel PCM backend
- **Measurement Stability**:
  - **Bandwidth Stabilization**: Improved warmup handling with automatic bypass for low-throughput scenarios; more robust windowing and steady-state detection
  - **Performance Counters**: Extra counters now measured in the same command as bandwidth counters (reduced overhead); fixed counter value reporting for perf add-counters
- **Topology & Binding**:
  - **Topology Detection**: Enhanced NUMA and CPU topology discovery for more accurate system characterization
  - **Memory Binding**: Improved CPU and memory binding logic with better inheritance from `numactl`/`taskset`
- **Assembly Generation**:
  - New `AddressingPolicy` for flexible memory addressing modes
  - New `StoreLinePolicy` for configurable store line management
- **Curve Processing**: Smoother bandwidth/latency curve output with improved correction and visualization formatting
- **CLI**: Better argument validation, clearer error messages, and improved diagnostics

### Fixed
- **Various Minor bugs**


## [2.0.2] - 09-02-2026
_February 9th 2026_

### Added

- **x86 Scalar Support**: Added a scalar fallback backend for x86, enabling baseline performance comparisons and broader compatibility with legacy hardware.
- **Improved bandwidth measurer**: Enhanced bandwidth measurement infrastructure with better separation of concerns.

### CLI Changes

- **New `--measurer` Flag**: Replaces deprecated `--likwid` flag with explicit tool selection:
  - `--measurer=auto` (default): Automatically select best available tool
  - `--measurer=perf`: Use Linux perf for standard DDR systems
  - `--measurer=likwid`: Use LIKWID for HBM systems
  
- **New `--add-counters` Flag**: Replaces deprecated `--extra-perf` flag:
  - Same functionality: add extra performance counters to measurements
  - More consistent naming with other CLI options
  - Currently only supports perf counters

### Improved

- **Code Maintainability**: Refactored `BandwidthCounterStrategy` and related components to improve code organization and readability, making the codebase easier to maintain and extend.
- **Bandwidth Stability**: Enhanced stability detection logic to properly handle bandwidth ramp-up phases and skip array initialization artifacts, resulting in more reliable steady-state measurements.

### Fixed

- **ARM SVE Assembly**: Resolved instruction scheduling errors in the ARM SVE backend that affected data validity on SVE-capable systems.
- **General Stability**: Fixed minor bugs in argument parsing and topology detection for cleaner execution flows.

### Deprecated

- `--likwid`: Use `--measurer=likwid` instead
- `--extra-perf`: Use `--add-counters` instead

## [2.0.1] - 12-01-2026

_January 12th 2026_

### Added

- **Plotting Utilities**: New Python scripts in `utils/` directory for visualizing benchmark results
  - `app_plotter.py`: Overlay application profiler data on memory curves

### Improved

- **mess-profiler**: Enhanced unified memory bandwidth profiling tool
  - Automatic backend detection (`perf`, `likwid`, or Intel PCM)
  - Consistent output format regardless of underlying tool
  - Automatic inheritance of CPU/memory bindings from `numactl` or `taskset`
  - Added `--dry-run` option to preview counter configuration

## [2.0.0] - 18-12-2025

_December 18th 2025_

Presenting **Mess**, a new approach to using Mess that preserves its logical core while completely rewriting the execution engine to be faster, more portable, and easier to use.

Key highlights:

- **Rewritten entirely in C++**: Native performance and type safety.
- **84x Faster**: Massive validation speedup compared to v1.0.
- **Zero Setup Cost**: No complex dependencies, just plug and play.
- **Universal Portability**: Compiles and runs on all supported ISAs (x86, ARM, Power, RISC-V).
- **Fully Configurable**: All parameters adjustable via CLI arguments.
- **Improved Experience**: More user-friendly with enhanced plotting logic.

### Added

- Rebuilt measurement workflow that keeps bandwidth/latency in sync, shrinking full-system runs from weeks to hours.
- Plain/Profile execution modes plus `--system`, `--ratio`, `--bind`, `--dry-run`, and extended verbosity controls for tailored runs.
- Automatic kernel generation driven by `KernelConfig`, enabling ISA-aware TrafficGenerator and pointer-chase code per architecture.
- Multi-architecture support (x86, ARM, Power, RISC-V) with auto-detected NUMA layouts and system characterization.
- Measurement storage outputs (profiling files, per-kernel data) for plotting bandwidth/latency curves.

### Compatibility

- Mess 1.0 curve formats stay supported so existing workflows continue to work.
