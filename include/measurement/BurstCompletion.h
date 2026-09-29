#pragma once

#include <chrono>
#include "platform/Ipc.h"
#include "platform/Process.h"
#include <string>

namespace measurement {

enum class BurstCompletion { Pending, Complete, Failed, TimedOut };

// The producer closes its record before posting done. Never block on either IPC
// endpoint: a crashed child or an incomplete record must reach the retry path.
BurstCompletion poll_burst_completion(
    platform::Semaphore& done, platform::MessagePipe& pipe, platform::ProcessId pid,
    std::chrono::steady_clock::time_point deadline,
    std::string& record, std::string& error);

}
