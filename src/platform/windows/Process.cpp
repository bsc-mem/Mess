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

#include "platform/Process.h"

#include "WinStrings.h"
#include "WinHandle.h"

#include <windows.h>

#include <cstdlib>
#include <vector>

namespace platform {

namespace {

// cmd /c strips the outer quote pair, splitting a quoted exe path at its first space. The
// extra pair gives cmd something to consume.
std::string cmd_shell_safe(const char* cmd) {
    if (!cmd) return std::string();
    if (cmd[0] != '"') return std::string(cmd);
    return "\"" + std::string(cmd) + "\"";
}

}

bool run_command_success(const char* cmd) {
    const std::string safe = cmd_shell_safe(cmd);
    const int ret = std::system(safe.c_str());
    if (ret == -1) {
        return false;
    }
    return ret == 0;
}

bool run_command_success(const std::string& cmd) {
    return run_command_success(cmd.c_str());
}

bool close_pipe_success(std::FILE* stream) {
    if (!stream) return false;
    const int ret = _pclose(stream);
    if (ret == -1) return false;
    return ret == 0;
}

std::FILE* open_pipe(const char* cmd, const char* mode) {
    const std::string safe = cmd_shell_safe(cmd);
    return _popen(safe.c_str(), mode);
}

int close_pipe(std::FILE* stream) {
    if (!stream) return -1;
    return _pclose(stream);
}

// No wait-status packing: the value is the exit code.
int exit_code_of(int status) {
    return status;
}

bool set_env(const char* name, const char* value) {
    return _putenv_s(name, value) == 0;
}

bool set_env(const std::string& name, const std::string& value) {
    return set_env(name.c_str(), value.c_str());
}

// _putenv_s(name, "") is the documented deletion form.
bool unset_env(const char* name) {
    return _putenv_s(name, "") == 0;
}

bool unset_env(const std::string& name) {
    return unset_env(name.c_str());
}

// A handle, not a pid: the id cannot be recycled while a handle to it lives.
struct Process::Impl {
    HANDLE handle = nullptr;
    HANDLE job = nullptr;
    DWORD  id = 0;
    ~Impl() {
        if (job) { TerminateJobObject(job, 1); CloseHandle(job); }
        if (handle) { WaitForSingleObject(handle, 2000); CloseHandle(handle); }
    }
};

Process::~Process() {
    if (p_) {
        delete p_;
    }
}

Process::Process(Process&& other) noexcept : p_(other.p_) {
    other.p_ = nullptr;
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        if (p_) {
            delete p_;
        }
        p_ = other.p_;
        other.p_ = nullptr;
    }
    return *this;
}

ProcessId Process::native_id() const {
    return p_ ? static_cast<ProcessId>(p_->id) : -1;
}

bool Process::running() {
    if (!p_ || !p_->handle) return false;
    return WaitForSingleObject(p_->handle, 0) == WAIT_TIMEOUT;
}

bool Process::wait(std::chrono::milliseconds timeout, int& exit_code) {
    if (!p_ || !p_->handle) return false;

    const DWORD ms = (timeout.count() < 0)
        ? INFINITE
        : static_cast<DWORD>(timeout.count());
    if (WaitForSingleObject(p_->handle, ms) != WAIT_OBJECT_0) {
        return false;
    }

    DWORD code = 0;
    exit_code = GetExitCodeProcess(p_->handle, &code) ? static_cast<int>(code) : -1;
    return true;
}

// No SIGTERM, so Graceful and Force both end at TerminateProcess.
void Process::terminate(Stop) {
    if (!p_ || !p_->handle) return;
    TerminateJobObject(p_->job, 1);
}

std::optional<Process> spawn_shell(const std::string& command) {
    std::wstring command_line = L"cmd.exe /D /S /C \"" + win::widen(command) + L"\"";

    wchar_t comspec[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    const bool have_comspec = (n > 0 && n < MAX_PATH);

    std::string error;
    auto job = win::make_job(error);
    if (!job) return std::nullopt;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(have_comspec ? comspec : nullptr,
                        command_line.data(),
                        nullptr, nullptr,
                        FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &si, &pi)) {
        return std::nullopt;
    }
    win::Handle thread(pi.hThread), process(pi.hProcess);
    if (!AssignProcessToJobObject(job.get(), pi.hProcess) ||
        ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        return std::nullopt;
    }

    Process proc;
    proc.p_ = new Process::Impl();
    proc.p_->handle = process.release();
    proc.p_->job = job.release();
    proc.p_->id = pi.dwProcessId;
    return proc;
}

namespace {

// Pin the suspended thread before the child's first touch.
bool pin_suspended(HANDLE process, HANDLE thread, int cpu) {
    if (cpu < 0) return true;
    if (cpu / 64 >= GetActiveProcessorGroupCount()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    GROUP_AFFINITY ga{};
    ga.Group = static_cast<WORD>(cpu / 64);
    ga.Mask  = static_cast<KAFFINITY>(1) << (cpu % 64);

    if (GetActiveProcessorGroupCount() == 1) {
        if (!SetProcessAffinityMask(process, static_cast<DWORD_PTR>(ga.Mask))) return false;
    }
    return SetThreadGroupAffinity(thread, &ga, nullptr) != 0;
}

// CPU affinity precedes first touch; explicit memory placement is a separate capability.

}

struct ProcessGroup::Impl {
    HANDLE job = nullptr;
    std::string error;
    std::vector<HANDLE> handles;
    std::vector<ProcessId> ids;
};

ProcessGroup::ProcessGroup() : p_(new Impl()) {
    p_->job = win::make_job(p_->error).release();
}

ProcessGroup::~ProcessGroup() {
    if (!p_) return;
    terminate_all();
    if (p_->job) CloseHandle(p_->job);
    delete p_;
}

ProcessGroup::ProcessGroup(ProcessGroup&& other) noexcept : p_(other.p_) {
    other.p_ = nullptr;
}

ProcessGroup& ProcessGroup::operator=(ProcessGroup&& other) noexcept {
    if (this != &other) {
        if (p_) {
            terminate_all();
            if (p_->job) CloseHandle(p_->job);
            delete p_;
        }
        p_ = other.p_;
        other.p_ = nullptr;
    }
    return *this;
}

bool ProcessGroup::valid() const {
    return p_ != nullptr && p_->job != nullptr;
}

std::string ProcessGroup::error() const {
    return p_ ? p_->error : "invalid process group";
}

ProcessId ProcessGroup::spawn(const std::string& program,
                              const std::vector<std::string>& args,
                              const SpawnOptions& opts) {
    if (!valid()) return 0;
    p_->error.clear();
    if (opts.pin.numa_node >= 0) {
        p_->error = "explicit NUMA memory placement is unsupported";
        return 0;
    }

    // Allocate ownership bookkeeping before a child can start executing.
    p_->handles.reserve(p_->handles.size() + 1);
    p_->ids.reserve(p_->ids.size() + 1);
    const std::wstring wide_program = win::widen(program);

    std::wstring command_line;
    win::append_quoted(command_line, wide_program);
    for (const std::string& a : args) {
        command_line += L' ';
        win::append_quoted(command_line, win::widen(a));
    }

    // FILE_SHARE_* and FILE_APPEND_DATA so several workers can share one log.
    HANDLE log = INVALID_HANDLE_VALUE;
    if (!opts.log_path.empty()) {
        log = CreateFileW(win::widen(opts.log_path).c_str(),
                          FILE_APPEND_DATA | SYNCHRONIZE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            p_->error = win::last_error("open worker log");
            return 0;
        }
        if (!SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            p_->error = win::last_error("SetHandleInformation");
            CloseHandle(log);
            return 0;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    win::RedirectedStartup redirected;
    if (log != INVALID_HANDLE_VALUE && !redirected.initialize(log, p_->error)) {
        CloseHandle(log);
        return 0;
    }
    const std::wstring cwd = opts.working_dir.empty()
        ? std::wstring()
        : win::widen(opts.working_dir);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(wide_program.c_str(),
                                   command_line.data(),
                                   nullptr, nullptr,
                                   log != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                                   CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                       (log != INVALID_HANDLE_VALUE ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                   nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(),
                                   log != INVALID_HANDLE_VALUE ? &redirected.info.StartupInfo : &si, &pi);

    const DWORD spawn_error = ok ? ERROR_SUCCESS : GetLastError();
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) { p_->error = win::last_error("CreateProcessW", spawn_error); return 0; }

    win::Handle process(pi.hProcess);
    win::Handle thread(pi.hThread);
    const char* failed = nullptr;
    if (!pin_suspended(pi.hProcess, pi.hThread, opts.pin.cpu)) failed = "CPU affinity";
    else if (!AssignProcessToJobObject(p_->job, pi.hProcess)) failed = "AssignProcessToJobObject";
    else if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) failed = "ResumeThread";
    if (failed) {
        p_->error = win::last_error(failed);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        return 0;
    }
    process.release();

    p_->handles.push_back(pi.hProcess);
    p_->ids.push_back(static_cast<ProcessId>(pi.dwProcessId));
    return static_cast<ProcessId>(pi.dwProcessId);
}

std::vector<ProcessId> ProcessGroup::members() const {
    return p_ ? p_->ids : std::vector<ProcessId>{};
}

int ProcessGroup::alive() const {
    if (!p_) return 0;
    int n = 0;
    for (HANDLE h : p_->handles) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) ++n;
    }
    return n;
}

void ProcessGroup::terminate_all() {
    if (!p_) return;
    if (!p_->job || !TerminateJobObject(p_->job, 1)) {
        for (HANDLE h : p_->handles) TerminateProcess(h, 1);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (HANDLE h : p_->handles) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        WaitForSingleObject(h, remaining > 0 ? static_cast<DWORD>(remaining) : 0);
        CloseHandle(h);
    }
    p_->handles.clear();
    p_->ids.clear();
}

namespace {

// OpenProcess may succeed after exit, so WaitForSingleObject decides liveness.
HANDLE open_for(ProcessId pid, DWORD access) {
    if (pid <= 0) return nullptr;
    return OpenProcess(access, FALSE, static_cast<DWORD>(pid));
}

}

bool process_exists(ProcessId pid) {
    const HANDLE h = open_for(pid, SYNCHRONIZE);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

bool process_running(ProcessId pid) {
    return process_exists(pid);
}

bool signal_process(ProcessId pid, Stop) {
    const HANDLE h = open_for(pid, PROCESS_TERMINATE);
    if (!h) return false;
    const BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != 0;
}

void reap_process(ProcessId) {
}

bool wait_for_process_exit(ProcessId pid, std::chrono::milliseconds timeout) {
    if (pid <= 0) return true;

    const HANDLE h = open_for(pid, SYNCHRONIZE);
    if (!h) return true;   // gone, or not ours to observe - same answer as ESRCH

    const DWORD ms = (timeout.count() < 0)
        ? INFINITE
        : static_cast<DWORD>(timeout.count());
    const bool exited = WaitForSingleObject(h, ms) != WAIT_TIMEOUT;
    CloseHandle(h);
    return exited;
}

}
