#pragma once
// Immutable route snapshots and mutable per-backend health/load state.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "net/endpoint.hpp"

namespace erikslund::routing {

inline constexpr std::size_t kBackendCacheLineBytes = 64;

struct BackendMetrics {
    std::string name;
    std::string pool;
    std::uint64_t generation = 0;
    alignas(kBackendCacheLineBytes) std::atomic<std::uint64_t> active_connections{0};
    alignas(kBackendCacheLineBytes) std::atomic<std::uint64_t> connection_attempts{0};
    std::atomic<std::uint64_t> connection_errors{0};
};

struct BackendState {
    std::string name;
    std::string pool;
    std::string configured_address;
    std::string configured_health_address;
    std::vector<net::SocketAddress> socket_addresses;
    std::vector<net::SocketAddress> health_socket_addresses;
    bool send_proxy_v2 = true;
    alignas(kBackendCacheLineBytes) std::atomic<bool> healthy{false};
    std::atomic<int> consecutive_successes{0};
    std::atomic<int> consecutive_failures{0};
    // Selection uses only this destination generation's load; observability also includes
    // established sessions still draining from earlier addresses of the same logical backend.
    alignas(kBackendCacheLineBytes) std::atomic<std::uint64_t> active_connections{0};
    std::shared_ptr<BackendMetrics> metrics;
};

struct BackendMetricEntry {
    std::weak_ptr<BackendMetrics> metrics;
    std::weak_ptr<BackendState> current_backend;
};

struct PoolState {
    std::string name;
    std::vector<std::shared_ptr<BackendState>> backends;
};

struct RoutingTable {
    std::string active_pool;
    bool failover = true;
    core::HealthConfig health;
    std::vector<net::SocketAddress> upstream_source_addresses;
    std::vector<PoolState> pools;
    // Removed identities stay observable and reusable while an older routing snapshot owns
    // them. Weak entries avoid accumulating permanent tombstones through configuration churn.
    std::vector<BackendMetricEntry> backend_metrics;
    // New identities get a new counter label without retaining unbounded metric tombstones.
    std::uint64_t next_backend_metrics_generation = 1;
    std::size_t active_pool_index = 0;
};

std::shared_ptr<RoutingTable> make_routing_table(
    const core::Config& config, const std::shared_ptr<const RoutingTable>& previous = {});
std::shared_ptr<BackendState> select_backend(const RoutingTable& table, std::uint64_t sequence);
bool has_routable_backend(const RoutingTable& table);

} // namespace erikslund::routing
