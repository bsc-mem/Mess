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

#include "platform/Process.h"
#include "WinStrings.h"
#include "WinHandle.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

namespace platform {
namespace {

RunResult spawn_and_capture(const std::wstring& application,
                            std::wstring command_line,
                            const RunOptions& opts) {
    RunResult result;
    auto fail = [&](const char* operation) {
        result.spawn_failed = true;
        result.error = win::last_error(operation);
        return result;
    };
    win::Handle job = win::make_job(result.error);
    if (!job) { result.spawn_failed = true; return result; }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &sa, 1 << 16)) return fail("CreatePipe");
    win::Handle read_end(read_raw), write_end(write_raw);
    if (!SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0))
        return fail("SetHandleInformation");
    win::RedirectedStartup startup;
    if (!startup.initialize(write_end.get(), result.error)) {
        result.spawn_failed = true;
        return result;
    }
    const auto cwd = win::widen(opts.working_dir);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(application.empty() ? nullptr : application.c_str(),
                        command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup.info.StartupInfo, &pi))
        return fail("CreateProcessW");
    win::Handle process(pi.hProcess), thread(pi.hThread);
    write_end.reset();
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        result = fail("AssignProcessToJobObject");
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 2000);
        return result;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) return fail("ResumeThread");
    thread.reset();

    const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
    bool pipe_open = true;
    // One reader, no concurrent ReadFile: peek and read only buffered bytes. Check
    // the same deadline between chunks even when output is continuous or capped.
    for (;;) {
        const DWORD state = WaitForSingleObject(process.get(), 0);
        if (state == WAIT_FAILED) { result.error = win::last_error("process wait"); break; }
        DWORD available = 0;
        if (pipe_open && !PeekNamedPipe(read_end.get(), nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                result.error = win::last_error("PeekNamedPipe", error);
                break;
            }
            pipe_open = false;
        }
        if (state == WAIT_OBJECT_0 && available == 0) {
            DWORD code = 0;
            if (GetExitCodeProcess(process.get(), &code)) result.exit_status = static_cast<int>(code);
            else result.error = win::last_error("GetExitCodeProcess");
            break;
        }
        if (opts.timeout.count() >= 0 && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        if (available > 0) {
            char chunk[4096];
            DWORD read = 0;
            if (!ReadFile(read_end.get(), chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr)) {
                result.error = win::last_error("ReadFile");
                break;
            }
            const auto room = opts.max_output_bytes - result.output.size();
            result.output.append(chunk, std::min<std::size_t>(read, room));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // This call owns the process tree, including descendants retaining stdout.
    TerminateJobObject(job.get(), 1);
    WaitForSingleObject(process.get(), 2000);
    return result;
}

} // namespace

RunResult run(const std::string& program, const std::vector<std::string>& args,
              const RunOptions& opts) {
    std::wstring command_line;
    win::append_quoted(command_line, win::widen(program));
    for (const auto& arg : args) {
        command_line += L' ';
        win::append_quoted(command_line, win::widen(arg));
    }
    return spawn_and_capture({}, std::move(command_line), opts);
}

RunResult run_shell(const std::string& command, const RunOptions& opts) {
    std::wstring command_line = L"cmd.exe /D /S /C \"" + win::widen(command) + L"\"";
    wchar_t comspec[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    return spawn_and_capture(n > 0 && n < MAX_PATH ? comspec : L"", std::move(command_line), opts);
}

struct Coprocess::Impl {
    win::Handle job;
    win::Handle process;
    win::Handle input;
    win::Handle output;
    std::string buffer;
    bool output_open = true;

    ~Impl() { stop(5000); }

    void stop(DWORD grace_ms) {
        input.reset();
        if (process) WaitForSingleObject(process.get(), grace_ms);
        if (job) TerminateJobObject(job.get(), 1);
        if (process) WaitForSingleObject(process.get(), 2000);
        process.reset();
        job.reset();
        output.reset();
        output_open = false;
    }
};

std::optional<Coprocess> Coprocess::spawn(const std::string& program, const std::vector<std::string>& args, const std::string& working_dir, std::string& error) {
    std::wstring command_line;
    win::append_quoted(command_line, win::widen(program));
    for (const auto& arg : args) {
        command_line += L' ';
        win::append_quoted(command_line, win::widen(arg));
    }
    auto impl = std::make_unique<Impl>();
    impl->job = win::make_job(error);
    if (!impl->job) return std::nullopt;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, in_read = nullptr, in_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 1 << 16)) { error = win::last_error("CreatePipe"); return std::nullopt; }
    impl->output.reset(out_read);
    win::Handle child_output(out_write);
    if (!CreatePipe(&in_read, &in_write, &sa, 1 << 16)) { error = win::last_error("CreatePipe"); return std::nullopt; }
    win::Handle child_input(in_read);
    impl->input.reset(in_write);
    if (!SetHandleInformation(impl->output.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(impl->input.get(), HANDLE_FLAG_INHERIT, 0)) {
        error = win::last_error("SetHandleInformation");
        return std::nullopt;
    }
    win::RedirectedStartup startup;
    if (!startup.initialize(child_output.get(), error, child_input.get())) return std::nullopt;
    const auto cwd = win::widen(working_dir);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup.info.StartupInfo, &pi)) {
        error = win::last_error("CreateProcessW");
        return std::nullopt;
    }
    impl->process.reset(pi.hProcess);
    win::Handle thread(pi.hThread);
    child_output.reset();
    child_input.reset();
    if (!AssignProcessToJobObject(impl->job.get(), impl->process.get())) {
        error = win::last_error("AssignProcessToJobObject");
        TerminateProcess(impl->process.get(), 1);
        return std::nullopt;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        error = win::last_error("ResumeThread");
        return std::nullopt;
    }
    Coprocess child;
    child.p_ = impl.release();
    return child;
}

Coprocess::~Coprocess() { delete p_; }

Coprocess::Coprocess(Coprocess&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}

Coprocess& Coprocess::operator=(Coprocess&& other) noexcept {
    if (this != &other) {
        delete p_;
        p_ = std::exchange(other.p_, nullptr);
    }
    return *this;
}

bool Coprocess::write_line(const std::string& line) {
    if (!p_ || !p_->input) return false;
    const std::string data = line + '\n';
    for (size_t offset = 0; offset < data.size();) {
        DWORD written = 0;
        if (!WriteFile(p_->input.get(), data.data() + offset, static_cast<DWORD>(data.size() - offset),
                       &written, nullptr))
            return false;
        offset += written;
    }
    return true;
}

ReceiveStatus Coprocess::read_line(std::string& line, std::chrono::milliseconds timeout) {
    if (!p_) return ReceiveStatus::Error;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto newline = p_->buffer.find('\n');
        if (newline != std::string::npos) {
            line = p_->buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            p_->buffer.erase(0, newline + 1);
            return ReceiveStatus::Data;
        }
        if (!p_->output_open) {
            if (p_->buffer.empty()) return ReceiveStatus::Eof;
            line = std::move(p_->buffer);
            p_->buffer.clear();
            return ReceiveStatus::Data;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(p_->output.get(), nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) return ReceiveStatus::Error;
            p_->output_open = false;
            continue;
        }
        if (available > 0) {
            char chunk[4096];
            DWORD read = 0;
            if (!ReadFile(p_->output.get(), chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr))
                return ReceiveStatus::Error;
            p_->buffer.append(chunk, read);
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) return ReceiveStatus::Timeout;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Coprocess::terminate() {
    if (p_) p_->stop(0);
}

} // namespace platform
