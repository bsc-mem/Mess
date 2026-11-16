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


cmake_minimum_required(VERSION 3.21)
project(mess VERSION 2.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(MessPlatform)
include(MessToolchainGuard)
include(MessSources)

# Each build tree and configuration owns its executables.
set(MESS_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>" CACHE STRING
    "Executable and generated-kernel directory (use a unique path per configuration)")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MESS_RUNTIME_OUTPUT_DIRECTORY}")

set(MESS_SRCS ${MESS_CORE_SRCS})
set(MESS_GC_SRCS ${MESS_GENERATE_CODE_SRCS})
set(MESS_PROF_SRCS ${MESS_PROFILER_SRCS})
set(MESS_BACKEND_SRCS ${MESS_VTUNE_SRCS})
if(MESS_PLATFORM_WINDOWS)
    list(APPEND MESS_BACKEND_SRCS ${MESS_WINDOWS_ONLY_SRCS})
else()
    list(APPEND MESS_SRCS ${MESS_SAMPLER_SRCS})
    list(APPEND MESS_BACKEND_SRCS ${MESS_POSIX_BACKEND_SRCS})
endif()


if(MESS_PLATFORM_WINDOWS)
    set(MESS_PLATFORM_SRCS ${MESS_PLATFORM_WINDOWS_SRCS})
else()
    set(MESS_PLATFORM_SRCS ${MESS_PLATFORM_POSIX_SRCS})
endif()

add_library(mess_platform OBJECT ${MESS_PLATFORM_SRCS})
target_link_libraries(mess_platform PUBLIC mess_build_options mess_platform_options)
set_target_properties(mess_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mess_lib OBJECT ${MESS_LIB_SRCS})
target_link_libraries(mess_lib PUBLIC mess_build_options mess_platform)
set_target_properties(mess_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mess_arch OBJECT ${MESS_ARCH_SRCS})
target_link_libraries(mess_arch PUBLIC mess_build_options mess_platform)
set_target_properties(mess_arch PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mess_backends OBJECT ${MESS_BACKEND_SRCS})
target_link_libraries(mess_backends PUBLIC mess_build_options mess_platform)

add_library(mess_core OBJECT ${MESS_SRCS})
target_link_libraries(mess_core PUBLIC mess_build_options mess_platform)

add_executable(mess Mess.cpp)
target_link_libraries(mess PRIVATE mess_core mess_arch mess_lib mess_platform mess_backends)

add_executable(generate_code ${MESS_GC_SRCS})
target_link_libraries(generate_code PRIVATE mess_arch mess_lib mess_platform)

add_executable(mess-profiler ${MESS_PROF_SRCS})
target_link_libraries(mess-profiler PRIVATE mess_arch mess_lib mess_platform mess_backends)

set_source_files_properties(
    src/measurement/CpuFrequencyProbe.cpp
    src/measurement/PointerChaseTiming.cpp
    src/measurement/UprofParser.cpp
    src/gpu/GpuMeasurement.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math"
)

include(CTest)
if(BUILD_TESTING AND EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
    include("${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
endif()

# Kernel generation is explicit: it detects/calibrates the machine on which it runs.
if(NOT CMAKE_CROSSCOMPILING)
    add_custom_target(kernels COMMAND $<TARGET_FILE:generate_code>
        DEPENDS generate_code USES_TERMINAL VERBATIM)
endif()
