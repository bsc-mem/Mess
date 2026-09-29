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
#include <cstring>
#include <set>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

namespace platform {
namespace {
void reap_until(pid_t pid, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto result = waitpid(pid, nullptr, WNOHANG);
        if (result == pid || (result < 0 && errno != EINTR)) return;
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace


bool run_command_success(const char* cmd) {
    int ret = std::system(cmd);
    if (ret == -1) {
        return false;
    }
    return WIFEXITED(ret) && WEXITSTATUS(ret) == 0;
}

bool run_command_success(const std::string& cmd) {
    return run_command_success(cmd.c_str());
}

bool close_pipe_success(std::FILE* stream) {
    if (!stream) return false;
    int ret = pclose(stream);
    if (ret == -1) return false;
    return WIFEXITED(ret) && WEXITSTATUS(ret) == 0;
}

std::FILE* open_pipe(const char* cmd, const char* mode) {
    return popen(cmd, mode);
}

int close_pipe(std::FILE* stream) {
    if (!stream) return -1;
    return pclose(stream);
}

int exit_code_of(int status) {
    if (status == -1) return -1;
    return (WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool set_env(const char* name, const char* value) {
    return setenv(name, value, 1) == 0;
}

bool set_env(const std::string& name, const std::string& value) {
    return set_env(name.c_str(), value.c_str());
}

bool unset_env(const char* name) {
    return unsetenv(name) == 0;
}

bool unset_env(const std::string& name) {
    return unset_env(name.c_str());
}

// A pid must be waited on exactly once, so the status is cached when it is collected.
struct Process::Impl {
    pid_t pid = -1;
    bool  reaped = false;
    int   exit_code = -1;

    bool poll_once(int flags) {
        if (reaped) return true;
        int status = 0;
        pid_t r;
        do { r = waitpid(pid, &status, flags); } while (r < 0 && errno == EINTR);
        if (r == pid) {
            reaped = true;
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return true;
        }
        if (r == -1) {
            reaped = true;
            exit_code = -1;
            return true;
        }
        return false;
    }
    ~Impl() {
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            if (!reaped) {
                ::kill(pid, SIGKILL);
                reap_until(pid, std::chrono::steady_clock::now() + std::chrono::seconds(2));
            }
        }
    }

};

Process::~Process() {
    delete p_;
}

Process::Process(Process&& other) noexcept : p_(other.p_) {
    other.p_ = nullptr;
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        delete p_;
        p_ = other.p_;
        other.p_ = nullptr;
    }
    return *this;
}

ProcessId Process::native_id() const {
    return p_ ? static_cast<ProcessId>(p_->pid) : -1;
}

bool Process::running() {
    if (!p_) return false;
    return !p_->poll_once(WNOHANG);
}

bool Process::wait(std::chrono::milliseconds timeout, int& exit_code) {
    if (!p_) return false;

    if (timeout.count() < 0) {
        p_->poll_once(0);
        exit_code = p_->exit_code;
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (p_->poll_once(WNOHANG)) {
            exit_code = p_->exit_code;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Process::terminate(Stop mode) {
    if (!p_ || p_->reaped) return;
    ::kill(-p_->pid, (mode == Stop::Force) ? SIGKILL : SIGTERM);
}

std::optional<Process> spawn_shell(const std::string& command) {
    const pid_t pid = fork();
    if (pid < 0) {
        return std::nullopt;
    }
    if (pid == 0) {
        if (setpgid(0, 0) < 0) _exit(127);
        execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    setpgid(pid, pid);
    Process proc;
    proc.p_ = new Process::Impl();
    proc.p_->pid = pid;
    return proc;
}

struct ProcessGroup::Impl {
    pid_t leader = 0;
    std::string error;
    std::vector<ProcessId> ids;
    std::set<ProcessId> exited;
};

ProcessGroup::ProcessGroup() : p_(new Impl()) {}

ProcessGroup::~ProcessGroup() {
    terminate_all();
    delete p_;
}

ProcessGroup::ProcessGroup(ProcessGroup&& other) noexcept : p_(other.p_) {
    other.p_ = nullptr;
}

ProcessGroup& ProcessGroup::operator=(ProcessGroup&& other) noexcept {
    if (this != &other) {
        terminate_all();
        delete p_;
        p_ = other.p_;
        other.p_ = nullptr;
    }
    return *this;
}

bool ProcessGroup::valid() const {
    return p_ != nullptr;
}

std::string ProcessGroup::error() const {
    return p_ ? p_->error : "invalid process group";
}

ProcessId ProcessGroup::spawn(const std::string& program,
                              const std::vector<std::string>& args,
                              const SpawnOptions& opts) {
    if (!p_) return 0;
    p_->error.clear();
    if (opts.pin.numa_node >= 0) {
        p_->error = "explicit NUMA memory placement is unsupported by ProcessGroup";
        return 0;
    }
#ifndef __linux__
    if (opts.pin.cpu >= 0) {
        p_->error = "CPU affinity is unsupported by this ProcessGroup backend";
        return 0;
    }
#else
    if (opts.pin.cpu >= CPU_SETSIZE) {
        p_->error = "CPU index exceeds CPU_SETSIZE";
        return 0;
    }
#endif
    p_->ids.reserve(p_->ids.size() + 1);
    int errors[2];
    if (pipe(errors) < 0) { p_->error = std::strerror(errno); return 0; }
    if (fcntl(errors[1], F_SETFD, FD_CLOEXEC) < 0) {
        p_->error = std::strerror(errno);
        close(errors[0]); close(errors[1]);
        return 0;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        p_->error = std::strerror(errno);
        close(errors[0]); close(errors[1]);
        return 0;
    }
    if (pid == 0) {
        close(errors[0]);
        auto fail = [&]() {
            const int error = errno;
            ssize_t n;
            do { n = write(errors[1], &error, sizeof(error)); } while (n < 0 && errno == EINTR);
            _exit(127);
        };
        if (setpgid(0, p_->leader) < 0) fail();
        if (!opts.working_dir.empty() && chdir(opts.working_dir.c_str()) < 0) fail();
        if (!opts.log_path.empty()) {
            const int fd = open(opts.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) fail();
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) fail();
            close(fd);
        }
#ifdef __linux__
        if (opts.pin.cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(opts.pin.cpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) < 0) fail();
        }
#endif
        execvp(program.c_str(), argv.data());
        fail();
    }
    close(errors[1]);
    setpgid(pid, p_->leader);
    int error = 0;
    ssize_t n;
    do { n = read(errors[0], &error, sizeof(error)); } while (n < 0 && errno == EINTR);
    close(errors[0]);
    if (n != 0) {
        p_->error = std::string("spawn: ") + std::strerror(n > 0 ? error : errno);
        kill(pid, SIGKILL);
        reap_until(pid, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        return 0;
    }
    if (p_->leader == 0) p_->leader = pid;
    p_->ids.push_back(pid);
    return pid;
}

std::vector<ProcessId> ProcessGroup::members() const {
    return p_ ? p_->ids : std::vector<ProcessId>{};
}

int ProcessGroup::alive() const {
    if (!p_) return 0;
    int n = 0;
    for (ProcessId id : p_->ids) {
        if (p_->exited.count(id)) continue;
        int status = 0;
        const auto result = waitpid(static_cast<pid_t>(id), &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) ++n;
        else p_->exited.insert(id);
    }
    return n;
}

void ProcessGroup::terminate_all() {
    if (!p_) return;
    if (p_->leader > 0) {
        ::kill(-p_->leader, SIGKILL);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (ProcessId id : p_->ids) reap_until(static_cast<pid_t>(id), deadline);
    p_->ids.clear();
    p_->exited.clear();
    p_->leader = 0;
}


bool process_exists(ProcessId pid) {
    if (pid <= 0) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

bool process_running(ProcessId pid) {
    if (pid <= 0) return false;

    int status;
    int result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (result == static_cast<pid_t>(pid)) {
        return false;
    }

    if (result == 0) {
        return true;
    }

    return (::kill(static_cast<pid_t>(pid), 0) == 0);
}

bool signal_process(ProcessId pid, Stop mode) {
    if (pid <= 0) return false;
    const int sig = (mode == Stop::Force) ? SIGKILL : SIGTERM;
    return ::kill(static_cast<pid_t>(pid), sig) == 0;
}

void reap_process(ProcessId pid) {
    if (pid <= 0) return;
    ::waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
}

bool wait_for_process_exit(ProcessId pid, std::chrono::milliseconds timeout) {
    if (pid <= 0) return true;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}
