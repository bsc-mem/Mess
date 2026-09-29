#pragma once

namespace measurement {
// Stop the batch if readiness times out or a worker exits during initialization.
template<class Manager>
bool wait_for_ready_workers(Manager& manager, int timeout_seconds = 120) {
    if (manager.wait_for_traffic_gen_ready(timeout_seconds) &&
        manager.is_traffic_gen_running(manager.active_traffic_gen_pid())) {
        return true;
    }
    manager.kill_all_traffic_gen();
    return false;
}
} // namespace measurement
