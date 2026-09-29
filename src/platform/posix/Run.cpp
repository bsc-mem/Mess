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

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace platform {

RunResult run(const std::string& program, const std::vector<std::string>& args,
              const RunOptions& opts) {
    RunResult result;
    auto fail = [&](const char* operation) {
        result.spawn_failed = true;
        result.error = std::string(operation) + ": " + std::strerror(errno);
        return result;
    };
    int output[2], errors[2];
    if (pipe(output) < 0) return fail("pipe");
    if (pipe(errors) < 0) {
        result = fail("exec error pipe");
        close(output[0]); close(output[1]);
        return result;
    }
    if (fcntl(errors[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(output[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(errors[0], F_SETFL, O_NONBLOCK) < 0) {
        result = fail("fcntl");
        for (int fd : {output[0], output[1], errors[0], errors[1]}) close(fd);
        return result;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        result = fail("fork");
        for (int fd : {output[0], output[1], errors[0], errors[1]}) close(fd);
        return result;
    }
    if (pid == 0) {
        close(output[0]); close(errors[0]);
        auto child_fail = [&]() {
            const int error = errno;
            ssize_t written;
            do { written = write(errors[1], &error, sizeof(error)); } while (written < 0 && errno == EINTR);
            _exit(127);
        };
        if (setpgid(0, 0) < 0) child_fail();
        if (!opts.working_dir.empty() && chdir(opts.working_dir.c_str()) < 0) child_fail();
        if (dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0) child_fail();
        close(output[1]);
        execvp(program.c_str(), argv.data());
        child_fail();
    }
    close(output[1]); close(errors[1]);
    // Child also establishes the group before exec; the parent closes the race.
    setpgid(pid, pid);
    const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
    bool exited = false, eof = false;
    int status = 0;
    for (;;) {
        if (!exited) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) exited = true;
            else if (waited < 0 && errno != EINTR) {
                result.error = std::string("waitpid: ") + std::strerror(errno);
                break;
            }
        }
        char chunk[4096];
        const ssize_t n = eof ? 0 : read(output[0], chunk, sizeof(chunk));
        if (n > 0) {
            result.output.append(chunk, std::min<std::size_t>(n, opts.max_output_bytes - result.output.size()));
        } else if (n == 0) {
            eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            result.error = std::string("read: ") + std::strerror(errno);
            break;
        }
        if (exited && n <= 0) {
            result.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (opts.timeout.count() >= 0 && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        if (n <= 0) {
            pollfd pfd{output[0], POLLIN, 0};
            // EOF does not imply process exit; avoid spinning on POLLHUP.
            poll(eof ? nullptr : &pfd, eof ? 0 : 1, 5);
        }
    }
    // Scope includes descendants. Never wait for an inherited stdout to close.
    kill(-pid, SIGKILL);
    if (!exited) {
        kill(pid, SIGKILL);
        const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno != EINTR)) break;
            if (std::chrono::steady_clock::now() >= cleanup_deadline) {
                result.error = "child did not exit after SIGKILL";
                break;
            }
            poll(nullptr, 0, 5);
        }
    }
    close(output[0]);
    int exec_error = 0;
    ssize_t n;
    do { n = read(errors[0], &exec_error, sizeof(exec_error)); } while (n < 0 && errno == EINTR);
    close(errors[0]);
    if (n == sizeof(exec_error)) {
        result.spawn_failed = true;
        result.exit_status = -1;
        result.error = std::string("spawn: ") + std::strerror(exec_error);
    }
    return result;
}

RunResult run_shell(const std::string& command, const RunOptions& opts) {
    return run("/bin/sh", {"-lc", command}, opts);
}

struct Coprocess::Impl {
    pid_t pid = -1;
    int input = -1;
    int output = -1;
    std::string buffer;

    ~Impl() { stop(std::chrono::seconds(5)); }

    void stop(std::chrono::milliseconds grace) {
        if (input >= 0) { close(input); input = -1; }
        if (pid > 0) {
            int status = 0;
            const auto deadline = std::chrono::steady_clock::now() + grace;
            pid_t waited;
            while ((waited = waitpid(pid, &status, WNOHANG)) == 0 &&
                   std::chrono::steady_clock::now() < deadline)
                poll(nullptr, 0, 5);
            kill(-pid, SIGKILL);
            if (waited == 0) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            pid = -1;
        }
        if (output >= 0) { close(output); output = -1; }
    }
};

std::optional<Coprocess> Coprocess::spawn(const std::string& program, const std::vector<std::string>& args, const std::string& working_dir, std::string& error) {
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1}, exec_errors[2] = {-1, -1};
    auto close_all = [&] {
        for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1], exec_errors[0], exec_errors[1]})
            if (fd >= 0) close(fd);
    };
    auto fail = [&](const char* operation) {
        error = std::string(operation) + ": " + std::strerror(errno);
        close_all();
        return std::nullopt;
    };
    if (pipe(to_child) < 0 || pipe(from_child) < 0 || pipe(exec_errors) < 0) return fail("pipe");
    for (int fd : {to_child[1], from_child[0], exec_errors[0], exec_errors[1]})
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return fail("fcntl");
    if (fcntl(from_child[0], F_SETFL, O_NONBLOCK) < 0) return fail("fcntl");

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        auto child_fail = [&]() {
            const int code = errno;
            ssize_t written;
            do { written = write(exec_errors[1], &code, sizeof(code)); } while (written < 0 && errno == EINTR);
            _exit(127);
        };
        if (setpgid(0, 0) < 0) child_fail();
        if (!working_dir.empty() && chdir(working_dir.c_str()) < 0) child_fail();
        if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0 ||
            dup2(from_child[1], STDERR_FILENO) < 0) child_fail();
        close(to_child[0]);
        close(from_child[1]);
        execvp(program.c_str(), argv.data());
        child_fail();
    }
    setpgid(pid, pid);
    close(to_child[0]); to_child[0] = -1;
    close(from_child[1]); from_child[1] = -1;
    close(exec_errors[1]); exec_errors[1] = -1;

    Coprocess child;
    child.p_ = new Impl();
    child.p_->pid = pid;
    child.p_->input = to_child[1]; to_child[1] = -1;
    child.p_->output = from_child[0]; from_child[0] = -1;

    int exec_error = 0;
    ssize_t n;
    do { n = read(exec_errors[0], &exec_error, sizeof(exec_error)); } while (n < 0 && errno == EINTR);
    close(exec_errors[0]);
    if (n == sizeof(exec_error)) {
        error = std::string("spawn: ") + std::strerror(exec_error);
        return std::nullopt;
    }
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
    if (!p_ || p_->input < 0) return false;
    const std::string data = line + '\n';
    sigset_t pipe_signal, previous;
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);
    bool written = true;
    for (size_t offset = 0; offset < data.size();) {
        const ssize_t n = write(p_->input, data.data() + offset, data.size() - offset);
        if (n > 0) offset += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else { written = false; break; }
    }
    if (!written) {
        sigset_t pending;
        sigpending(&pending);
        int signal_number = 0;
        if (sigismember(&pending, SIGPIPE)) sigwait(&pipe_signal, &signal_number);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return written;
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
        if (p_->output < 0) {
            if (p_->buffer.empty()) return ReceiveStatus::Eof;
            line = std::move(p_->buffer);
            p_->buffer.clear();
            return ReceiveStatus::Data;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return ReceiveStatus::Timeout;
        pollfd pfd{p_->output, POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(std::min<long long>(remaining.count(), INT_MAX)));
        if (ready < 0 && errno != EINTR) return ReceiveStatus::Error;
        if (ready <= 0) continue;
        char chunk[4096];
        const ssize_t n = read(p_->output, chunk, sizeof(chunk));
        if (n > 0) p_->buffer.append(chunk, static_cast<size_t>(n));
        else if (n == 0) { close(p_->output); p_->output = -1; }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return ReceiveStatus::Error;
    }
}

void Coprocess::terminate() {
    if (p_) p_->stop(std::chrono::milliseconds(0));
}

} // namespace platform
