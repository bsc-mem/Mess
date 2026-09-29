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

#pragma once

#include <windows.h>
#include <string>
#include <utility>
#include <vector>

namespace platform::win {

// Unique ownership for both NULL and INVALID_HANDLE_VALUE based APIs.
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

inline std::string last_error(const char* operation, DWORD code = GetLastError()) {
    return std::string(operation) + " failed (Windows error " + std::to_string(code) + ")";
}

inline Handle make_job(std::string& error) {
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) { error = last_error("CreateJobObjectW"); return Handle{}; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        error = last_error("SetInformationJobObject");
        return Handle{};
    }
    return job;
}

// Restrict redirected children to their own handles, including a valid stdin.
class RedirectedStartup {
public:
    RedirectedStartup() { info.StartupInfo.cb = sizeof(info); }
    ~RedirectedStartup() {
        if (info.lpAttributeList) DeleteProcThreadAttributeList(info.lpAttributeList);
    }
    RedirectedStartup(const RedirectedStartup&) = delete;
    RedirectedStartup& operator=(const RedirectedStartup&) = delete;
    bool initialize(HANDLE output, std::string& error, HANDLE input = nullptr) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!input) {
            input_.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, nullptr));
            if (!input_) { error = last_error("open NUL"); return false; }
            input = input_.get();
        }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
            error = last_error("InitializeProcThreadAttributeList"); return false;
        }
        info.lpAttributeList = attributes;
        inherited_[0] = output;
        inherited_[1] = input;
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited_, sizeof(inherited_), nullptr, nullptr)) {
            error = last_error("UpdateProcThreadAttribute"); return false;
        }
        info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        info.StartupInfo.hStdOutput = output;
        info.StartupInfo.hStdError = output;
        info.StartupInfo.hStdInput = input;
        return true;
    }
    STARTUPINFOEXW info{};
private:
    Handle input_;
    HANDLE inherited_[2]{};
    std::vector<unsigned char> storage_;
};

} // namespace platform::win
