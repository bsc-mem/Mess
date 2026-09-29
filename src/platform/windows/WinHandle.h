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
