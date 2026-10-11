#pragma once
// Read-only Linux process/cgroup evidence for offline capacity qualification.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace erikslund::core {

struct CapacitySnapshot {
    int schema_version = 1;
    int target_pid = 0;
    std::optional<std::uint64_t> open_file_descriptors;
    std::map<std::string, std::string> files;
};

[[nodiscard]] CapacitySnapshot read_capacity_snapshot(int target_pid);
[[nodiscard]] std::string capacity_snapshot_json(int target_pid);

} // namespace erikslund::core
