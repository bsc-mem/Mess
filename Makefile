# Copyright (c) 2026, Barcelona Supercomputing Center
# Contact: mess             [at] bsc [dot] es
#          victor.xirau     [at] bsc [dot] es
#          mariana.carmin   [at] bsc [dot] es
#          pau.diazcuesta   [at] bsc [dot] es
#          javier.beiro     [at] bsc [dot] es
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
# Mess Makefile
UNAME_S := $(shell uname -s)
ifeq ($(filter $(UNAME_S),Linux Darwin),)
CMAKE ?= cmake
BUILD_TYPE ?= release
JOBS ?= 4
ifeq ($(filter $(BUILD_TYPE),release debug),)
$(error BUILD_TYPE must be release or debug)
endif
CONFIG = Release
BIN_DIR = $(CURDIR)/build/bin
ifeq ($(BUILD_TYPE),debug)
CONFIG = Debug
BIN_DIR = $(CURDIR)/build/debug/bin
endif
CMAKE_BUILD_DIR = $(CURDIR)/build/cmake/make-$(BUILD_TYPE)
CMAKE_ARGS ?=
# Only explicit compiler overrides are forwarded; Make's built-in CC/CXX are not.
ifneq ($(filter command line environment environment override,$(origin CC)),)
CMAKE_COMPILER_ARGS += -DCMAKE_C_COMPILER="$(CC)"
endif
ifneq ($(filter command line environment environment override,$(origin CXX)),)
CMAKE_COMPILER_ARGS += -DCMAKE_CXX_COMPILER="$(CXX)"
endif
.DEFAULT_GOAL := all
.PHONY: all binaries parallel debug configure test install clean distclean
configure:
	$(CMAKE) -S . -B "$(CMAKE_BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(CONFIG) -DMESS_RUNTIME_OUTPUT_DIRECTORY="$(BIN_DIR)" $(CMAKE_COMPILER_ARGS) $(CMAKE_ARGS)
all binaries parallel: configure
	+$(CMAKE) --build "$(CMAKE_BUILD_DIR)" --config $(CONFIG) --parallel $(JOBS)
debug:
	+$(MAKE) BUILD_TYPE=debug all
test: all
	"$(BIN_DIR)/mess" --help
install: all
	"$(BIN_DIR)/generate_code" $(DEBUG_FLAGS)
# Clean only this configuration's CMake products. Keep measurements and kernels.
clean:
	if test -f "$(CMAKE_BUILD_DIR)/CMakeCache.txt"; then $(CMAKE) --build "$(CMAKE_BUILD_DIR)" --config $(CONFIG) --target clean; fi
distclean: clean
	$(CMAKE) -E rm -rf "$(CMAKE_BUILD_DIR)"

else

CXX = g++
CC = gcc

JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

ARCH := $(shell uname -m)

ifeq ($(ARCH),riscv64)
    OPTFLAGS = -O3 -march=rv64g
else
    OPTFLAGS = -O3 -march=native
endif

OPTFLAGS += -funroll-loops -ffast-math -fomit-frame-pointer
OPTFLAGS += -finline-functions

GCC_VERSION := $(shell $(CXX) -dumpversion | cut -d. -f1)
GCC_MINOR := $(shell $(CXX) -dumpversion | cut -d. -f2)

ifeq ($(shell [ $(GCC_VERSION) -lt 5 ] 2>/dev/null && echo 1 || echo 0), 1)
CXXSTD = -std=c++11
else ifeq ($(shell [ $(GCC_VERSION) -eq 5 ] 2>/dev/null && echo 1 || echo 0), 1)
CXXSTD = -std=c++14
else ifeq ($(shell [ $(GCC_VERSION) -eq 6 ] 2>/dev/null || [ $(GCC_VERSION) -eq 7 ] 2>/dev/null && echo 1 || echo 0), 1)
CXXSTD = -std=c++14
else ifeq ($(shell [ $(GCC_VERSION) -ge 8 ] 2>/dev/null && echo 1 || echo 0), 1)
CXXSTD = -std=c++17
else
CXXSTD = -std=c++11
endif

BUILD_TYPE ?= release
ifeq ($(filter $(BUILD_TYPE),release debug),)
$(error BUILD_TYPE must be release or debug)
endif
BUILD_OPTFLAGS = $(OPTFLAGS)
ifeq ($(BUILD_TYPE),debug)
BUILD_OPTFLAGS = -g -O0
endif

CXXFLAGS = $(CXXSTD) -Wall -Wextra -Wunused $(BUILD_OPTFLAGS) -pthread -I. -I./include -I./src -I./src/libraries -I./include/arch
CFLAGS = $(OPTFLAGS) -I. -I./include -I./src -I./src/libraries -I./include/arch
LDFLAGS = -pthread -lm
LDFLAGS += $(shell [ $(GCC_VERSION) -lt 9 ] 2>/dev/null && echo "-lstdc++fs" || true)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CXXFLAGS += -fblocks
    LDFLAGS += -framework CoreFoundation -framework IOKit
endif
BUILD_DIR = build
# Preserve release paths used by launchers and installation scripts.
BIN_DIR = $(BUILD_DIR)/bin
ifeq ($(BUILD_TYPE),debug)
BIN_DIR = $(BUILD_DIR)/debug/bin
endif
OBJ_DIR = $(BUILD_DIR)/obj/$(BUILD_TYPE)
.DEFAULT_GOAL := all

PLATFORM_SRCS = src/platform/posix/Process.cpp \
                src/platform/posix/Fs.cpp \
                src/platform/posix/Ipc.cpp \
                src/platform/posix/Run.cpp \
                src/platform/posix/Sys.cpp \
                src/platform/posix/Console.cpp

PERF_COUNTER_SRCS = src/measurement/bw_measurers/PerfBandwidthMeasurer.cpp \
                    src/measurement/PerfStatCsv.cpp

SRCS = Mess.cpp \
       src/SystemDetection.cpp \
       src/BenchmarkConfig.cpp \
       src/CliParser.cpp \
       src/KernelGenerator.cpp \
       src/measurement/MeasurementStorage.cpp \
       src/measurement/TrafficGenArrayCalibrator.cpp \
       src/measurement/LatencyMeasurer.cpp \
       src/measurement/BurstCompletion.cpp \
       src/measurement/OutlierDetector.cpp \
       src/measurement/MeasurementUtils.cpp \
       src/measurement/ResultFormatter.cpp \
       src/measurement/BandwidthMeasurerFactory.cpp \
       $(PERF_COUNTER_SRCS) \
       src/measurement/bw_measurers/LikwidBandwidthMeasurer.cpp \
       src/measurement/bw_measurers/VtuneBandwidthMeasurer.cpp \
       src/measurement/VtuneCounterParser.cpp \
       src/measurement/VtuneCollection.cpp \
       src/measurement/bw_measurers/PcmBandwidthMeasurer.cpp \
       src/measurement/bw_measurers/IOReportBandwidthMeasurer.cpp \
       src/process/TrafficGenProcessManager.cpp \
       src/process/PtrchaseProcessManager.cpp \
       src/utils/ProgressTracker.cpp \
       src/utils/PmuSysfs.cpp \
       src/utils/CpuTopology.cpp \
       src/utils/SubprocessCapture.cpp \
       src/utils/TerminalCursor.cpp \
       src/gpu/GpuBenchmarkExecutor.cpp \
       src/gpu/GpuKernelGenerator.cpp \
       src/gpu/GpuMeasurement.cpp \
       src/gpu/GpuWorker.cpp \
       src/BenchmarkExecutor.cpp \
       src/ResultsProcessor.cpp \
       src/TlbUtils.cpp \
       src/Utils.cpp \
       src/PtrchasePerfHelper.cpp \
       src/measurement/AdaptiveSweep.cpp \
       src/CurveTracer.cpp
SRCS += src/measurement/instruction_samplers/IntelPebsSampler.cpp \
        src/measurement/instruction_samplers/ArmSpeSampler.cpp \
        src/measurement/InstructionSamplerFactory.cpp

ARCH_SRCS = src/arch/ArchitectureRegistry.cpp \
            src/arch/BandwidthCounterStrategy.cpp \
            src/measurement/PointerChaseTiming.cpp \
            src/measurement/CpuFrequencyProbe.cpp \
            src/arch/x86/X86Architecture.cpp \
            src/arch/x86/X86Assembler.cpp \
            src/arch/x86/X86Counters.cpp \
            src/arch/x86/counters/IntelSkylakeCounters.cpp \
            src/arch/x86/counters/IntelSPRCounters.cpp \
            src/arch/x86/counters/IntelEmeraldRapidsCounters.cpp \
            src/arch/x86/counters/IntelGraniteRapidsCounters.cpp \
            src/arch/x86/counters/AmdZenCounters.cpp \
            src/arch/x86/counters/AmdZen4Counters.cpp \
            src/arch/x86/counters/AmdZen5Counters.cpp \
            src/arch/arm/ArmArchitecture.cpp \
            src/arch/arm/ArmISAUtils.cpp \
            src/arch/arm/ArmAssembler.cpp \
            src/arch/arm/ArmCounters.cpp \
            src/arch/arm/counters/A64FXCounters.cpp \
            src/arch/arm/counters/Graviton3Counters.cpp \
            src/arch/arm/counters/NvidiaGraceCounters.cpp \
            src/arch/arm/counters/AppleSiliconCounters.cpp \
            src/arch/powerpc/PowerPCArchitecture.cpp \
            src/arch/powerpc/PowerPCAssembler.cpp \
            src/arch/powerpc/PowerPCCounters.cpp \
            src/arch/riscv/RiscvArchitecture.cpp \
            src/arch/riscv/RiscvAssembler.cpp \
            src/arch/riscv/RiscvCounters.cpp \
            src/arch/riscv/counters/SiFiveCounters.cpp
       
PROFILER_SRCS = src/SystemDetection.cpp \
                     src/BenchmarkConfig.cpp \
                     src/measurement/MeasurementUtils.cpp \
                     src/measurement/BandwidthMeasurerFactory.cpp \
                     $(PERF_COUNTER_SRCS) \
                     src/measurement/bw_measurers/LikwidBandwidthMeasurer.cpp \
                     src/measurement/bw_measurers/VtuneBandwidthMeasurer.cpp \
                     src/measurement/VtuneCounterParser.cpp \
                     src/measurement/VtuneCollection.cpp \
                     src/measurement/bw_measurers/PcmBandwidthMeasurer.cpp \
                     src/measurement/bw_measurers/IOReportBandwidthMeasurer.cpp \
                     src/measurement/MeasurementStorage.cpp \
                     src/process/TrafficGenProcessManager.cpp \
                     src/profiler/ProcessBinding.cpp \
                     src/profiler/ProfilerConfig.cpp \
                     src/utils/PmuSysfs.cpp \
                     src/utils/CpuTopology.cpp \
                     src/utils/SubprocessCapture.cpp \
                     src/Utils.cpp


MESS_OBJS = $(addprefix $(OBJ_DIR)/,$(SRCS:.cpp=.o) $(ARCH_SRCS:.cpp=.o) $(PLATFORM_SRCS:.cpp=.o) src/SystemInfo.o)
GENERATOR_SRCS = GenerateCode.cpp src/SystemDetection.cpp src/KernelGenerator.cpp \
                 src/TlbUtils.cpp src/PtrchasePerfHelper.cpp src/Utils.cpp \
                 src/measurement/TrafficGenArrayCalibrator.cpp src/utils/PmuSysfs.cpp \
                 src/utils/CpuTopology.cpp src/utils/SubprocessCapture.cpp
GENERATOR_OBJS = $(addprefix $(OBJ_DIR)/,$(GENERATOR_SRCS:.cpp=.o) $(ARCH_SRCS:.cpp=.o) $(PLATFORM_SRCS:.cpp=.o) src/SystemInfo.o)
PROFILER_OBJS = $(addprefix $(OBJ_DIR)/,MessProfiler.o $(PROFILER_SRCS:.cpp=.o) $(ARCH_SRCS:.cpp=.o) $(PLATFORM_SRCS:.cpp=.o) src/SystemInfo.o)
ALL_OBJS = $(sort $(MESS_OBJS) $(GENERATOR_OBJS) $(PROFILER_OBJS))
TARGETS = $(BIN_DIR)/mess $(BIN_DIR)/generate_code $(BIN_DIR)/mess-profiler

all:
	$(MAKE) -j$(JOBS) binaries

binaries: $(TARGETS)

parallel: all

debug:
	$(MAKE) BUILD_TYPE=debug all

$(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/src/measurement/CpuFrequencyProbe.o $(OBJ_DIR)/src/measurement/PointerChaseTiming.o $(OBJ_DIR)/src/gpu/GpuMeasurement.o: CXXFLAGS += -fno-fast-math

$(BIN_DIR)/mess: $(MESS_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/generate_code: $(GENERATOR_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/mess-profiler: $(PROFILER_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

install:
	@echo "Running code generation..."
	@./$(BIN_DIR)/generate_code $(DEBUG_FLAGS)

.PHONY: all binaries parallel debug install clean distclean

clean:
	rm -f $(ALL_OBJS) $(TARGETS)
	rm -rf $(BUILD_DIR)/
	rm -rf src/traffic_gen/build
	rm -rf src/traffic_gen/src/nop.c
	rm -rf src/traffic_gen/src/nop_*.c
	rm -rf src/traffic_gen/src/generated_*.c
	rm -rf src/traffic_gen/src/utils_*.c
	find . -type f -name '*.o' -delete
	rm -rf build/*


distclean: clean
	rm -f *.log *.csv *.json

endif
