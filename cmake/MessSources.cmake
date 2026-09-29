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

# Authoritative source lists. Runtime-generated kernels are built separately.
set(MESS_LIB_SRCS
    src/SystemInfo.cpp
)

set(MESS_CORE_SRCS
    Mess.cpp
    src/SystemDetection.cpp
    src/BenchmarkConfig.cpp
    src/CliParser.cpp
    src/KernelGenerator.cpp
    src/measurement/MeasurementStorage.cpp
    src/measurement/TrafficGenArrayCalibrator.cpp
    src/measurement/LatencyMeasurer.cpp
    src/measurement/BurstCompletion.cpp
    src/measurement/OutlierDetector.cpp
    src/measurement/MeasurementUtils.cpp
    src/measurement/ResultFormatter.cpp
    src/measurement/BandwidthMeasurerFactory.cpp
    src/measurement/InstructionSamplerFactory.cpp
    src/process/TrafficGenProcessManager.cpp
    src/process/PtrchaseProcessManager.cpp
    src/utils/ProgressTracker.cpp
    src/utils/PmuSysfs.cpp
    src/utils/CpuTopology.cpp
    src/utils/SubprocessCapture.cpp
    src/utils/TerminalCursor.cpp
    src/gpu/GpuBenchmarkExecutor.cpp
    src/gpu/GpuKernelGenerator.cpp
    src/gpu/GpuMeasurement.cpp
    src/gpu/GpuWorker.cpp
    src/BenchmarkExecutor.cpp
    src/ResultsProcessor.cpp
    src/TlbUtils.cpp
    src/Utils.cpp
    src/PtrchasePerfHelper.cpp
    src/measurement/AdaptiveSweep.cpp
    src/CurveTracer.cpp
)

set(MESS_POSIX_ONLY_SRCS
    src/measurement/PerfStatCsv.cpp
    src/measurement/bw_measurers/PerfBandwidthMeasurer.cpp
    src/measurement/bw_measurers/LikwidBandwidthMeasurer.cpp
    src/measurement/bw_measurers/PcmBandwidthMeasurer.cpp
    src/measurement/bw_measurers/IOReportBandwidthMeasurer.cpp
    src/measurement/instruction_samplers/IntelPebsSampler.cpp
    src/measurement/instruction_samplers/ArmSpeSampler.cpp
)

set(MESS_VTUNE_SRCS
    src/measurement/bw_measurers/VtuneBandwidthMeasurer.cpp
    src/measurement/VtuneCounterParser.cpp
    src/measurement/VtuneCollection.cpp
)

set(MESS_WINDOWS_ONLY_SRCS
    src/measurement/bw_measurers/WindowsImcBandwidthMeasurer.cpp
    src/measurement/bw_measurers/AMDuProfBandwidthMeasurer.cpp
    src/measurement/UprofParser.cpp
    src/measurement/UprofCollection.cpp
)

set(MESS_PLATFORM_POSIX_SRCS
    src/platform/posix/Process.cpp
    src/platform/posix/Fs.cpp
    src/platform/posix/Ipc.cpp
    src/platform/posix/Run.cpp
    src/platform/posix/Sys.cpp
    src/platform/posix/Console.cpp
)

set(MESS_PLATFORM_WINDOWS_SRCS
    src/platform/windows/Process.cpp
    src/platform/windows/Fs.cpp
    src/platform/windows/Ipc.cpp
    src/platform/windows/Run.cpp
    src/platform/windows/WinStrings.cpp
    src/platform/windows/Sys.cpp
    src/platform/windows/Console.cpp
    src/platform/windows/Topology.cpp
    src/platform/windows/SmbiosMemory.cpp
    src/platform/windows/SysInfoWindows.cpp
)

set(MESS_ARCH_SRCS
    src/arch/ArchitectureRegistry.cpp
    src/arch/BandwidthCounterStrategy.cpp
    src/measurement/PointerChaseTiming.cpp
    src/measurement/CpuFrequencyProbe.cpp
    src/arch/x86/X86Architecture.cpp
    src/arch/x86/X86Assembler.cpp
    src/arch/x86/X86Counters.cpp
    src/arch/x86/counters/IntelSkylakeCounters.cpp
    src/arch/x86/counters/IntelSPRCounters.cpp
    src/arch/x86/counters/IntelEmeraldRapidsCounters.cpp
    src/arch/x86/counters/IntelGraniteRapidsCounters.cpp
    src/arch/x86/counters/AmdZenCounters.cpp
    src/arch/x86/counters/AmdZen4Counters.cpp
    src/arch/x86/counters/AmdZen5Counters.cpp
    src/arch/arm/ArmArchitecture.cpp
    src/arch/arm/ArmISAUtils.cpp
    src/arch/arm/ArmAssembler.cpp
    src/arch/arm/ArmCounters.cpp
    src/arch/arm/counters/A64FXCounters.cpp
    src/arch/arm/counters/Graviton3Counters.cpp
    src/arch/arm/counters/NvidiaGraceCounters.cpp
    src/arch/arm/counters/AppleSiliconCounters.cpp
    src/arch/powerpc/PowerPCArchitecture.cpp
    src/arch/powerpc/PowerPCAssembler.cpp
    src/arch/powerpc/PowerPCCounters.cpp
    src/arch/riscv/RiscvArchitecture.cpp
    src/arch/riscv/RiscvAssembler.cpp
    src/arch/riscv/RiscvCounters.cpp
    src/arch/riscv/counters/SiFiveCounters.cpp
)

set(MESS_GENERATE_CODE_SRCS
    GenerateCode.cpp
    src/SystemDetection.cpp
    src/KernelGenerator.cpp
    src/TlbUtils.cpp
    src/PtrchasePerfHelper.cpp
    src/Utils.cpp
    src/measurement/TrafficGenArrayCalibrator.cpp
    src/utils/PmuSysfs.cpp
    src/utils/CpuTopology.cpp
    src/utils/SubprocessCapture.cpp
)

set(MESS_PROFILER_SRCS
    MessProfiler.cpp
    src/SystemDetection.cpp
    src/BenchmarkConfig.cpp
    src/measurement/MeasurementUtils.cpp
    src/measurement/BandwidthMeasurerFactory.cpp
    src/measurement/MeasurementStorage.cpp
    src/process/TrafficGenProcessManager.cpp
    src/profiler/ProcessBinding.cpp
    src/profiler/ProfilerConfig.cpp
    src/utils/PmuSysfs.cpp
    src/utils/CpuTopology.cpp
    src/utils/SubprocessCapture.cpp
    src/Utils.cpp

)