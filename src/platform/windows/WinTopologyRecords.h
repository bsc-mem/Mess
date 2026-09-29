#pragma once
#include <windows.h>
#include <cstddef>
#include <cstring>

namespace platform::win {
// Wire-compatible with old SDKs whose final Reserved bytes contain GroupCount.
// https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-numa_node_relationship
template<class F>
bool for_each_numa_affinity(const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& record,
                            bool extended, F&& visit) {
    constexpr std::size_t payload = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode);
    constexpr std::size_t masks = payload + offsetof(NUMA_NODE_RELATIONSHIP, GroupMask);
    constexpr std::size_t group_count = payload + sizeof(DWORD) + 18;
    static_assert(masks == payload + 24, "unexpected Windows NUMA layout");
    if (record.Size < masks + sizeof(GROUP_AFFINITY)) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
    WORD count = 1;
    if (extended) {
        std::memcpy(&count, bytes + group_count, sizeof(count));
        if (count == 0) count = 1; // Older Windows reports the single GroupMask.
    }
    if (count > (record.Size - masks) / sizeof(GROUP_AFFINITY)) return false;
    for (WORD i = 0; i < count; ++i) {
        GROUP_AFFINITY affinity{};
        std::memcpy(&affinity, bytes + masks + i * sizeof(affinity), sizeof(affinity));
        visit(affinity);
    }
    return true;
}
} // namespace platform::win
