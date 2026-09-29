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

#include "measurement/UprofCollection.h"
#include "platform/Fs.h"
#include "platform/Process.h"
#include "utils/StringUtils.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace measurement::uprof {
namespace {
class CollectionDirectory {
public:
    std::filesystem::path path;
    CollectionDirectory() = default;
    CollectionDirectory(const CollectionDirectory&) = delete;
    CollectionDirectory& operator=(const CollectionDirectory&) = delete;
    ~CollectionDirectory() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    bool create(std::string& error) {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = std::filesystem::path(platform::temp_dir()) /
                ("mess_uprof_" + std::to_string(stamp) + "_" + std::to_string(serial++));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) { path = candidate; return true; }
            if (ec && ec != std::errc::file_exists) {
                error = "could not create uProf collection directory: " + ec.message();
                return false;
            }
        }
        error = "could not allocate a unique uProf collection directory";
        return false;
    }
};
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
} // namespace

std::string run_collection(const std::string& binary, int duration_s, bool cumulative,
                           int interval_ms, std::string& error,
                           std::chrono::milliseconds timeout) {
    error.clear();
    CollectionDirectory directory;
    if (!directory.create(error)) return {};
    const auto output = directory.path / "report.csv";
    std::vector<std::string> args{"-m", "memory", "-a"};
    if (cumulative) args.push_back("-C");
    else args.insert(args.end(), {"-I", std::to_string(std::max(kUprofReportIntervalMs, interval_ms))});
    const int duration = uprof_collection_seconds(duration_s);
    args.insert(args.end(), {"-q", "-d", std::to_string(duration), "-o", output.string()});
    platform::RunOptions opts;
    opts.timeout = timeout.count() < 0
        ? std::chrono::milliseconds{(static_cast<long long>(duration) + 60) * 1000}
        : timeout;
    const auto result = platform::run(binary, args, opts);
    if (result.spawn_failed || result.timed_out || result.exit_status != 0 || !result.error.empty()) {
        if (result.spawn_failed) error = "could not launch AMDuProfPcm: " + result.error;
        else if (result.timed_out) error = "AMDuProfPcm exceeded its collection timeout";
        else error = "AMDuProfPcm failed (exit " + std::to_string(result.exit_status) + "): " + result.error;
        const auto detail = string_utils::trim(result.output);
        if (!detail.empty()) error += " " + detail.substr(0, 400);
        return {};
    }
    auto csv = read_file(output);
    if (!csv.empty()) return csv;

    // uProf may add a session suffix. Only inspect this invocation's directory;
    // never pick an arbitrary report if several candidates exist.
    std::filesystem::path candidate;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory.path, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto path = it->path();
        if (path.extension() != ".csv" || path.stem().string().rfind("report", 0) != 0 ||
            path == output) continue;
        if (!candidate.empty()) {
            error = "AMDuProfPcm produced multiple CSV reports; refusing an ambiguous result";
            return {};
        }
        candidate = path;
    }
    if (ec) { error = "could not inspect uProf reports: " + ec.message(); return {}; }
    if (!candidate.empty()) csv = read_file(candidate);
    if (csv.empty()) {
        error = "AMDuProfPcm produced no usable CSV";
        const auto detail = string_utils::trim(result.output);
        if (!detail.empty()) error += ": " + detail.substr(0, 400);
    }
    return csv;
}
} // namespace measurement::uprof
