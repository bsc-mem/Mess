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

#include "measurement/VtuneCounterParser.h"
#include "Utils.h"
#include "utils/SubprocessCapture.h"
#include "utils/StringUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <optional>
#include <vector>
#include "platform/Process.h"
#include "platform/Fs.h"
#if !defined(_WIN32)
// access() and mkdtemp(): libc++ does not pull these in.
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#include <winsvc.h>
#endif

#include "measurement/VtuneCollection.h"
#include <mutex>
namespace measurement::vtune {
struct CollectionSession::Impl {
    std::string binary_key;

#if !defined(_WIN32)
    static constexpr double kMinVtuneRunsaWindowSeconds = 0.5;
    static constexpr double kMinEmonWindowSeconds = 0.5;
    const char* kLegacyVtuneEventConfig =
        "UNC_M_CAS_COUNT.ALL";
    const char* kEmonReadEvent = "UNC_M_CAS_COUNT.RD";
    const char* kEmonWriteEvent = "UNC_M_CAS_COUNT.WR";

    std::string format_seconds(double seconds) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << seconds;
        return oss.str();
    }
#endif

#if !defined(_WIN32)
    std::string format_whole_seconds(double seconds) {
        const int whole_seconds = std::max(1, static_cast<int>(seconds + 0.999999));
        return std::to_string(whole_seconds);
    }
#endif

    std::string choose_vtune_result_root() {
#if defined(_WIN32)
        return platform::temp_dir();
#else
        const char* candidates[] = {"/dev/shm", "/tmp"};
        for (const char* candidate : candidates) {
            std::error_code ec;
            const std::filesystem::path path(candidate);
            if (std::filesystem::exists(path, ec) &&
                std::filesystem::is_directory(path, ec) &&
                access(candidate, W_OK | X_OK) == 0) {
                return candidate;
            }
        }
        return "/tmp";
#endif
    }

#if !defined(_WIN32)
    void append_unique_event(std::vector<std::string>& events, const std::string& event) {
        if (event.empty()) {
            return;
        }

        for (const auto& existing : events) {
            if (string_utils::to_lower(existing) == string_utils::to_lower(event)) {
                return;
            }
        }
        events.push_back(event);
    }

    std::string event_config_from_events(const std::vector<std::string>& events) {
        std::ostringstream oss;
        for (size_t i = 0; i < events.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << events[i];
        }
        return oss.str();
    }

    std::vector<std::string> vtune_legacy_events_with_extras(const BandwidthCounterSelection& selection) {
        std::vector<std::string> events;
        std::istringstream legacy_stream(kLegacyVtuneEventConfig);
        std::string event;
        while (std::getline(legacy_stream, event, ',')) {
            append_unique_event(events, string_utils::trim(event));
        }

        for (const auto& extra : selection.extra_counters) {
            const std::vector<std::string> aliases = vtune_event_aliases(extra);
            append_unique_event(events, aliases.empty() ? canonicalize_vtune_event_name(extra) : aliases.front());
        }

        return events;
    }

    std::string build_legacy_vtune_event_config(const BandwidthCounterSelection& selection) {
        return event_config_from_events(vtune_legacy_events_with_extras(selection));
    }

    std::string extract_command_path_from_output(const std::string& output,
                                                 const std::string& command_name) {
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            const std::string trimmed = string_utils::trim(line);
            if (trimmed.empty()) {
                continue;
            }

            const std::filesystem::path path(trimmed);
            if (path.filename() == command_name || trimmed == command_name) {
                return trimmed;
            }
        }
        return "";
    }
#endif

    std::string build_vtune_event_config(const BandwidthCounterSelection& selection) {
        std::vector<std::string> events;

        if (selection.type == CounterType::CAS_COUNT ||
            selection.type == CounterType::NVIDIA_GRACE ||
            selection.type == CounterType::TOTAL_BYTES) {
            events.insert(events.end(), selection.cas.read_events.begin(), selection.cas.read_events.end());
            events.insert(events.end(), selection.cas.write_events.begin(), selection.cas.write_events.end());
            events.insert(events.end(), selection.cas.combined_events.begin(), selection.cas.combined_events.end());
        } else if (selection.type == CounterType::UPI_FLITS) {
            events.insert(events.end(), selection.upi.rxl_data_events.begin(), selection.upi.rxl_data_events.end());
            events.insert(events.end(), selection.upi.txl_data_events.begin(), selection.upi.txl_data_events.end());
        }

        events.insert(events.end(), selection.extra_counters.begin(), selection.extra_counters.end());

        std::ostringstream oss;
        for (size_t i = 0; i < events.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            const std::vector<std::string> aliases = vtune_event_aliases(events[i]);
            oss << (aliases.empty() ? canonicalize_vtune_event_name(events[i]) : aliases.front());
        }
        return oss.str();
    }

#if defined(_WIN32)
    /**
     * @brief One way of asking VTune for memory-controller counts on Windows.
     *
     * Counter names differ by part and some flag combinations report nothing
     * without erroring, so the recipes are tried in order once at startup and the
     * first that returns counter lines is kept for the run.
     */
    struct WindowsVtuneRecipe {
        const char*              label;
        std::vector<std::string> read_events;
        std::vector<std::string> write_events;
        bool                     no_summary;      /**< pass -no-summary */
        bool                     discard_raw;     /**< pass -discard-raw-data */
        bool                     analyze_system;  /**< -analyze-system instead of a target */
        // "none" skips post-processing, the largest cost in a collection. Whether an unfinalized
        // result still reports counts is undocumented, so the probe settles it.
        const char*              finalization;
    };

    const std::vector<WindowsVtuneRecipe>& windows_vtune_recipes() {
        static const std::vector<WindowsVtuneRecipe> recipes = {
            {"client-cas-fast",
             {"UNC_M_CAS_COUNT_RD"}, {"UNC_M_CAS_COUNT_WR"},
             true, true, false, "none"},

            {"client-cas",
             {"UNC_M_CAS_COUNT_RD"}, {"UNC_M_CAS_COUNT_WR"},
             false, false, false, "fast"},

            {"client-freerun",
             {"UNC_MC0_RDCAS_COUNT_FREERUN", "UNC_MC1_RDCAS_COUNT_FREERUN"},
             {"UNC_MC0_WRCAS_COUNT_FREERUN", "UNC_MC1_WRCAS_COUNT_FREERUN"},
             false, false, false, "fast"},

            {"server-cas",
             {"UNC_M_CAS_COUNT.RD"}, {"UNC_M_CAS_COUNT.WR"},
             false, false, false, "fast"},

            {"client-cas-system",
             {"UNC_M_CAS_COUNT_RD"}, {"UNC_M_CAS_COUNT_WR"},
             false, false, true, "fast"},
        };
        return recipes;
    }

    const WindowsVtuneRecipe* windows_recipe = nullptr;
    bool windows_probe_done = false;
    bool windows_probing    = false;
    bool windows_probe_ok   = false;

    bool process_is_elevated() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != 0;
        CloseHandle(token);
        return ok && elevation.TokenIsElevated != 0;
    }

    /**
     * @brief Whether any of VTune's sampling driver services is registered.
     *
     * Without one every collection fails with "Cannot enable Hardware Event-Based
     * Sampling", which looks like an unsupported processor. Matched by prefix: the
     * names carry a version suffix (sepdrv5_2).
     */
    bool sampling_driver_registered() {
        SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (!scm) return false;

        DWORD needed = 0, count = 0, resume = 0;
        EnumServicesStatusExA(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
                              nullptr, 0, &needed, &count, &resume, nullptr);

        bool found = false;
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            if (EnumServicesStatusExA(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER,
                                      SERVICE_STATE_ALL, buf.data(), needed, &needed,
                                      &count, &resume, nullptr)) {
                const auto* svc = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSA*>(buf.data());
                for (DWORD i = 0; i < count && !found; ++i) {
                    if (!svc[i].lpServiceName) continue;
                    std::string name = svc[i].lpServiceName;
                    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    found = name.rfind("sepdrv", 0) == 0 || name.rfind("socperf", 0) == 0 ||
                            name.rfind("sepdal", 0) == 0 || name.rfind("vtss", 0) == 0;
                }
            }
        }
        CloseServiceHandle(scm);
        return found;
    }

    /**
     * @brief True only when VTune answered `-knob event-config=?` and named nothing.
     *
     * An empty list means VTune does not support this part (current builds dropped
     * pre-Ice-Lake). Fails open: "could not ask" must not read as "asked, and none".
     */
    bool answered_none = false;
    bool loaded = false;
    bool vtune_reports_no_events(const std::string& vtune_binary) {
        if (loaded) return answered_none;
        loaded = true;

        const std::string cmd = string_utils::shell_quote(vtune_binary) +
                                " -collect-with runsa -knob event-config=? 2>&1";
        std::FILE* pipe = platform::open_pipe(cmd.c_str(), "r");
        if (!pipe) return false;                    // could not ask

        std::string output;
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
        platform::close_pipe(pipe);

        const auto marker = output.find("Possible values:");
        if (marker == std::string::npos) return false;   // did not answer

        const std::string list = output.substr(marker + std::strlen("Possible values:"));
        for (const char c : list) {
            if (!std::isspace(static_cast<unsigned char>(c)) && c != ',') return false;
        }
        answered_none = true;
        return true;
    }

    /** @brief Absolute path of amplxe-sepreg.exe, which sits beside vtune.exe. */
    std::string sepreg_path(const std::string& vtune_binary) {
        std::error_code ec;
        const std::filesystem::path p =
            std::filesystem::path(vtune_binary).parent_path() / "amplxe-sepreg.exe";
        return std::filesystem::exists(p, ec) ? p.string() : std::string();
    }
    bool windows_probe_quiet = false;

    /** @brief The strategy's selection with the recipe's counter names swapped in. */
    BandwidthCounterSelection with_recipe_events(const BandwidthCounterSelection& base,
                                                 const WindowsVtuneRecipe& recipe) {
        BandwidthCounterSelection out = base;
        out.cas.read_events  = recipe.read_events;
        out.cas.write_events = recipe.write_events;
        out.cas.combined_events.clear();
        return out;
    }
#endif  // _WIN32

#if defined(_WIN32)

    /** @brief A private result directory per collection; mkdtemp has no Win32 twin. */
    std::string make_windows_result_dir() {
        static std::atomic<unsigned> counter{0};
        std::filesystem::path dir(choose_vtune_result_root());
        dir /= "mess_vtune_" +
               std::to_string(static_cast<unsigned long long>(
                   std::chrono::steady_clock::now().time_since_epoch().count())) +
               "_" + std::to_string(counter.fetch_add(1));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return {};
        return dir.string();
    }
#endif  // _WIN32

#if defined(_WIN32)
    // emon reads the same counters without creating a result directory.

    struct EmonShape {
        const char* label;
        /** %E events, %S seconds per sample, %L samples per process. */
        const char* arg_template;
        /** Whether %L appears: only then can one process yield several samples. */
        bool        supports_loops;
    };

    const std::vector<EmonShape>& emon_shapes() {
        static const std::vector<EmonShape> shapes = {
            {"loop-1x",  "-C \"%E\" -l %L -t %S", true},
            {"duration", "-C \"%E\" -t %S",       false},
            {"bare",     "-C \"%E\"",             false},
        };
        return shapes;
    }

    const EmonShape* emon_shape = nullptr;
    bool emon_probe_done = false;
    bool emon_usable     = false;
    // emon's -t is not VTune's -duration. A fraction is used only once the probe confirms it.
    bool emon_subsecond  = false;
    // Roughly 1.7 s of driver attach and teardown per emon process, so one process serves the whole
    // window.
    bool emon_batching   = false;

    /** @brief emon.exe, which the installer puts beside vtune.exe. */
    std::string emon_binary(const std::string& vtune_binary) {
        if (vtune_binary.empty()) return {};
        std::error_code ec;
        const std::filesystem::path p =
            std::filesystem::path(vtune_binary).parent_path() / "emon.exe";
        return std::filesystem::exists(p, ec) ? p.string() : std::string();
    }

    /**
     * @brief Seconds as emon wants to read them: 0.1, not 0.100000, and 2, not 2.0.
     *
     * Trailing zeros are trimmed because a value is being handed to another tool's
     * argument parser, and VTune already refused "2.000" where it wanted "2".
     */
    std::string emon_format_seconds(double seconds) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << seconds;
        std::string out = oss.str();
        if (out.find('.') != std::string::npos) {
            while (!out.empty() && out.back() == '0') out.pop_back();
            if (!out.empty() && out.back() == '.') out.pop_back();
        }
        return out;
    }

    std::string build_emon_command(const std::string& binary, const EmonShape& shape,
                                   const std::string& events, double seconds, int loops) {
        std::string args = shape.arg_template;
        const std::string secs  = emon_format_seconds(seconds);
        const std::string count = std::to_string(std::max(1, loops));
        for (size_t at = args.find("%E"); at != std::string::npos; at = args.find("%E", at + events.size())) {
            args.replace(at, 2, events);
        }
        for (size_t at = args.find("%S"); at != std::string::npos; at = args.find("%S", at + secs.size())) {
            args.replace(at, 2, secs);
        }
        for (size_t at = args.find("%L"); at != std::string::npos; at = args.find("%L", at + count.size())) {
            args.replace(at, 2, count);
        }
        return string_utils::shell_quote(binary) + " -experimental " + args + " 2>&1";
    }

    /**
     * @brief One emon process, one sample per loop it printed.
     *
     * @return One entry per sample that yielded both counters. Never empty: a run
     *         that produced nothing comes back as a single unsuccessful sample
     *         carrying the error and the raw output, so a caller can report it.
     */
    std::vector<VtuneSample> run_emon_batch(const std::string& emon_exe,
                                            const BandwidthCounterSelection& selection,
                                            double seconds, int loops,
                                            const EmonShape& shape, int verbosity) {
        std::vector<VtuneSample> samples;
        const std::string events = build_vtune_event_config(selection);
        const std::string cmd = build_emon_command(emon_exe, shape, events, seconds, loops);

        const auto started = std::chrono::steady_clock::now();
        std::FILE* pipe = platform::open_pipe(cmd.c_str(), "r");
        if (!pipe) {
            VtuneSample failed;
            failed.error = "Failed to launch emon";
            samples.push_back(failed);
            return samples;
        }

        std::string output;
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
        const int code = platform::exit_code_of(platform::close_pipe(pipe));
        const auto finished = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(finished - started).count();

        return parse_emon_batch(output, selection, seconds, wall,
            std::string(shape.arg_template).find("%S") != std::string::npos, code, cmd, verbosity);
    }

    /** @brief Probes the installed emon command shape once. */
    bool emon_probe(const std::string& emon_exe,
                    const BandwidthCounterSelection& selection, int verbosity) {
        if (emon_probe_done) return emon_usable;
        emon_probe_done = true;
        if (emon_exe.empty()) return false;

        // Attempts in increasing order of concession. Two loops rather than trusting -l.
        struct Attempt { double seconds; int loops; };
        static const Attempt attempts[] = {{0.5, 2}, {0.5, 1}, {2.0, 2}, {2.0, 1}};

        for (const auto& shape : emon_shapes()) {
            if (verbosity >= 3) {
                std::cout << "    Trying emon shape '" << shape.label << "'..." << '\n';
            }

            std::vector<VtuneSample> last;
            for (const auto& attempt : attempts) {
                if (attempt.loops > 1 && !shape.supports_loops) continue;

                last = run_emon_batch(emon_exe, selection, attempt.seconds, attempt.loops,
                                      shape, verbosity);

                bool ok = (static_cast<int>(last.size()) == attempt.loops);
                for (const auto& sample : last) {
                    if (!sample.success) ok = false;
                }
                if (!ok) continue;

                emon_shape     = &shape;
                emon_usable    = true;
                emon_subsecond = (attempt.seconds < 1.0);
                emon_batching  = (attempt.loops > 1);
                if (verbosity >= 2) {
                    std::cout << "    emon works (" << shape.label << ", "
                              << emon_format_seconds(attempt.seconds) << " s"
                              << (emon_batching ? ", batched" : "")
                              << "); using it instead of VTune collections." << '\n';
                }
                return true;
            }

            if (verbosity >= 3 && !last.empty()) {
                std::cout << "      no counters"
                          << (last.front().error.empty() ? "" : (": " + last.front().error))
                          << '\n';
                std::istringstream raw(last.front().raw_output);
                std::string l;
                int shown = 0;
                while (std::getline(raw, l) && shown < 5) {
                    const std::string t = string_utils::trim(l);
                    if (t.empty()) continue;
                    std::cout << "      | " << t << '\n';
                    ++shown;
                }
            }
        }
        if (verbosity >= 2) {
            std::cout << "    emon returned no counters; using VTune collections." << '\n';
        }
        return false;
    }

    bool try_emon_samples(const std::string& vtune_binary,
                          const BandwidthCounterSelection& selection_in,
                          double window_seconds, int loops, int verbosity,
                          std::vector<VtuneSample>& out) {
        out.clear();
        if (vtune_binary.empty() || !selection_in.is_valid()) return false;

        if (!windows_probe_done && !windows_probing) {
            windows_probe_vtune_recipe(vtune_binary, selection_in, verbosity);
        }
        if (!windows_probe_ok || windows_probing) return false;

        const WindowsVtuneRecipe& recipe =
            windows_recipe ? *windows_recipe : windows_vtune_recipes().front();
        const BandwidthCounterSelection selection = with_recipe_events(selection_in, recipe);

        const std::string emon_exe = emon_binary(vtune_binary);
        if (!emon_probe(emon_exe, selection, verbosity) || !emon_shape) return false;

        // Use whole seconds when the installed emon rejects fractional intervals.
        const double requested = (window_seconds > 0.0) ? window_seconds : 2.0;
        const double seconds   = emon_subsecond ? std::max(0.1, requested)
                                                  : std::max(1.0, std::ceil(requested));
        const int    ask_loops = emon_batching ? std::max(1, loops) : 1;

        std::vector<VtuneSample> batch =
            run_emon_batch(emon_exe, selection, seconds, ask_loops, *emon_shape, verbosity);
        for (auto& sample : batch) {
            if (sample.success) out.push_back(std::move(sample));
        }
        if (!out.empty()) return true;

        if (verbosity >= 2 && !batch.empty() && !batch.front().error.empty()) {
            std::cerr << "    emon stopped returning counters (" << batch.front().error
                      << "); falling back to VTune collections." << std::endl;
        }
        emon_usable   = false;
        emon_shape    = nullptr;
        emon_batching = false;
        return false;
    }

#endif  // _WIN32

    VtuneSample run_vtune_collection(const std::string& vtune_binary,
                                     const BandwidthCounterSelection& selection_in,
                                     double duration_seconds,
                                     const std::string& command,
                                     const std::string& event_config_override,
                                     int verbosity) {
        VtuneSample sample;

        if (!selection_in.is_valid()) {
            sample.error = "Counter selection not initialized";
            return sample;
        }

#if defined(_WIN32)
        (void)event_config_override;
        if (!windows_probe_done && !windows_probing) {
            windows_probe_vtune_recipe(vtune_binary, selection_in, verbosity);
        }
        const WindowsVtuneRecipe& recipe =
            windows_recipe ? *windows_recipe : windows_vtune_recipes().front();

        const BandwidthCounterSelection windows_selection =
            with_recipe_events(selection_in, recipe);
        const BandwidthCounterSelection& selection = windows_selection;

        const std::string result_dir = make_windows_result_dir();
        if (result_dir.empty()) {
            sample.error = "Failed to create temporary VTune result directory";
            return sample;
        }
        const std::filesystem::path result_path(result_dir);
        const std::string event_config = build_vtune_event_config(selection);
#else
        (void)duration_seconds;
        const BandwidthCounterSelection& selection = selection_in;

        std::string tmp_template = choose_vtune_result_root() + "/mess_vtune_XXXXXX";
        std::vector<char> tmp_buffer(tmp_template.begin(), tmp_template.end());
        tmp_buffer.push_back('\0');
        char* tmp_dir = mkdtemp(tmp_buffer.data());
        if (!tmp_dir) {
            sample.error = "Failed to create temporary VTune result directory";
            return sample;
        }
        const std::filesystem::path result_root(tmp_dir);
        const std::filesystem::path result_path = result_root / "result";

        std::string event_config = event_config_override.empty()
            ? build_vtune_event_config(selection)
            : event_config_override;
#endif

        std::string collect_cmd =
            string_utils::shell_quote(vtune_binary) +
#if defined(_WIN32)
            " -collect-with runsa -quiet -result-dir " + string_utils::shell_quote(result_path.string()) +
            " -finalization-mode=" + std::string(recipe.finalization ? recipe.finalization : "fast");
        if (recipe.discard_raw) collect_cmd += " -discard-raw-data";
        if (recipe.no_summary)  collect_cmd += " -no-summary";
        collect_cmd += " -knob enable-stack-collection=false";
        // 10 ms is a hotspots figure. Nothing here reads core events, so coarser sampling leaves
        // the totals untouched.
        collect_cmd += " -knob sampling-interval=100";
        collect_cmd += " -knob event-config=" + string_utils::shell_quote(event_config);
        if (duration_seconds >= 1.0) {
            // VTune rejects a fractional -duration.
            collect_cmd += " -duration=" +
                std::to_string(static_cast<long long>(std::ceil(duration_seconds)));
        }
        // -analyze-system cannot be combined with a target command.
        if (recipe.analyze_system) {
            collect_cmd += " -analyze-system";
        } else if (!command.empty()) {
            collect_cmd += " -- " + command;
        }
#else
            " -collect-with runsa" + (verbosity >= 3 ? "" : " -quiet") +
            " -result-dir " + string_utils::shell_quote(result_path.string());
        collect_cmd += " -knob event-config=" + string_utils::shell_quote(event_config);
        if (!command.empty()) {
            collect_cmd += " -- " + command;
        }
#endif
        collect_cmd += " 2>&1";
#if defined(_WIN32)
        const std::string report_cmd =
            string_utils::shell_quote(vtune_binary) +
            " -report summary -quiet -result-dir " + string_utils::shell_quote(result_path.string()) +
            " 2>&1";
#endif

        if (verbosity >= 4) {
            std::cout << "[VTUNE DEBUG] collect-command: " << collect_cmd << std::endl;
            std::cout << "[VTUNE DEBUG] result-dir: " << result_path << std::endl;
            std::cout << "[VTUNE DEBUG] events: " << event_config << std::endl;
        }

        std::string collect_output;
        char buffer[4096];
#if defined(_WIN32)
        FILE* pipe = platform::open_pipe(collect_cmd.c_str(), "r");
        if (!pipe) {
            std::filesystem::remove_all(result_path);
            sample.error = "Failed to launch VTune";
            return sample;
        }
        while (fgets(buffer, sizeof(buffer), pipe)) {
            collect_output += buffer;
        }
#else
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            std::filesystem::remove_all(result_root);
            sample.error = "Failed to create pipe for VTune";
            return sample;
        }

        pid_t child_pid = fork();
        if (child_pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            std::filesystem::remove_all(result_root);
            sample.error = "Failed to fork for VTune";
            return sample;
        }

        if (child_pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            const int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
            for (int fd = 3; fd < max_fd; fd++) {
                close(fd);
            }

            execl("/bin/sh", "sh", "-lc", collect_cmd.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(pipefd[1]);
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            collect_output += buffer;
        }
#endif

#if defined(_WIN32)
        // exit_code_of() yields -1 for a child that did not exit normally.
        int code = platform::exit_code_of(platform::close_pipe(pipe));
        if (code >= 0) {
            sample.exit_code = code;
        }
        const bool collect_ok = (code == 0);
#else
        close(pipefd[0]);

        int status = 0;
        waitpid(child_pid, &status, 0);
        int rc = status;
        if (WIFEXITED(rc)) {
            sample.exit_code = WEXITSTATUS(rc);
        }
        if (verbosity >= 4) {
            std::cout << "[VTUNE DEBUG] collect-exit-code: " << sample.exit_code << std::endl;
            if (!collect_output.empty()) {
                std::cout << "[VTUNE DEBUG] collect-output-begin\n"
                          << collect_output
                          << "[VTUNE DEBUG] collect-output-end" << std::endl;
            }
        }
        const bool collect_ok = (rc != -1) && WIFEXITED(rc) && (WEXITSTATUS(rc) == 0);
        bool command_ok = collect_ok;

        std::filesystem::remove_all(result_root);
#endif
        if (!collect_ok) {
#if defined(_WIN32)
            sample.raw_output = collect_output;
            std::filesystem::remove_all(result_path);
#endif
            const std::string vtune_error = first_meaningful_vtune_error(collect_output);
            sample.error = !vtune_error.empty() ? vtune_error : "VTune collect command failed";
#if defined(_WIN32)
            return sample;
#endif
        }

#if defined(_WIN32)
        pipe = platform::open_pipe(report_cmd.c_str(), "r");
        if (!pipe) {
            sample.raw_output = collect_output;
            std::filesystem::remove_all(result_path);
            sample.error = "Failed to launch VTune summary report";
            return sample;
        }

        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        code = platform::exit_code_of(platform::close_pipe(pipe));
        if (code >= 0) {
            sample.exit_code = code;
        }
        const bool command_ok = (code == 0);
        sample.raw_output = collect_output;
        if (!sample.raw_output.empty() && !output.empty()) {
            sample.raw_output += "\n";
        }
        sample.raw_output += output;
        std::filesystem::remove_all(result_path);

        const std::string vtune_error = first_meaningful_vtune_error(sample.raw_output);
        const std::string& parse_output = output;
#else
        const std::string vtune_error = first_meaningful_vtune_error(collect_output);
        const std::string& parse_output = collect_output;
#endif

#ifdef _WIN32
        constexpr bool legacy_events = false;
#else
        constexpr bool legacy_events = true;
#endif
        return parse_vtune_report(parse_output, selection, std::move(sample), command_ok,
            vtune_error, legacy_events, verbosity);
    }

#if defined(_WIN32)
    std::string make_sleep_command(double seconds, int helper_cpu) {
#if defined(_WIN32)
        // ping waits a second between echoes, so N echoes is N-1 seconds. No ">NUL": cmd would
        // apply it to the whole line.
        (void)helper_cpu;
        const long long echoes = std::max(2LL, static_cast<long long>(std::ceil(seconds)) + 1);
        return "ping -n " + std::to_string(echoes) + " 127.0.0.1";
#else
        const std::string sleep_seconds = format_seconds(seconds);
        if (helper_cpu >= 0 && run_command_success("command -v taskset >/dev/null 2>&1")) {
            return "taskset -c " + std::to_string(helper_cpu) + " sleep " + sleep_seconds;
        }
        return "sleep " + sleep_seconds;
#endif
    }

#if defined(_WIN32)
    /**
     * @brief Finds a recipe that actually returns counts, cached in this session.
     *
     * Success is "the report held a read line and a write line", not "the numbers
     * look right" - at startup there is no load to judge against.
     */
    void windows_probe_vtune_recipe(const std::string& vtune_binary,
                                    const BandwidthCounterSelection& selection,
                                    int verbosity) {
        if (windows_probe_done) return;

        windows_probing   = true;
        windows_probe_done = true;

        const double probe_seconds = 2.0;
        const std::string probe_command = make_sleep_command(probe_seconds, -1);

        for (const auto& recipe : windows_vtune_recipes()) {
            windows_recipe = &recipe;

            if (verbosity >= 3) {
                std::cout << "    Trying VTune counter set '" << recipe.label << "'..." << '\n';
            }

            const VtuneSample probe = run_vtune_collection(vtune_binary, selection,
                                                           probe_seconds, probe_command,
                                                           {}, verbosity);

            // A recipe returning only half the pair would leave every write count at zero.
            if (probe.matched_lines >= 2) {
                if (verbosity >= 2) {
                    std::cout << "    VTune counters: " << recipe.label << " ("
                              << build_vtune_event_config(with_recipe_events(selection, recipe))
                              << ")" << '\n';
                }
                windows_probing = false;
                windows_probe_ok = true;
                return;
            }

            if (verbosity >= 3) {
                std::cout << "      no counter lines returned"
                          << (probe.error.empty() ? "" : (": " + probe.error)) << '\n';

                if (probe.raw_output.empty()) {
                    std::cout << "      (vtune produced no output at all)" << '\n';
                } else {
                    std::istringstream raw(probe.raw_output);
                    std::string raw_line;
                    int shown = 0;
                    while (std::getline(raw, raw_line) && shown < 8) {
                        const std::string t = string_utils::trim(raw_line);
                        if (t.empty()) continue;
                        std::cout << "      | " << t << '\n';
                        ++shown;
                    }
                }
            }
        }

        // Keep the first recipe so the failure the user sees is consistent.
        windows_recipe  = &windows_vtune_recipes().front();
        windows_probing = false;

        if (!windows_probe_quiet) {
            std::cerr << "    WARNING: none of the " << windows_vtune_recipes().size()
                      << " known VTune counter sets returned memory-controller counts on "
                         "this processor. Bandwidth will read zero. Run with -v 3 to see "
                         "what each one reported." << '\n';
        }
    }
#endif  // _WIN32

    int local_socket_helper_cpu(const system_info& sys_info) {
        if (sys_info.total_physical_cores > 0 && sys_info.socket_count > 0) {
            const int physicals_per_socket = std::max(1, sys_info.total_physical_cores / sys_info.socket_count);
            return std::max(0, physicals_per_socket - 1);
        }

        if (sys_info.total_logical_cores <= 0) {
            return 0;
        }

        const int sockets = std::max(1, sys_info.socket_count);
        const int logicals_per_socket = std::max(1, sys_info.total_logical_cores / sockets);
        return std::max(0, logicals_per_socket - 1);
    }

    double effective_vtune_window_seconds(double requested_ms) {
        return std::max(0.1, requested_ms / 1000.0);
    }
#else
    std::string find_emon_binary(const std::string& vtune_binary) {
        if (!vtune_binary.empty()) {
            std::error_code ec;
            const std::filesystem::path vtune_path(vtune_binary);
            if (!vtune_path.parent_path().empty()) {
                const std::filesystem::path candidate = vtune_path.parent_path() / "emon";
                if (std::filesystem::exists(candidate, ec)) {
                    return candidate.string();
                }
            }
        }

        const std::string output = subprocess::run_shell("command -v emon 2>/dev/null");
        return extract_command_path_from_output(output, "emon");
    }

    VtuneSample run_emon_collection(const std::string& emon_binary,
                                    const BandwidthCounterSelection& selection,
                                    double window_seconds,
                                    int verbosity) {
        VtuneSample sample;

        const std::string duration = format_whole_seconds(std::max(kMinEmonWindowSeconds, window_seconds));
        std::vector<std::string> emon_events = {kEmonReadEvent, kEmonWriteEvent};
        for (const auto& extra : selection.extra_counters) {
            const std::vector<std::string> aliases = vtune_event_aliases(extra);
            append_unique_event(emon_events, aliases.empty() ? canonicalize_vtune_event_name(extra) : aliases.front());
        }
        const std::string events = event_config_from_events(emon_events);
        const std::string cmd = string_utils::shell_quote(emon_binary) +
            " -experimental -C " + string_utils::shell_quote(events) +
            " -t " + duration + " -l 1 2>&1";

        if (verbosity >= 4) {
            std::cout << "[VTUNE DEBUG] emon-command: " << cmd << std::endl;
        }

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            sample.error = "Failed to launch emon";
            return sample;
        }
        std::string output;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        const int rc = pclose(pipe);
        if (rc != -1 && WIFEXITED(rc)) {
            sample.exit_code = WEXITSTATUS(rc);
        }

        if (verbosity >= 4) {
            std::cout << "[VTUNE DEBUG] emon-exit-code: " << sample.exit_code << std::endl;
            if (!output.empty()) {
                std::cout << "[VTUNE DEBUG] emon-output-begin\n" << output
                          << "[VTUNE DEBUG] emon-output-end" << std::endl;
            }
        }

        return parse_emon_report(output, selection, window_seconds, std::move(sample), verbosity);
    }

    std::string make_sleep_command(double seconds, int helper_cpu) {
        const std::string sleep_seconds = format_seconds(seconds);
        const std::string shell_cmd = "sh -c 'for i in $(seq 1 20000); do :; done; sleep " + sleep_seconds + "'";

        if (helper_cpu >= 0 && run_command_success("command -v numactl >/dev/null 2>&1")) {
            return "numactl -C " + std::to_string(helper_cpu) + " " + shell_cmd;
        }
        if (helper_cpu >= 0 && run_command_success("command -v taskset >/dev/null 2>&1")) {
            return "taskset -c " + std::to_string(helper_cpu) + " " + shell_cmd;
        }
        return shell_cmd;
    }

    int select_vtune_helper_cpu(const BenchmarkConfig& config, int traffic_gen_cores) {
        const std::vector<int> allowed_cpus = get_allowed_cpus();
        if (allowed_cpus.empty()) {
            return -1;
        }

        const auto& cache = SystemToolsCache::instance();
        const int reserved_cpu = cache.initialized ? cache.ptr_chase_cpu : find_ptr_chase_cpu().first;

        std::set<int> busy_cpus;
        busy_cpus.insert(reserved_cpu);
        if (!config.traffic_gen_explicit_cores.empty()) {
            for (const auto& core : config.traffic_gen_explicit_cores) {
                int parsed = -1;
                if (string_utils::parse_int_strict(core, parsed)) {
                    busy_cpus.insert(parsed);
                }
            }
        } else if (traffic_gen_cores > 0) {
            for (int cpu : select_worker_cpus(traffic_gen_cores, reserved_cpu)) {
                busy_cpus.insert(cpu);
            }
        }

        for (int cpu : allowed_cpus) {
            if (busy_cpus.count(cpu) == 0) {
                return cpu;
            }
        }
        return -1;
    }

    double effective_vtune_window_seconds(double requested_ms) {
        return std::max(kMinVtuneRunsaWindowSeconds, requested_ms / 1000.0);
    }

    VtuneSample collect_window_sample(const std::string& vtune_binary,
                                      const BandwidthCounterSelection& selection,
                                      double window_seconds,
                                      int helper_cpu,
                                      int verbosity) {
        if (selection.type == CounterType::CAS_COUNT) {
            const std::string emon_binary = find_emon_binary(vtune_binary);
            if (!emon_binary.empty()) {
                VtuneSample sample = run_emon_collection(emon_binary, selection, window_seconds, verbosity);
                if (sample.success) {
                    return sample;
                }
                if (verbosity >= 2) {
                    std::cerr << "emon counting-mode sampling failed: " << sample.error
                              << " (exit-code=" << sample.exit_code
                              << "); falling back to vtune runsa." << std::endl;
                }
            } else if (verbosity >= 2) {
                std::cerr << "emon not found next to vtune or in PATH; using vtune runsa "
                          << "(may conflict with concurrent perf sessions)." << std::endl;
            }
        }

        const std::string helper_command = make_sleep_command(window_seconds, helper_cpu);
        return run_vtune_collection(
            vtune_binary, selection, 0.0, helper_command,
            build_legacy_vtune_event_config(selection), verbosity);
    }
#endif
    bool process_elevated() {
#if defined(_WIN32)
        return process_is_elevated();
#else
        return true;
#endif
    }

    std::string setup_guidance() {
#if defined(_WIN32)
        const std::string binary = locate_binary();
        if (binary.empty()) return {};          // nothing installed, nothing to advise
        bind_binary(binary);

        if (vtune_reports_no_events(binary)) {
            return "VTune is installed but reports no events for this processor, so it "
                   "cannot\n         profile it. Current VTune supports Ice Lake and newer; "
                   "for an older CPU\n         use the msr/IMC path, or install a VTune from "
                   "before support was dropped.";
        }

        if (sampling_driver_registered()) {
            if (!process_is_elevated()) {
                return "VTune's sampling driver is loaded, but hardware event collection "
                       "needs\n         Administrator. Re-run Mess from an elevated prompt.";
            }

            // A hypervisor gives the same symptom, so this is not asserted as a diagnosis.
            return "VTune's sampling driver is loaded and Mess is elevated, but no counters "
                   "came\n         back. If this CPU is older than Ice Lake, current VTune no "
                   "longer supports\n         it - use the msr/IMC path. Otherwise check whether "
                   "VBS is hiding the counters.";
        }

        const std::string sepreg = sepreg_path(binary);
        if (sepreg.empty()) {
            return "VTune's sampling driver is not registered, and amplxe-sepreg.exe was "
                   "not found\n         beside vtune.exe to install it.";
        }

        std::string out =
            "VTune is installed but its sampling driver is not registered.\n"
            "         Install it from an Administrator prompt:\n"
            "           \"" + sepreg + "\" -i\n"
            "         Then check with -s, or -c to see what is still missing.";
        if (!process_is_elevated()) {
            out += "\n         (amplxe-sepreg requires elevation even to query.)";
        }
        return out;
#else
        return {};
#endif
    }

    bool probe_available(int verbosity) {
#if defined(_WIN32)
        const std::string binary = locate_binary();
        if (binary.empty()) return false;
        bind_binary(binary);

        if (windows_probe_done) return windows_probe_ok;

        if (!process_is_elevated()) {
            windows_probe_done = true;
            windows_probe_ok   = false;
            return false;
        }

        if (vtune_reports_no_events(binary)) {
            if (verbosity >= 2) {
                std::cout << "    VTune reports no events for this processor; "
                             "skipping the counter probe." << '\n';
            }
            windows_probe_done = true;
            windows_probe_ok   = false;
            return false;
        }

        if (!sampling_driver_registered()) {
            if (verbosity >= 2) {
                std::cout << "    VTune is installed but its sampling driver is not "
                             "registered; skipping the counter probe." << '\n';
            }
            windows_probe_done = true;
            windows_probe_ok   = false;
            return false;
        }

        BandwidthCounterSelection probe_selection;
        probe_selection.type              = CounterType::CAS_COUNT;
        probe_selection.perf_available    = true;
        probe_selection.cas.perf_available = true;
        probe_selection.cas.has_read_write = true;
        probe_selection.cas.bytes_per_cas  = 64;

        windows_probe_quiet = true;
        windows_probe_vtune_recipe(binary, probe_selection, verbosity);
        windows_probe_quiet = false;
        return windows_probe_ok;
#else
        (void)verbosity;
        return false;
#endif
    }

    std::string locate_binary() {
#if defined(_WIN32)
        if (const auto found = platform::which("vtune")) {
            return found->string();
        }

        std::error_code ec;
        for (const char* var : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"}) {
            const char* root = std::getenv(var);
            if (!root) continue;

            const std::filesystem::path base = std::filesystem::path(root) / "Intel" / "oneAPI" / "vtune";

            // "latest" is a junction the installer maintains.
            const std::filesystem::path latest = base / "latest" / "bin64" / "vtune.exe";
            if (std::filesystem::exists(latest, ec)) return latest.string();

            if (!std::filesystem::is_directory(base, ec)) continue;

            // Newest by write time, not by name: a name sort puts 2025.4 above 2026.1.
            std::filesystem::path best;
            std::filesystem::file_time_type best_time{};
            for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
                if (ec) break;
                if (!entry.is_directory()) continue;
                const std::filesystem::path candidate = entry.path() / "bin64" / "vtune.exe";
                if (!std::filesystem::exists(candidate, ec)) continue;
                const auto stamp = std::filesystem::last_write_time(candidate, ec);
                if (ec) continue;
                if (best.empty() || stamp > best_time) { best = candidate; best_time = stamp; }
            }
            if (!best.empty()) return best.string();
        }
        return "";
#else
        const std::string output = subprocess::run_shell("command -v vtune 2>/dev/null");
        return extract_command_path_from_output(output, "vtune");
#endif
    }

    void bind_binary(const std::string& binary) {
        if (binary_key == binary) return;
        *this = Impl{};
        binary_key = binary;
    }

};

CollectionSession::CollectionSession() : impl_(std::make_unique<Impl>()) {}
CollectionSession::~CollectionSession() = default;
CollectionSession::CollectionSession(const CollectionSession& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
bool CollectionSession::probe_available(int v) { return impl_->probe_available(v); }
std::string CollectionSession::setup_guidance() { return impl_->setup_guidance(); }
std::string CollectionSession::locate_binary() { return impl_->locate_binary(); }
bool CollectionSession::process_elevated() { return impl_->process_elevated(); }
double CollectionSession::window_seconds(double ms) { return impl_->effective_vtune_window_seconds(ms); }
int CollectionSession::helper_cpu(const system_info& info, const BenchmarkConfig& config, int cores) {
#ifdef _WIN32
    (void)config; (void)cores;
    return impl_->local_socket_helper_cpu(info);
#else
    (void)info;
    return impl_->select_vtune_helper_cpu(config, cores);
#endif
}
VtuneSample CollectionSession::collect(const std::string& binary, const BandwidthCounterSelection& selection,
    double seconds, int cpu, int verbosity) {
    impl_->bind_binary(binary);
#ifdef _WIN32
    std::vector<VtuneSample> samples;
    if (impl_->try_emon_samples(binary, selection, seconds, 1, verbosity, samples)) return samples.front();
    return impl_->run_vtune_collection(binary, selection, 0.0, impl_->make_sleep_command(seconds, cpu), {}, verbosity);
#else
    return impl_->collect_window_sample(binary, selection, seconds, cpu, verbosity);
#endif
}
std::vector<VtuneSample> CollectionSession::collect_batch(const std::string& binary,
    const BandwidthCounterSelection& selection, double seconds, int loops, int cpu, int verbosity) {
    impl_->bind_binary(binary);
#ifdef _WIN32
    std::vector<VtuneSample> samples;
    if (impl_->try_emon_samples(binary, selection, seconds, loops, verbosity, samples)) return samples;
#else
    (void)loops;
#endif
    return {collect(binary, selection, seconds, cpu, verbosity)};
}
VtuneSample CollectionSession::profile(const std::string& binary, const BandwidthCounterSelection& selection,
    const std::string& command, int verbosity) {
    impl_->bind_binary(binary);
    return impl_->run_vtune_collection(binary, selection, 0.0, command, {}, verbosity);
}
namespace {
struct StartupProbe {
    std::mutex mutex;
    CollectionSession session;
};
StartupProbe& startup_probe() { static StartupProbe probe; return probe; }
}
std::unique_ptr<CollectionSession> make_collection_session() {
    auto& probe = startup_probe();
    std::lock_guard<std::mutex> lock(probe.mutex);
    return std::make_unique<CollectionSession>(probe.session);
}
bool probe_available(int verbosity) {
    auto& probe = startup_probe();
    std::lock_guard<std::mutex> lock(probe.mutex);
    return probe.session.probe_available(verbosity);
}
std::string setup_guidance() {
    auto& probe = startup_probe();
    std::lock_guard<std::mutex> lock(probe.mutex);
    return probe.session.setup_guidance();
}
std::string locate_binary() { CollectionSession session; return session.locate_binary(); }
bool process_elevated() { CollectionSession session; return session.process_elevated(); }
}
