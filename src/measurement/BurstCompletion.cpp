#include "measurement/BurstCompletion.h"

namespace measurement {

BurstCompletion poll_burst_completion(
    platform::Semaphore& done, platform::MessagePipe& pipe, platform::ProcessId pid,
    std::chrono::steady_clock::time_point deadline,
    std::string& record, std::string& error) {
    record.clear();
    error.clear();
    if (!done.valid() || !pipe.valid() || pid <= 0) {
        error = "invalid pointer-chase IPC endpoint";
        return BurstCompletion::Failed;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        error = "pointer-chase burst exceeded its deadline";
        return BurstCompletion::TimedOut;
    }
    const auto ready = done.try_wait();
    if (ready != platform::WaitResult::Acquired) {
        if (ready == platform::WaitResult::Failed) {
            error = "pointer-chase completion semaphore failed";
            return BurstCompletion::Failed;
        }
        if (!platform::process_exists(pid)) {
            error = "pointer-chase process exited before completing its burst";
            return BurstCompletion::Failed;
        }
        return BurstCompletion::Pending;
    }

    char buffer[1024];
    // Bound both bytes and reads, including repeated signal interruptions.
    for (int attempt = 0; attempt < 8 && record.size() < sizeof(buffer); ++attempt) {
        std::size_t count = 0;
        const auto status = pipe.receive(buffer, sizeof(buffer) - record.size(),
                                         std::chrono::milliseconds(0), count);
        if (status == platform::ReceiveStatus::Data && count > 0) {
            record.append(buffer, static_cast<size_t>(count));
            if (record.back() == '\n') return BurstCompletion::Complete;
        } else if (status == platform::ReceiveStatus::Retry) {
            continue;
        } else {
            break;
        }
    }
    error = "missing, truncated, or unreadable pointer-chase result";
    return BurstCompletion::Failed;
}

}
