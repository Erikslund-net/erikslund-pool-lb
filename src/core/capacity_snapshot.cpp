#include "core/capacity_snapshot.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <glaze/glaze.hpp>

#include "core/errors.hpp"

namespace erikslund::core {

namespace {

constexpr std::size_t kMaximumMetadataBytes = 65'536;

void read_metadata(CapacitySnapshot& snapshot, const std::string& name,
                    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return;
    std::string content(kMaximumMetadataBytes + 1, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    const auto bytes = static_cast<std::size_t>(input.gcount());
    // Missing, unreadable and oversized files stay absent, never fabricated as zero counters.
    if (input.bad() || bytes > kMaximumMetadataBytes)
        return;
    content.resize(bytes);
    snapshot.files.emplace(name, std::move(content));
}

} // namespace

CapacitySnapshot read_capacity_snapshot(int target_pid) {
    if (target_pid <= 0)
        throw ConfigError("capacity snapshot requires a positive PID");
    CapacitySnapshot snapshot{.target_pid = target_pid, .open_file_descriptors = {}, .files = {}};
    const auto process_root = std::filesystem::path("/proc") / std::to_string(target_pid);
    for (const std::string name : {"limits", "stat", "status", "cgroup"})
        read_metadata(snapshot, name, process_root / name);

    std::error_code error;
    std::filesystem::directory_iterator iterator(process_root / "fd", error);
    std::uint64_t descriptors = 0;
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        ++descriptors;
        iterator.increment(error);
    }
    if (!error)
        snapshot.open_file_descriptors = descriptors;

    read_metadata(snapshot, "boot_id", "/proc/sys/kernel/random/boot_id");
    read_metadata(snapshot, "file_max", "/proc/sys/fs/file-max");
    read_metadata(snapshot, "file_nr", "/proc/sys/fs/file-nr");
    for (const std::string name : {"ip_local_port_range", "ip_local_reserved_ports"})
        read_metadata(snapshot, name, std::filesystem::path("/proc/sys/net/ipv4") / name);
    for (const std::string name : {"sockstat", "sockstat6", "snmp", "netstat"})
        read_metadata(snapshot, name, process_root / "net" / name);

    const auto membership = snapshot.files.find("cgroup");
    if (membership != snapshot.files.end()) {
        std::istringstream lines(membership->second);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.starts_with("0::/"))
                continue;
            const std::filesystem::path relative = line.substr(std::string_view("0::/").size());
            bool escapes_root = relative.is_absolute();
            for (const auto& component : relative)
                escapes_root = escapes_root || component == "..";
            if (escapes_root)
                break;
            const auto cgroup_root = std::filesystem::path("/sys/fs/cgroup") / relative;
            for (const std::string name : {"cpu.max", "cpu.stat", "cpuset.cpus.effective",
                                           "memory.max", "memory.current", "memory.events"})
                read_metadata(snapshot, name, cgroup_root / name);
            break;
        }
    }
    return snapshot;
}

std::string capacity_snapshot_json(int target_pid) {
    auto result = glz::write_json(read_capacity_snapshot(target_pid));
    if (!result)
        throw ConfigError("could not encode capacity snapshot");
    return std::move(*result);
}

} // namespace erikslund::core
