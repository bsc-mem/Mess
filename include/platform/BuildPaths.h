#pragma once
#include "platform/Fs.h"
#include <cstdlib>
#include <stdexcept>

namespace platform {
// Kernels and arrays travel with their controller, independent of cwd or sources.
inline std::filesystem::path runtime_directory() {
    const auto executable = executable_path();
    if (executable.empty()) throw std::runtime_error("Cannot locate the running executable");
    return executable.parent_path();
}
inline std::filesystem::path codegen_directory() { return runtime_directory() / ".codegen"; }
inline std::string kernel_c_compiler() {
    if (const char* override_cc = std::getenv("MESS_CC")) {
        if (*override_cc) return override_cc;
    }
#ifdef MESS_RUNTIME_C_COMPILER
    if (is_executable(MESS_RUNTIME_C_COMPILER)) return MESS_RUNTIME_C_COMPILER;
#endif
    if (auto compiler = which("gcc")) return compiler->string();
    return exe_name("gcc");
}
} // namespace platform
