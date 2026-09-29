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

#include "WinStrings.h"

#include <windows.h>

#include <algorithm>

namespace platform {

namespace {

HANDLE as_handle(std::intptr_t h) {
    return reinterpret_cast<HANDLE>(h);
}

void whole_file_range(OVERLAPPED& ov) {
    ZeroMemory(&ov, sizeof(ov));
}

}

FileLock FileLock::acquire(const std::filesystem::path& path) {
    FileLock lock;

    const HANDLE h = CreateFileW(path.wstring().c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return lock;
    }

    OVERLAPPED ov;
    whole_file_range(ov);
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
        CloseHandle(h);
        return lock;
    }

    lock.handle_ = reinterpret_cast<std::intptr_t>(h);
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
    const HANDLE h = as_handle(handle_);

    OVERLAPPED ov;
    whole_file_range(ov);
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
    CloseHandle(h);
    handle_ = kInvalid;
}

std::string FileLock::read_all() {
    std::string content;
    if (handle_ == kInvalid) return content;

    const HANDLE h = as_handle(handle_);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return content;

    char  buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        content.append(buffer, bytes_read);
    }
    return content;
}

bool FileLock::replace_all(const std::string& content) {
    if (handle_ == kInvalid) return false;

    const HANDLE h = as_handle(handle_);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return false;
    if (!SetEndOfFile(h)) return false;
    if (content.empty()) return true;

    DWORD written = 0;
    if (!WriteFile(h, content.c_str(), static_cast<DWORD>(content.length()), &written, nullptr)) {
        return false;
    }
    return written == content.length();
}

bool FileLock::append(const std::string& content) {
    if (handle_ == kInvalid) return false;

    const HANDLE h = as_handle(handle_);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, nullptr, FILE_END)) return false;
    if (content.empty()) return true;

    DWORD written = 0;
    if (!WriteFile(h, content.c_str(), static_cast<DWORD>(content.length()), &written, nullptr)) {
        return false;
    }
    return written == content.length();
}

namespace {

// A Win32 object name may hold no '/', so it is dropped and interior ones folded to '_'.
// Deterministic, so ptr_chase derives the same name.
std::wstring semaphore_object_name(const std::string& posix_name) {
    std::string bare = posix_name;
    if (!bare.empty() && bare.front() == '/') {
        bare.erase(bare.begin());
    }
    std::replace(bare.begin(), bare.end(), '/', '_');
    std::replace(bare.begin(), bare.end(), '\\', '_');
    return L"Local\\" + win::widen(bare);
}

}

Semaphore Semaphore::create_or_open(const std::string& name, unsigned initial) {
    Semaphore sem;

    const std::wstring object = semaphore_object_name(name);

    const HANDLE h = CreateSemaphoreW(nullptr,
                                      static_cast<LONG>(initial),
                                      LONG_MAX,
                                      object.c_str());
    if (h == nullptr) {
        return sem;
    }

    sem.handle_ = reinterpret_cast<std::intptr_t>(h);
    return sem;
}

void Semaphore::unlink(const std::string&) {
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
    CloseHandle(as_handle(handle_));
    handle_ = kInvalid;
}

bool Semaphore::post() {
    if (handle_ == kInvalid) return false;
    return ReleaseSemaphore(as_handle(handle_), 1, nullptr) != 0;
}

WaitResult Semaphore::try_wait() {
    if (handle_ == kInvalid) return WaitResult::Failed;
    switch (WaitForSingleObject(as_handle(handle_), 0)) {
        case WAIT_OBJECT_0: return WaitResult::Acquired;
        case WAIT_TIMEOUT:  return WaitResult::WouldBlock;
        default:            return WaitResult::Failed;
    }
}

void Semaphore::drain() {
    while (try_wait() == WaitResult::Acquired) {
    }
}

namespace {

// Only the final component is used: a named pipe is not a filesystem object.
std::wstring pipe_object_name(const std::string& posix_path) {
    const std::size_t sep = posix_path.find_last_of("/\\");
    const std::string leaf = (sep == std::string::npos) ? posix_path
                                                        : posix_path.substr(sep + 1);
    return L"\\\\.\\pipe\\" + win::widen(leaf);
}

}

// The server instance IS the reader's handle, created in open_reader().
bool MessagePipe::create_endpoint(const std::string&) {
    return true;
}

void MessagePipe::destroy_endpoint(const std::string&) {
}

MessagePipe MessagePipe::open_reader(const std::string& path) {
    MessagePipe pipe;

    const std::wstring object = pipe_object_name(path);

    // PIPE_NOWAIT enables nonblocking reads.
    const HANDLE h = CreateNamedPipeW(object.c_str(),
                                      PIPE_ACCESS_INBOUND,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                      PIPE_UNLIMITED_INSTANCES,
                                      0,
                                      1 << 16,
                                      0,
                                      nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return pipe;
    }

    pipe.handle_ = reinterpret_cast<std::intptr_t>(h);
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
    const HANDLE h = as_handle(handle_);
    DisconnectNamedPipe(h);
    CloseHandle(h);
    handle_ = kInvalid;
}

namespace {

// ERROR_PIPE_CONNECTED is success: a client connected between CreateNamedPipeW and this call.
void accept_if_pending(HANDLE h) {
    if (ConnectNamedPipe(h, nullptr)) return;
    const DWORD err = GetLastError();
    (void)err;
}

void recycle_instance(HANDLE h) {
    DisconnectNamedPipe(h);
    ConnectNamedPipe(h, nullptr);
}

}

ReceiveStatus MessagePipe::receive(char* buffer,
                                   std::size_t max_bytes,
                                   std::chrono::milliseconds timeout,
                                   std::size_t& bytes_read) {
    bytes_read = 0;
    if (handle_ == kInvalid) return ReceiveStatus::Error;

    const HANDLE h = as_handle(handle_);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        accept_if_pending(h);

        DWORD n = 0;
        if (ReadFile(h, buffer, static_cast<DWORD>(max_bytes), &n, nullptr) && n > 0) {
            bytes_read = n;
            return ReceiveStatus::Data;
        }

        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            recycle_instance(h);
            if (std::chrono::steady_clock::now() >= deadline) {
                return ReceiveStatus::Timeout;
            }
            Sleep(1);
            continue;
        }
        if (err != ERROR_NO_DATA && err != ERROR_PIPE_LISTENING && n == 0) {
            if (err != ERROR_SUCCESS) {
                return ReceiveStatus::Error;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return ReceiveStatus::Timeout;
        }
        Sleep(5);
    }
}

long MessagePipe::receive_blocking(char* buffer, std::size_t max_bytes) {
    if (handle_ == kInvalid) return -1;

    const HANDLE h = as_handle(handle_);

    // ReadFile may return ERROR_NO_DATA before the pending write becomes readable.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        accept_if_pending(h);

        DWORD n = 0;
        if (ReadFile(h, buffer, static_cast<DWORD>(max_bytes), &n, nullptr) && n > 0) {
            return static_cast<long>(n);
        }

        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            return 0;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        Sleep(1);
    }
}

}
