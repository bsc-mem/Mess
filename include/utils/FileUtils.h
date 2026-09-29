/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
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

#ifndef UTILS_FILE_UTILS_H
#define UTILS_FILE_UTILS_H

#include <cctype>
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>

#include <dirent.h>
#include <unistd.h>

namespace file_utils {

inline bool read_first_line(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    if (!std::getline(f, out)) return false;
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
        out.pop_back();
    return true;
}

inline std::string read_token(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::string val;
    f >> val;
    return val;
}

inline bool read_int_file(const std::filesystem::path& path, int& value) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file >> value;
    return file.good() || file.eof();
}

inline bool read_long_long_file(const std::filesystem::path& path, long long& value) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file >> value;
    return file.good() || file.eof();
}

inline bool read_string_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::getline(file, out);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return true;
}

inline std::string choose_temp_dir() {
    const char* candidates[] = {"/dev/shm", "/tmp"};
    for (const char* d : candidates) {
        if (std::filesystem::exists(d) && access(d, W_OK) == 0) return d;
    }
    return "/tmp";
}

inline bool for_each_dir_entry(const std::string& dir_path,
                               const std::function<void(const std::string&)>& visit) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        visit(name);
    }

    closedir(dir);
    return true;
}

}

#endif
