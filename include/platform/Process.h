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

#ifndef PLATFORM_PROCESS_H
#define PLATFORM_PROCESS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "platform/Ipc.h"

namespace platform {

using ProcessId = std::int64_t;

enum class Stop {
    Graceful,
    Force
};

bool run_command_success(const char* cmd);
bool run_command_success(const std::string& cmd);
bool close_pipe_success(std::FILE* stream);
std::FILE* open_pipe(const char* cmd, const char* mode);
int close_pipe(std::FILE* stream);
int exit_code_of(int status);
bool set_env(const char* name, const char* value);
bool set_env(const std::string& name, const std::string& value);
bool unset_env(const char* name);
bool unset_env(const std::string& name);

// run() owns its process tree. Return closes descendants as well as capture.
struct RunOptions {
    std::chrono::milliseconds timeout{std::chrono::seconds(10)};
    std::size_t max_output_bytes = 65536;
    std::string working_dir;
};

struct RunResult {
    std::string output;
    int  exit_status  = -1;
    bool timed_out    = false;
    bool spawn_failed = false;
    std::string error; // Failed operation and native error, when available.
};

RunResult run(const std::string& program,
              const std::vector<std::string>& args,
              const RunOptions& opts = {});

RunResult run_shell(const std::string& command, const RunOptions& opts = {});

// Line-oriented child with merged stdout/stderr; owns its descendants.
class Coprocess {
public:
    static std::optional<Coprocess> spawn(const std::string& program, const std::vector<std::string>& args, const std::string& working_dir, std::string& error);

    Coprocess() = default;
    ~Coprocess();
    Coprocess(Coprocess&& other) noexcept;
    Coprocess& operator=(Coprocess&& other) noexcept;
    Coprocess(const Coprocess&) = delete;
    Coprocess& operator=(const Coprocess&) = delete;

    bool write_line(const std::string& line);
    ReceiveStatus read_line(std::string& line, std::chrono::milliseconds timeout);
    void terminate();

private:
    struct Impl;
    Impl* p_ = nullptr;
};

// Owns a shell command and its descendants; destruction terminates unfinished work.
class Process {
public:
    Process() = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool valid() const { return p_ != nullptr; }

    ProcessId native_id() const;
    bool running();
    bool wait(std::chrono::milliseconds timeout, int& exit_code);
    void terminate(Stop mode);

private:
    struct Impl;
    Impl* p_ = nullptr;

    friend std::optional<Process> spawn_shell(const std::string& command);
};

std::optional<Process> spawn_shell(const std::string& command);

struct PinSpec {
    int cpu = -1;
    // Explicit memory placement. Unsupported backends must reject, never ignore it.
    int numa_node = -1;
};

struct SpawnOptions {
    std::string working_dir;
    std::string log_path;
    PinSpec     pin;
};

class ProcessGroup {
public:
    ProcessGroup();
    ~ProcessGroup();
    ProcessGroup(ProcessGroup&& other) noexcept;
    ProcessGroup& operator=(ProcessGroup&& other) noexcept;
    ProcessGroup(const ProcessGroup&) = delete;
    ProcessGroup& operator=(const ProcessGroup&) = delete;

    bool valid() const;
    std::string error() const;
    ProcessId spawn(const std::string& program,
                    const std::vector<std::string>& args,
                    const SpawnOptions& opts);

    // Owns all successfully launched workers; destruction terminates the group.
    std::vector<ProcessId> members() const;
    int alive() const;
    void terminate_all();

private:
    struct Impl;
    Impl* p_ = nullptr;
};

bool process_exists(ProcessId pid);
bool process_running(ProcessId pid);
bool signal_process(ProcessId pid, Stop mode);
void reap_process(ProcessId pid);
bool wait_for_process_exit(ProcessId pid, std::chrono::milliseconds timeout);

}

#endif // PLATFORM_PROCESS_H
