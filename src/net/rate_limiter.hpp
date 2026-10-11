#pragma once
// Per-client-IP concurrent connection and token-bucket admission control.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "core/config.hpp"
#include "net/endpoint.hpp"

namespace erikslund::net {

enum class AdmissionResult {
    Accepted,
    ConcurrentLimit,
    RateLimit,
    GlobalRateLimit,
    SourceCapacity,
};

class RateLimiter {
    struct ClientState;

public:
    using Clock = std::chrono::steady_clock;

    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        void reset() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept {
            return limiter_ != nullptr;
        }

    private:
        friend class RateLimiter;

        Lease(RateLimiter& limiter, std::size_t shard_index, ClientState& state) noexcept;

        RateLimiter* limiter_ = nullptr;
        std::size_t shard_index_ = 0;
        ClientState* state_ = nullptr;
    };

    struct Admission {
        AdmissionResult result = AdmissionResult::RateLimit;
        Lease lease;
    };

    explicit RateLimiter(const core::LimitsConfig& limits = {});

    Admission acquire(const ClientKey& client_key, const core::LimitsConfig& limits);
    bool consume_traffic(const Lease& lease, std::size_t bytes, std::size_t messages,
                         Clock::time_point now);
    void update_traffic_limits(const core::LimitsConfig& limits,
                               Clock::time_point now = Clock::now());

    [[nodiscard]] std::size_t tracked_clients() const noexcept {
        return tracked_clients_.load(std::memory_order_relaxed);
    }

private:
    struct TokenBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point updated_at{};
    };

    struct ClientState {
        ClientKey key;
        TokenBucket connection_bucket;
        TokenBucket message_bucket;
        TokenBucket byte_bucket;
        std::size_t active = 0;
        std::chrono::steady_clock::time_point last_seen{};
        ClientState* previous_inactive = nullptr;
        ClientState* next_inactive = nullptr;
        bool inactive_listed = false;
    };

    struct TrafficLimits {
        double messages_per_second = 0.0;
        std::size_t message_burst = 0;
        double bytes_per_second = 0.0;
        std::size_t byte_burst = 0;

        bool operator==(const TrafficLimits&) const = default;
    };

    static constexpr std::size_t kShardCount = std::numeric_limits<std::uint64_t>::digits;

    struct Shard {
        std::mutex mutex;
        std::unordered_map<ClientKey, ClientState, ClientKeyHash> clients;
        TrafficLimits traffic_limits;
        ClientState* oldest_inactive = nullptr;
        ClientState* newest_inactive = nullptr;
        std::chrono::steady_clock::time_point last_pruned{};
    };

    [[nodiscard]] std::size_t shard_index_for(const ClientKey& client_key) const noexcept;
    void release(std::size_t shard_index, ClientState* state) noexcept;
    bool prune(Shard& shard, std::chrono::steady_clock::time_point now,
               std::chrono::seconds retention);
    bool evict_oldest_inactive(Shard& shard);
    bool reserve_tracking_slot(std::size_t maximum);
    bool reclaim_tracking_slot(std::size_t maximum);
    void unlink_inactive(Shard& shard, ClientState& state) noexcept;
    void append_inactive(Shard& shard, ClientState& state) noexcept;
    bool consume_global_connection(std::chrono::steady_clock::time_point now,
                                   const core::LimitsConfig& limits);
    static void refill(TokenBucket& bucket, std::chrono::steady_clock::time_point now,
                       double rate_per_second, std::size_t burst);

    std::array<Shard, kShardCount> shards_;
    // Only policy publication takes multiple shards, always in ascending order. Capacity
    // reclamation releases each shard before visiting another.
    std::mutex traffic_update_mutex_;
    std::mutex reclamation_mutex_;
    std::size_t next_reclamation_shard_ = 0;
    // A hint only: list contents and node lifetimes remain protected by each shard lock.
    std::atomic<std::uint64_t> inactive_shards_{0};
    std::atomic<std::int64_t> global_theoretical_arrival_nanoseconds_{0};
    std::atomic<std::size_t> tracked_clients_{0};
};

} // namespace erikslund::net
