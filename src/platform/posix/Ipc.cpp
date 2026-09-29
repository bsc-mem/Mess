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

#include "platform/Ipc.h"

#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <semaphore.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

namespace platform {

FileLock FileLock::acquire(const std::filesystem::path& path) {
    FileLock lock;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        return lock;
    }

    if (::flock(fd, LOCK_EX) == -1) {
        ::close(fd);
        return lock;
    }

    lock.handle_ = static_cast<std::intptr_t>(fd);
    return lock;
}

FileLock::~FileLock() {
    release();
}

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

void FileLock::release() {
    if (handle_ == kInvalid) return;
    const int fd = static_cast<int>(handle_);
    ::flock(fd, LOCK_UN);
    ::close(fd);
    handle_ = kInvalid;
}

std::string FileLock::read_all() {
    std::string content;
    if (handle_ == kInvalid) return content;

    const int fd = static_cast<int>(handle_);
    if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) return content;

    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = ::read(fd, buffer, sizeof(buffer))) > 0) {
        content.append(buffer, static_cast<std::size_t>(bytes_read));
    }
    return content;
}

bool FileLock::replace_all(const std::string& content) {
    if (handle_ == kInvalid) return false;

    const int fd = static_cast<int>(handle_);
    if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) return false;
    if (::ftruncate(fd, 0) != 0) return false;
    if (content.empty()) return true;

    const ssize_t written = ::write(fd, content.c_str(), content.length());
    return written == static_cast<ssize_t>(content.length());
}

bool FileLock::append(const std::string& content) {
    if (handle_ == kInvalid) return false;

    const int fd = static_cast<int>(handle_);
    if (::lseek(fd, 0, SEEK_END) == static_cast<off_t>(-1)) return false;
    if (content.empty()) return true;

    const ssize_t written = ::write(fd, content.c_str(), content.length());
    return written == static_cast<ssize_t>(content.length());
}


namespace {

sem_t* as_sem(std::intptr_t h) {
    return reinterpret_cast<sem_t*>(h);
}

}

Semaphore Semaphore::create_or_open(const std::string& name, unsigned initial) {
    Semaphore sem;

    sem_t* s = ::sem_open(name.c_str(), O_CREAT, 0666, initial);
    if (s == SEM_FAILED) {
        return sem;
    }

    sem.handle_ = reinterpret_cast<std::intptr_t>(s);
    return sem;
}

void Semaphore::unlink(const std::string& name) {
    if (name.empty()) return;
    ::sem_unlink(name.c_str());
}

Semaphore::~Semaphore() {
    close();
}

Semaphore::Semaphore(Semaphore&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

Semaphore& Semaphore::operator=(Semaphore&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

void Semaphore::close() {
    if (handle_ == kInvalid) return;
    ::sem_close(as_sem(handle_));
    handle_ = kInvalid;
}

bool Semaphore::post() {
    if (handle_ == kInvalid) return false;
    return ::sem_post(as_sem(handle_)) == 0;
}

WaitResult Semaphore::try_wait() {
    if (handle_ == kInvalid) return WaitResult::Failed;
    if (::sem_trywait(as_sem(handle_)) == 0) {
        return WaitResult::Acquired;
    }
    return (errno == EAGAIN || errno == EINTR) ? WaitResult::WouldBlock
                                               : WaitResult::Failed;
}

void Semaphore::drain() {
    while (try_wait() == WaitResult::Acquired) {
    }
}


bool MessagePipe::create_endpoint(const std::string& path) {
    // EEXIST is success: a stale FIFO from a killed run is reusable.
    return ::mkfifo(path.c_str(), 0666) == 0 || errno == EEXIST;
}

void MessagePipe::destroy_endpoint(const std::string& path) {
    if (path.empty()) return;
    std::remove(path.c_str());
}

MessagePipe MessagePipe::open_reader(const std::string& path) {
    MessagePipe pipe;

    // Without O_NONBLOCK, open() waits for a writer.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        return pipe;
    }

    pipe.handle_ = static_cast<std::intptr_t>(fd);
    return pipe;
}

MessagePipe::~MessagePipe() {
    close();
}

MessagePipe::MessagePipe(MessagePipe&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

MessagePipe& MessagePipe::operator=(MessagePipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

void MessagePipe::close() {
    if (handle_ == kInvalid) return;
    ::close(static_cast<int>(handle_));
    handle_ = kInvalid;
}

ReceiveStatus MessagePipe::receive(char* buffer,
                                   std::size_t max_bytes,
                                   std::chrono::milliseconds timeout,
                                   std::size_t& bytes_read) {
    bytes_read = 0;
    if (handle_ == kInvalid) return ReceiveStatus::Error;

    const int fd = static_cast<int>(handle_);

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv;
    const long ms = static_cast<long>(timeout.count());
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    const int ready = ::select(fd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready < 0) {
        return (errno == EINTR) ? ReceiveStatus::Retry : ReceiveStatus::Error;
    }
    if (ready == 0) {
        return ReceiveStatus::Timeout;
    }

    const ssize_t n = ::read(fd, buffer, max_bytes);
    if (n > 0) {
        bytes_read = static_cast<std::size_t>(n);
        return ReceiveStatus::Data;
    }
    if (n == 0) {
        return ReceiveStatus::Eof;
    }
    return (errno == EAGAIN || errno == EINTR) ? ReceiveStatus::Retry
                                               : ReceiveStatus::Error;
}

long MessagePipe::receive_blocking(char* buffer, std::size_t max_bytes) {
    if (handle_ == kInvalid) return -1;

    const int fd = static_cast<int>(handle_);

    const int flags = ::fcntl(fd, F_GETFL);
    if (flags != -1) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    return static_cast<long>(::read(fd, buffer, max_bytes));
}

}
