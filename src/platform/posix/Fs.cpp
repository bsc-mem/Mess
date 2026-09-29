/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the copyright holder nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/Fs.h"

#include <glob.h>
#include <unistd.h>

#include <cstdlib>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace platform {

std::vector<std::string> list_matching(const std::string& pattern) {
    std::vector<std::string> out;

    glob_t matches{};
    if (glob(pattern.c_str(), 0, nullptr, &matches) != 0) {
        globfree(&matches);
        return out;
    }

    out.reserve(matches.gl_pathc);
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
        out.emplace_back(matches.gl_pathv[i]);
    }
    globfree(&matches);
    return out;
}

std::string temp_dir() {
    return "/tmp";
}

std::filesystem::path executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::canonical(buf.c_str(), ec);
    return ec ? std::filesystem::path(buf.c_str()) : resolved;
#else
    std::error_code ec;
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe_path;
#endif
}

bool is_executable(const std::filesystem::path& path) {
    return access(path.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> which(const std::string& program) {
    if (program.empty()) {
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return std::nullopt;
    }

    std::stringstream path_stream(path_env);
    std::string path_entry;
    while (std::getline(path_stream, path_entry, ':')) {
        if (path_entry.empty()) {
            path_entry = ".";
        }
        std::filesystem::path candidate = std::filesystem::path(path_entry) / program;
        if (is_executable(candidate)) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::string local_path(const std::string& posix_path) {
    return posix_path;
}

std::string exe_name(const std::string& stem) {
    return stem;
}

}
