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

if(MSVC)
    message(FATAL_ERROR
        "MSVC is not supported because generated kernels use GCC inline assembly. "
        "Use MinGW-w64 GCC.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
    message(FATAL_ERROR
        "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old; Mess needs C++17 (GCC 8+).")
endif()

# Do not bake a build-host cross compiler into programs that run on another OS.
set(MESS_RUNTIME_C_COMPILER "" CACHE FILEPATH "Native C compiler used for runtime kernels; MESS_CC overrides it")
if(NOT MESS_RUNTIME_C_COMPILER AND NOT CMAKE_CROSSCOMPILING)
    set(MESS_RUNTIME_C_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "Runtime kernel compiler" FORCE)
endif()
if(MESS_RUNTIME_C_COMPILER)
    file(TO_CMAKE_PATH "${MESS_RUNTIME_C_COMPILER}" runtime_cc)
    target_compile_definitions(mess_build_options INTERFACE MESS_RUNTIME_C_COMPILER="${runtime_cc}")
endif()
