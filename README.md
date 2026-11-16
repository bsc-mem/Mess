<!--
Copyright (c) 2026, Barcelona Supercomputing Center
Contact: mess             [at] bsc [dot] es
         victor.xirau     [at] bsc [dot] es
         mariana.carmin   [at] bsc [dot] es
         pau.diazcuesta   [at] bsc [dot] es
         petar.radojkovic [at] bsc [dot] es
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the copyright holder nor the names
      of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-->

<div align="center">
  <img src="doc/logos/512_mess_white.png" alt="Mess Benchmark Logo" width="180">
  <h1>Mess Benchmark</h1>
  <p><strong>A multiplatform benchmark designed to provide a holistic, detailed, and close-to-hardware view of memory system performance through bandwidth-latency curves.</strong></p>
  <p>
    This is an update to the original
    <a href="https://github.com/bsc-mem/Mess-benchmark">Mess Benchmark</a>
    (<em>now deprecated</em>), focused on improved usability and portability.
  </p>
  <p>
    <a href="https://mess.bsc.es">Website</a> &nbsp;|&nbsp;
    <a href="https://github.com/bsc-mem/Mess">GitHub</a> &nbsp;|&nbsp;
    <a href="https://github.com/bsc-mem/Mess/wiki">Wiki</a> &nbsp;|&nbsp;
    <a href="https://arxiv.org/pdf/2405.10170">Paper</a>
  </p>
  <p>
    <img src="https://img.shields.io/badge/License-BSD%203--Clause-blue.svg" alt="License">
    <img src="https://img.shields.io/badge/Version-2.1.0-blue" alt="Version">
  </p>
</div>

## Table of Contents

- [Documentation](#documentation)
- [Motivation](#motivation)
- [Tools Included](#tools-included)
- [Architecture Support](#architecture-support)
  - [System Availability](#system-availability)
  - [Instruction Sets and GPUs](#instruction-sets-and-gpus)
- [Installation](#installation)
  - [Clone](#clone)
  - [Dependencies](#dependencies)
    - [Linux](#linux)
    - [macOS](#macos)
    - [Windows](#windows)
  - [Build](#build)
  - [Verification](#verification)
- [Quick Start](#quick-start)
- [Graphic User Interface](#graphic-user-interface)
- [Common Workflows](#common-workflows)
- [Mess Profiler](#mess-profiler)
- [Plotter-Parser](#plotter-parser)
- [Learning Resources](#learning-resources)
- [Troubleshooting](#troubleshooting)
- [Contributors](#contributors)
- [Citation](#citation)
- [References](#references)

---

## Documentation

Project documentation is available in the GitHub wiki:

- [Wiki Home](https://github.com/bsc-mem/Mess/wiki/Home)
- [Installation](https://github.com/bsc-mem/Mess/wiki/Installation)
- [Understanding CLI Arguments](https://github.com/bsc-mem/Mess/wiki/Understanding-CLI-Arguments)
- [Mess Benchmark](https://github.com/bsc-mem/Mess/wiki/Mess-Benchmark)
- [Mess Profiler](https://github.com/bsc-mem/Mess/wiki/Mess-Profiler)
- [Plotter-Parser](https://github.com/bsc-mem/Mess/wiki/Plotter-Parser)
- [Architecture Support](https://github.com/bsc-mem/Mess/wiki/Architecture-Support)
- [FAQ](https://github.com/bsc-mem/Mess/wiki/FAQ)

---

## Motivation

Traditional memory benchmarks report isolated metrics such as peak bandwidth or idle latency, which often fail to capture how memory systems behave under realistic workloads. **Mess** (Memory Stress) addresses this limitation by characterizing memory performance through **bandwidth-latency curves** that cover the full range of memory traffic intensity, from unloaded to fully saturated.

This approach reveals critical insights:

- Memory writes degrade performance significantly compared to reads
- Systems typically saturate at 70-90% of theoretical maximum bandwidth
- Latency ranges from 85-130ns when idle to 200-600ns+ under saturation

These ranges are illustrative; measured values depend on the system and memory configuration.

Mess provides a holistic, close-to-hardware view of memory system behavior, enabling researchers and engineers to understand real-world performance characteristics that standard benchmarks miss.

> **MICRO 2024 Best Paper Runner-Up**: The Mess methodology was published at the 57th IEEE/ACM International Symposium on Microarchitecture.

For a detailed explanation of the benchmark methodology, see the [Memory BSC Tools page](https://memory.bsc.es/tools/mess-benchmark).

---

## Tools Included

Mess provides an integrated workflow for memory system characterization, from benchmarking to application profiling:

- **[Mess Benchmark](https://github.com/bsc-mem/Mess/wiki/Mess-Benchmark)**: Characterizes your memory system by generating bandwidth-latency curves that reveal how it behaves under varying load.
- **[Mess Profiler](https://github.com/bsc-mem/Mess/wiki/Mess-Profiler)**: Automates counter discovery and runs profiling tools (`perf`, `likwid`, etc.) with the correct configuration, ensuring application measurements align with benchmark data.
- **[Plotter-Parser](https://github.com/bsc-mem/Mess/wiki/Plotter-Parser)**: Generates publication-quality plots as well as CSV and JSON files containing the parsed bandwidth-latency curves.
- **[Mess GUI](#graphic-user-interface)**: Desktop interface for configuring benchmark runs and inspecting results on Linux, macOS and Windows.
- **[Traffic Generator](https://github.com/bsc-mem/Mess/wiki/Traffic-Generator)**: The low-level engine that generates precise memory traffic patterns at the assembly level. Can also be used independently for custom microbenchmarks.

---

## Architecture Support

Mess can characterize memory systems in consumer laptops and desktops, workstations,
and servers. Availability depends on both the operating system and access to
bandwidth counters for the processor. ISA support alone does not guarantee that a
complete bandwidth-latency curve can be measured.

### System Availability

| System | Availability | Bandwidth backend / requirements |
| --- | --- | --- |
| Linux PCs, workstations and servers | Available on supported CPUs | `perf`, LIKWID, Intel PCM or VTune; requires accessible counters for the CPU and memory system |
| Apple Silicon Macs (M-series) | Available | IOReport reads Apple memory-controller counters; Xcode Command Line Tools required |
| Intel Macs running macOS | Unavailable for bandwidth-latency curves | No macOS bandwidth-counter backend for Intel CPUs |
| Windows PCs and workstations (Intel/AMD x86-64) | Available with vendor tools | Intel VTune or the Intel client IMC backend, or AMD uProf; Administrator access required |
| Linux RISC-V systems | Partial / WIP | Kernel generation and latency support are implemented; bandwidth measurement remains pending |

The [Mess GUI](#graphic-user-interface) provides a desktop interface for Linux,
macOS (Apple Silicon) and Windows. It uses the same benchmark engine and has the
same measurement requirements as the CLI.

Platform-specific limits:

- **Linux**: Instruction-latency sampling requires a supported PEBS/SPE CPU and
  access to its sampling events. NUMA memory binding uses `--bind`.
- **macOS**: `--bind`, `--inst-lat` and `--add-counters` are unavailable.
- **Windows**: `--bind`, `--inst-lat` and `--add-counters` are unavailable.
  Backends are implemented, but native Windows driver and curve validation
  remains pending.

### Instruction Sets and GPUs

Available modes depend on the hardware. For implementation details, see
[Architecture Support](https://github.com/bsc-mem/Mess/wiki/Architecture-Support).

| Architecture | ISA / API | Notes |
| --- | --- | --- |
| x86-64 CPUs | SCALAR, SSE2, AVX, AVX2, AVX-512 | Intel and AMD; vector modes require the corresponding CPU features |
| ARM CPUs | SCALAR, NEON, SVE | Apple Silicon, Neoverse, Graviton, A64FX and other supported ARM systems; SVE modes require SVE-capable hardware |
| Power-PC CPUs | VSX, VMX | Power8 and newer on Linux |
| RISC-V CPUs | SCALAR, RVV | Partial / WIP; bandwidth measurement is not yet available |
| CUDA-capable NVIDIA GPUs | CUDA | Run with `--gpu`; requires the NVIDIA driver and CUDA Toolkit 12.6+ (`nvcc`, CUPTI) |
| Other GPU vendors | — | Backend support is not yet available |

---

## Installation

See full instructions in the [Installation wiki page](https://github.com/bsc-mem/Mess/wiki/Installation).

For the desktop interface and installer information, see [Mess GUI](#graphic-user-interface).
The instructions below cover building and running the CLI.

### Clone

```bash
git clone --recursive https://github.com/bsc-mem/Mess.git
cd Mess
```

**Important**: The `--recursive` flag is required to download submodules that Mess depends on.

If you forget `--recursive`, initialize submodules later:

```bash
git submodule update --init --recursive
```

### Dependencies

#### Linux

Core requirements:

- **C++17 compiler**: GCC 9+ (recommended), Clang 10+, Intel OneAPI (ICX), AOCC
- **numactl**: Required for NUMA memory binding
- **taskset**: Core pinning (preferred, part of `util-linux`)
- **perf**: Recommended counter backend (`linux-tools-common`)
- **CMake 3.21+**: Required for CMake builds; the Linux `make` build does not need it
- **Python 3**: Plotting utilities

To run the NVIDIA GPU benchmark with `--gpu`, install the NVIDIA driver, CUDA
Toolkit 12.6 or newer (including `nvcc` and CUPTI). Mess compiles the GPU
kernels at runtime and reads DRAM counters in-process through CUPTI. GPUs before
Volta (including Pascal GTX 10-series) use the legacy CUPTI Event/Metric APIs;
Volta and newer use the CUPTI range profiler. For pre-Volta GPUs, use CUDA 12.6–12.9,
since CUDA 13 removes the legacy APIs. Extra counters on these GPUs use legacy
metric or event names, such as `dram_read_bytes` or `dram_read_transactions`;
list them with `nvprof --query-metrics` or `nvprof --query-events`.

GPU performance counters require administrator access unless the NVIDIA driver
is configured with `NVreg_RestrictProfilingToAdminUsers=0`.

GPU workload definition 2 uses line-coalesced, dependent Sequential loads. Set
`--gpu-seq=sector:independent` to select sector accesses with independent loads;
append `:full` to a sector policy to write all 32 bytes per thread. Workload
version, access granularity, load dependency and store width are saved in
`plotter.txt`, so curves made with different policies can be identified. Use a
fresh `--folder` when changing policies or comparing with older GPU results;
Mess rejects relabelling an existing dataset with a different workload.


Ensure perf access:

```bash
cat /proc/sys/kernel/perf_event_paranoid
echo 0 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

Huge pages are recommended for more accurate measurements:

```bash
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
```

> Even without huge pages, Mess automatically compensates for page walk latency. See [Huge-Memory-Pages](https://github.com/bsc-mem/Mess/wiki/Huge-Memory-Pages) for details.

#### macOS

Core requirements:

- **Apple Silicon**: M-series Mac required; bandwidth is read from the Apple memory controller through IOReport, and Intel Macs have no counter backend
- **Xcode Command Line Tools**: Apple Clang (C++17) plus the CoreFoundation and IOKit frameworks
- **CMake 3.21+**: Only for the `macos-cmake` preset, the `make` build does not need it
- **Python 3**: Plotting utilities

```bash
xcode-select --install
brew install cmake python3
```

No counter setup is needed: Mess reads `/usr/lib/libIOReport.dylib` directly, so
there is no driver to install and no elevated prompt to open.

#### Windows

Core requirements:

- **MSYS2**: UCRT64 toolchain ([msys2.org](https://www.msys2.org/))
- **CMake 3.21+**: Required by CMake and the Make compatibility commands
- **Python 3**: Plotting utilities
- **Ninja**: Generator used by that preset
- **Intel VTune, AMD uProf, or the Intel PCM driver**: Counter backend (see below)

Install the toolchain from an MSYS2 shell:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake ninja
```

Windows exposes no memory-bandwidth counter to normal programs, so Mess reads
one through a vendor tool and its kernel driver. **Install the tool for your CPU,
then always run Mess from an Administrator prompt**.

| Your CPU | Tool to install |
| --- | --- |
| AMD | [AMD uProf](https://www.amd.com/en/developer/uprof.html), with bandwidth collection support for your CPU |
| Intel | [Intel VTune Profiler](https://www.intel.com/content/www/us/en/developer/tools/oneapi/vtune-profiler-download.html), where memory-bandwidth counters are supported |
| Intel client CPUs (IMC fallback) | [Intel PCM](https://github.com/intel/pcm) `msr.sys` driver with physical-memory mapping support; Mess uses `--measurer=imc` |

Mess automatically selects the available backend with `--measurer=auto`.
The Intel client IMC backend does not support Xeon/server memory controllers.
The `--measurer=pcm` CLI backend is unavailable on Windows.

### Build

#### Linux & macOS

```bash
make
make install
```

Binaries are generated in `build/bin/`:

- `mess` — Core benchmark
- `mess-profiler` — Memory bandwidth profiler
- `generate_code` — Kernel generator; `make install` runs it to emit the traffic generator

Generated traffic-generator binaries are placed alongside `mess` in `build/bin/`:

- `traffic_gen_multiseq.x` — Standalone traffic generation tool

Optional PATH setup:

```bash
export PATH=$PATH:$(pwd)/build/bin
```

#### Windows

```powershell
$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"
cmake --preset windows-mingw
cmake --build --preset windows-mingw
build\cmake\windows-mingw\bin\Release\generate_code.exe
```

With this preset, binaries are generated in `build/cmake/windows-mingw/bin/Release/`:

- `mess.exe` — Core benchmark
- `mess-profiler.exe` — Memory bandwidth profiler
- `generate_code.exe` — Kernel generator; run it as shown above to emit the traffic generator

Keep `C:\msys64\ucrt64\bin` first on `PATH`. `--bind`, `--inst-lat`
and `--add-counters` are unavailable on Windows.

### Verification

Linux and macOS (`make` build):

```bash
./build/bin/mess --version
./build/bin/mess --dry-run --verbose=2
```

Windows (PowerShell, CMake preset):

```powershell
.\build\cmake\windows-mingw\bin\Release\mess.exe --version
.\build\cmake\windows-mingw\bin\Release\mess.exe --dry-run --verbose=2
```

---

## Quick Start

The examples below use the Linux/macOS `make` build. On Windows, use
`.\build\cmake\windows-mingw\bin\Release\mess.exe` with the same supported
options. See [System Availability](#system-availability) for platform limits.

Profiling is enabled by default: CPU and GPU runs save measurement files under
`measuring/` (or `--folder=DIR`). Use `--no-profile` to discard them after the run.

```bash
./build/bin/mess --dry-run --verbose=2

./build/bin/mess

./build/bin/mess --no-profile

./build/bin/mess --tier=lite
```

Common options:

| Option | Description | Example |
| --- | --- | --- |
| `--gpu` | Run the NVIDIA GPU bandwidth-latency benchmark (requires CUDA Toolkit 12.6+) | `--gpu` |
| `--ratio=N[,N...]` | Issued load ratio(s) in % | `--ratio=100,75,50` |
| `--pause=N[,N...]` | Pause bubble values | `--pause=0,10,100,1000` |
| `--tier=TIER` | Adaptive pause discovery preset (`lite/standard/detailed`) | `--tier=lite` |
| `--point-count=N` | Custom adaptive point budget | `--point-count=75` |
| `--no-profile` | Discard measurement files after the run (saved by default) | `--no-profile` |
| `--verbose=N` | Verbosity level `0-4` | `--verbose=3` |
| `--measurer=TYPE` | Counter backend (`auto/perf/likwid/pcm/vtune/ioreport/imc/uprof`) | `--measurer=perf` |
| `--inst-lat` | Linux: use supported PEBS/SPE instruction sampling for latency | `--inst-lat` |
| `--add-counters=LIST` | Measure extra counters alongside bandwidth | `--add-counters=cycles,instructions` |
| `--bind=LIST` | Linux: NUMA memory-node binding | `--bind=0` |
| `--cores=LIST` | Explicit traffic-generator cores | `--cores=0-15` |
| `--total-cores=N` | Number of traffic-generator cores | `--total-cores=16` |
| `--repetitions=N`  | Number of point repetitions (default: 3) | `--repetitions=5` |

Some options are platform-specific. For the complete set your build supports,
use `./build/bin/mess --help` or see [Understanding-CLI-Arguments](https://github.com/bsc-mem/Mess/wiki/Understanding-CLI-Arguments).

---

## Graphic User Interface

The Mess GUI is a desktop front-end for the same benchmark binary the CLI drives:
set up quick runs, launch full bandwidth-latency curves, and inspect the results.

### Install

Installers for Linux, macOS (Apple Silicon) and Windows will be available on the
**[Mess GUI download page](https://memory.bsc.es/downloads/mess-gui/)**.
Choose the package for your operating system and follow the installation
instructions provided there.

The GUI needs the [build tools and counter backend](#dependencies) for your
platform to compile Mess and collect measurements.

### First launch

The GUI asks where Mess lives:

| Choice | When to pick it | What it does |
| --- | --- | --- |
| **Locate existing Mess** | You already cloned this repository | Points the GUI at your existing checkout |
| **Install Mess** | Nothing cloned yet | Clones this repository for you |

Either way, the GUI then compiles Mess and uses the resulting binary as the
backend for every measurement.

---

## Common Workflows

### Single-Point Sanity Check

```bash
./build/bin/mess --ratio=100 --pause=0 --verbose=3 --repetitions=1
./build/bin/mess --ratio=0 --pause=0 --verbose=3 --repetitions=1
```

### Adaptive Curve Discovery

```bash
./build/bin/mess --tier=standard
./build/bin/mess --point-count=75
```

### Instruction-Latency Sampling

Linux only, on CPUs with supported PEBS/SPE sampling events:

```bash
./build/bin/mess --inst-lat --tier=lite
```

### Backend Selection

Linux examples; available backends vary by platform:

```bash
./build/bin/mess --measurer=likwid --bind=0
./build/bin/mess --measurer=pcm --bind=0
./build/bin/mess --measurer=vtune --tier=lite
./build/bin/mess --measurer=perf --add-counters=cycles,instructions
```

### NUMA Comparison

Linux only, on systems with multiple memory nodes:

```bash
./build/bin/mess --bind=0 --folder=numa0
./build/bin/mess --bind=1 --folder=numa1
```

### Core-Scaling Sweep

```bash
for c in 2 4 8 16; do
  ./build/bin/mess --total-cores=$c --folder=cores_$c
done
```

---

## Mess Profiler

`mess-profiler` reuses Mess counter discovery to profile applications with consistent output.

```bash
./build/bin/mess-profiler --dry-run

./build/bin/mess-profiler -s 100ms -o app_profile.csv ./my_app
```

Profiler docs: [Mess-Profiler](https://github.com/bsc-mem/Mess/wiki/Mess-Profiler)

---

## Plotter-Parser

Visualization utilities in `utils/`:

- `plotter.py`: generates memory-curve plots and processed CSV/JSON
- `app_plotter.py`: overlays application profile points on curves

```bash
cd utils
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Plotter docs: [Plotter-Parser](https://github.com/bsc-mem/Mess/wiki/Plotter-Parser)

---

## Learning Resources

- **Wiki**: [Mess Wiki](https://github.com/bsc-mem/Mess/wiki)
- **Tutorials and Slides**: [mess.bsc.es/tutorials](https://mess.bsc.es/tutorials)
- **Detailed Methodology**: [memory.bsc.es/tools/mess-benchmark](https://memory.bsc.es/tools/mess-benchmark)
- **Results Dataset**: [Mess Results (GitHub)](https://github.com/bsc-mem/Mess-Results) — bandwidth-latency curves for a variety of system architectures

---

## Troubleshooting

- Permission errors on counters: set `perf_event_paranoid` to `0`
- Missing counters/backend mismatch: check `./build/bin/mess-profiler --dry-run`
- Unstable measurements: increase `--repetitions` and use `--verbose=3`
- Windows: no bandwidth counters — run as Administrator and install the vendor tool for your CPU (see [Windows](#windows))
- macOS: bandwidth measurement requires Apple Silicon; `--bind`, `--inst-lat` and `--add-counters` are not supported (see [macOS](#macos))

More: [FAQ](https://github.com/bsc-mem/Mess/wiki/FAQ) and [Iterative-Debugging](https://github.com/bsc-mem/Mess/wiki/Iterative-Debugging)

**Found a bug?** Open an issue on [GitHub](https://github.com/bsc-mem/Mess/issues) or email [mess@bsc.es](mailto:mess@bsc.es?subject=%5BMess%5D%20Bug%20Report).

---

## Contributors

Mess is developed by the **[Memory Systems Team](https://memory.bsc.es)** at the Barcelona Supercomputing Center (BSC).

<table>
  <tr>
    <td align="center" width="50%" style="vertical-align: top;">
      <b>Victor Xirau Guardans</b><br/>Main Mess developer<br/><a href="mailto:victor.xirau@bsc.es">victor.xirau@bsc.es</a>
    </td>
    <td align="center" width="50%" style="vertical-align: top;">
      <b>Mariana Carmin</b><br/>Mess developer<br/><a href="mailto:mcarmin@bsc.es">mcarmin@bsc.es</a>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%" style="vertical-align: top;">
      <b>Pau Díaz</b><br/>Mess developer<br/><a href="mailto:pau.diazcuesta@bsc.es">pau.diazcuesta@bsc.es</a>
    </td>
    <td align="center" width="50%" style="vertical-align: top;">
      <b>Javier Beiro</b><br/>Mess GPU developer<br/><a href="mailto:javier.beiro@bsc.es">javier.beiro@bsc.es</a>
    </td>
  </tr>
</table>

**Mess Paper author:** Pouya Esmaili Dokht ([pouya.esmaili@bsc.es](mailto:pouya.esmaili@bsc.es))

Or email: [mess@bsc.es](mailto:mess@bsc.es?subject=%5BMess%5D%20Request)

---

## Citation

If you use Mess in research, please cite:

```bibtex
@inproceedings{esmaili2024mess,
  title     = {A Mess of Memory System Benchmarking, Simulation and Application Profiling},
  author    = {Esmaili-Dokht, Pouya and Sgherzi, Francesco and Girelli, Valeria Soldera
               and Boixaderas, Isaac and Carmin, Mariana and Monemi, Alireza
               and Armejach, Adria and Mercadal, Estanislao and Llort, German
               and Radojkovi{\'c}, Petar and Moreto, Miquel and Gim{\'e}nez, Judit
               and Martorell, Xavier and Ayguad{\'e}, Eduard and Labarta, Jesus
               and Confalonieri, Emanuele and Dubey, Rishabh and Adlard, Joshua},
  booktitle = {Proceedings of the 57th IEEE/ACM International Symposium on Microarchitecture (MICRO)},
  pages     = {136--152},
  year      = {2024},
  publisher = {IEEE}
}
```

---

## References

1. **[Mess Benchmark](https://github.com/bsc-mem/Mess-benchmark)** — The original implementation of the Mess benchmark.
2. **[Mess Simulator](https://github.com/bsc-mem/Mess-simulator)** — Analytical memory model using bandwidth-latency curves.
3. **[Mess-Paraver](https://github.com/bsc-mem/Mess-Paraver)** — Integration with Paraver for visualization.
4. **[Mess Results](https://github.com/bsc-mem/Mess-Results)** — Collection of bandwidth-latency curves for various system architectures; a dataset users can browse and compare.
5. **[Mess Paper](https://arxiv.org/abs/2405.10170)** — Esmaili-Dokht, P., Sgherzi, F., Girelli, V. S., Boixaderas, I., Carmin, M., Monemi, A., Armejach, A., Mercadal, E., Llort, G., Radojković, P., Moreto, M., Giménez, J., Martorell, X., Ayguadé, E., Labarta, J., Confalonieri, E., Dubey, R., & Adlard, J. (2024). A mess of memory system benchmarking, simulation and application profiling. In _Proceedings of the 57th IEEE/ACM International Symposium on Microarchitecture_ (MICRO) (pp. 136-152). IEEE.

---

<p align="center">
  <sub>Mess Benchmark is released under the BSD 3-Clause License</sub>
</p>
