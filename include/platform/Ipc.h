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

#ifndef PLATFORM_IPC_H
#define PLATFORM_IPC_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace platform {

class FileLock {
public:
    static FileLock acquire(const std::filesystem::path& path);

    FileLock() = default;
    ~FileLock();
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    bool valid() const { return handle_ != kInvalid; }
    std::string read_all();
    bool replace_all(const std::string& content);
    bool append(const std::string& content);
    void release();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;
};

enum class WaitResult {
    Acquired,
    WouldBlock,
    Failed
};

class Semaphore {
public:
    static Semaphore create_or_open(const std::string& name, unsigned initial = 0);
    static void unlink(const std::string& name);

    Semaphore() = default;
    ~Semaphore();
    Semaphore(Semaphore&& other) noexcept;
    Semaphore& operator=(Semaphore&& other) noexcept;
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    bool valid() const { return handle_ != kInvalid; }

    bool post();
    WaitResult try_wait();
    void drain();

    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;
};

enum class ReceiveStatus {
    Data,
    Timeout,
    Retry,
    Eof,
    Error
};

class MessagePipe {
public:
    static bool create_endpoint(const std::string& path);
    static void destroy_endpoint(const std::string& path);
    static MessagePipe open_reader(const std::string& path);

    MessagePipe() = default;
    ~MessagePipe();
    MessagePipe(MessagePipe&& other) noexcept;
    MessagePipe& operator=(MessagePipe&& other) noexcept;
    MessagePipe(const MessagePipe&) = delete;
    MessagePipe& operator=(const MessagePipe&) = delete;

    bool valid() const { return handle_ != kInvalid; }

    ReceiveStatus receive(char* buffer,
                          std::size_t max_bytes,
                          std::chrono::milliseconds timeout,
                          std::size_t& bytes_read);

    long receive_blocking(char* buffer, std::size_t max_bytes);

    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;
};

}

#endif // PLATFORM_IPC_H
