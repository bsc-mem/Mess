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

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

namespace platform {

// FindFirstFileW expands wildcards only in the final component.
std::vector<std::string> list_matching(const std::string& pattern) {
    std::vector<std::string> out;
    if (pattern.empty()) return out;

    const size_t sep = pattern.find_last_of("\\/");
    const std::string dir = (sep == std::string::npos) ? std::string(".")
                                                       : pattern.substr(0, sep);
    const std::string leaf = (sep == std::string::npos) ? pattern
                                                        : pattern.substr(sep + 1);

    if (dir.find('*') != std::string::npos || dir.find('?') != std::string::npos) {
        return out;
    }

    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        out.push_back(dir + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end());
    return out;
}

std::string temp_dir() {
    char buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathA(sizeof(buf), buf);
    if (n == 0 || n > MAX_PATH) return ".";
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

// GetModuleFileNameW truncates and sets ERROR_INSUFFICIENT_BUFFER instead of reporting the
// size.
std::filesystem::path executable_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            std::error_code ec;
            std::filesystem::path p(buf.data(), buf.data() + n);
            std::filesystem::path resolved = std::filesystem::canonical(p, ec);
            return ec ? p : resolved;
        }
        if (buf.size() >= 32768) {
            return {};
        }
        buf.resize(buf.size() * 2);
    }
}

namespace {

std::vector<std::string> path_extensions() {
    std::vector<std::string> exts;
    const char* pathext = std::getenv("PATHEXT");
    if (!pathext || *pathext == '\0') {
        pathext = ".COM;.EXE;.BAT;.CMD";
    }

    std::stringstream stream(pathext);
    std::string entry;
    while (std::getline(stream, entry, ';')) {
        if (entry.empty()) continue;
        if (entry.front() != '.') entry.insert(entry.begin(), '.');
        std::transform(entry.begin(), entry.end(), entry.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        exts.push_back(entry);
    }
    return exts;
}

}

// There is no execute bit, so "executable" means an extension in PATHEXT.
bool is_executable(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }

    std::string ext = path.extension().string();
    if (ext.empty()) return false;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    const std::vector<std::string> exts = path_extensions();
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

std::optional<std::filesystem::path> which(const std::string& program) {
    if (program.empty()) {
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return std::nullopt;
    }

    const std::vector<std::string> exts = path_extensions();

    std::stringstream path_stream(std::string(".;") + path_env);
    std::string path_entry;
    while (std::getline(path_stream, path_entry, ';')) {
        if (path_entry.empty()) continue;

        const std::filesystem::path dir(path_entry);
        const std::filesystem::path bare = dir / program;
        if (is_executable(bare)) {
            return bare;
        }
        for (const std::string& ext : exts) {
            const std::filesystem::path candidate = dir / (program + ext);
            if (is_executable(candidate)) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

// "/tmp/x" resolves against the current drive's root, so both ends of the handshake use
// GetTempPath() instead.
std::string local_path(const std::string& posix_path) {
    if (posix_path == "/dev/null") {
        return "NUL";
    }

    const std::string prefix = "/tmp/";
    if (posix_path.compare(0, prefix.size(), prefix) != 0) {
        return posix_path;
    }

    std::string leaf = posix_path.substr(prefix.size());
    for (char& c : leaf) {
        if (c == '/') c = '\\';
    }
    return temp_dir() + "\\" + leaf;
}

std::string exe_name(const std::string& stem) {
    static const std::string kExe = ".exe";
    if (stem.size() >= kExe.size() &&
        _stricmp(stem.c_str() + (stem.size() - kExe.size()), kExe.c_str()) == 0) {
        return stem;
    }
    return stem + kExe;
}

}
