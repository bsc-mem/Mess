
set(MESS_PLATFORM_LINUX   FALSE)
set(MESS_PLATFORM_DARWIN  FALSE)
set(MESS_PLATFORM_WINDOWS FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MESS_PLATFORM_LINUX TRUE)
    set(MESS_PLATFORM_NAME "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(MESS_PLATFORM_DARWIN TRUE)
    set(MESS_PLATFORM_NAME "darwin")
elseif(WIN32)
    set(MESS_PLATFORM_WINDOWS TRUE)
    set(MESS_PLATFORM_NAME "windows")
else()
    message(FATAL_ERROR
        "Unsupported CMAKE_SYSTEM_NAME '${CMAKE_SYSTEM_NAME}'. "
        "Mess supports Linux, Darwin and Windows.")
endif()

add_library(mess_build_options INTERFACE)
target_compile_features(mess_build_options INTERFACE cxx_std_17)
target_compile_definitions(mess_build_options INTERFACE MESS_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_include_directories(mess_build_options INTERFACE
    "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/libraries"
    "${PROJECT_SOURCE_DIR}/include/arch")
target_compile_options(mess_build_options INTERFACE -Wall -Wextra -Wunused)

# Portable controller binaries by default; generated kernels retain their own
# hardware-specific code-generation policy. Never enable fast math globally.
option(MESS_NATIVE_OPTIMIZATION "Tune controller binaries for the build host" OFF)
if(MESS_NATIVE_OPTIMIZATION)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "MESS_NATIVE_OPTIMIZATION cannot be used while cross compiling")
    endif()
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" MESS_HAS_MARCH_NATIVE)
    if(NOT MESS_HAS_MARCH_NATIVE)
        message(FATAL_ERROR "This compiler does not support -march=native")
    endif()
    target_compile_options(mess_build_options INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:-march=native>)
endif()

add_library(mess_platform_options INTERFACE)
if(MESS_PLATFORM_DARWIN)
    target_compile_options(mess_build_options INTERFACE -fblocks)
    find_library(MESS_COREFOUNDATION CoreFoundation REQUIRED)
    find_library(MESS_IOKIT IOKit REQUIRED)
    target_link_libraries(mess_platform_options INTERFACE ${MESS_COREFOUNDATION} ${MESS_IOKIT})
endif()
if(MESS_PLATFORM_WINDOWS)
    target_compile_definitions(mess_build_options INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 __USE_MINGW_ANSI_STDIO=1)
    option(MESS_STATIC_RUNTIME "Link the MinGW runtime statically" ON)
    if(MESS_STATIC_RUNTIME)
        target_link_options(mess_platform_options INTERFACE -static -static-libgcc -static-libstdc++)
    endif()
    target_link_libraries(mess_platform_options INTERFACE powrprof)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mess_platform_options INTERFACE Threads::Threads m)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(mess_platform_options INTERFACE stdc++fs)
endif()
