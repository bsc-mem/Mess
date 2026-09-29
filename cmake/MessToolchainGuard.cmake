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
