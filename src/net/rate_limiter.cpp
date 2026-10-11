#include "net/rate_limiter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>

namespace erikslund::net {

namespace {

constexpr auto kPruneInterval = std::chrono::minutes(1);
constexpr std::size_t kMaximumEvictionsPerAdmission = 64;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t kMaximumGcraIntervalNanoseconds = 1'000'000'000'000'000'000;

std::int64_t saturating_add(std::int64_t left, std::int64_t right) noexcept {
    if (right > std::numeric_limits<std::int64_t>::max() - left)
        return std::numeric_limits<std::int64_t>::max();
    return left + right;
}

std::int64_t saturating_multiply(std::size_t left, std::int64_t right) noexcept {
    if (left > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / right))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(left) * right;
}

} // namespace

RateLimiter::Lease::Lease(RateLimiter& limiter, std::size_t shard_index,
                          ClientState& state) noexcept
    : limiter_(&limiter), shard_index_(shard_index), state_(&state) {}

RateLimiter::Lease::~Lease() {
    reset();
}

RateLimiter::Lease::Lease(Lease&& other) noexcept
    : limiter_(std::exchange(other.limiter_, nullptr)), shard_index_(other.shard_index_),
      state_(std::exchange(other.state_, nullptr)) {}

RateLimiter::Lease& RateLimiter::Lease::operator=(Lease&& other) noexcept {
    if (this == &other)
        return *this;
    reset();
    limiter_ = std::exchange(other.limiter_, nullptr);
    shard_index_ = other.shard_index_;
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

void RateLimiter::Lease::reset() noexcept {
    if (limiter_ != nullptr)
        limiter_->release(shard_index_, state_);
    limiter_ = nullptr;
    state_ = nullptr;
}

RateLimiter::RateLimiter(const core::LimitsConfig& limits) {
    update_traffic_limits(limits);
}

void RateLimiter::update_traffic_limits(const core::LimitsConfig& limits,
                                        Clock::time_point now) {
    std::lock_guard update_lock(traffic_update_mutex_);
    const TrafficLimits replacement{
        .messages_per_second = limits.messages_per_second_per_ip,
        .message_burst = limits.message_burst_per_ip,
        .bytes_per_second = limits.bytes_per_second_per_ip,
        .byte_burst = limits.byte_burst_per_ip,
    };
    // Only this update mutex's owner writes policies. Unchanged reloads need no shard locks.
    if (shards_.front().traffic_limits == replacement)
        return;
    // No lease operation holds two shards at once. Take every lock before mutation so even a
    // failed lock acquisition leaves the previous policy and token balances intact.
    std::array<std::unique_lock<std::mutex>, kShardCount> locks;
    for (std::size_t index = 0; index < shards_.size(); ++index)
        locks[index] = std::unique_lock(shards_[index].mutex);
    for (Shard& shard : shards_) {
        const TrafficLimits& prior = shard.traffic_limits;
        for (auto& [key, state] : shard.clients) {
            refill(state.message_bucket, now, prior.messages_per_second, prior.message_burst);
            refill(state.byte_bucket, now, prior.bytes_per_second, prior.byte_burst);
            state.message_bucket.tokens = std::min(
                state.message_bucket.tokens, static_cast<double>(limits.message_burst_per_ip));
            state.byte_bucket.tokens = std::min(
                state.byte_bucket.tokens, static_cast<double>(limits.byte_burst_per_ip));
        }
        shard.traffic_limits = replacement;
    }
}

RateLimiter::Admission RateLimiter::acquire(const ClientKey& client_key,
                                            const core::LimitsConfig& limits) {
    const std::size_t shard_index = shard_index_for(client_key);
    Shard& shard = shards_[shard_index];
    std::unique_lock lock(shard.mutex);
    auto now = Clock::now();
    if (shard.last_pruned == std::chrono::steady_clock::time_point{} ||
        now - shard.last_pruned >= kPruneInterval) {
        if (prune(shard, now, std::chrono::seconds(limits.client_state_retention_seconds)))
            shard.last_pruned = now;
    }
    bool known_client = shard.clients.contains(client_key);
    if (!known_client && !reserve_tracking_slot(limits.max_tracked_client_ips)) {
        if (inactive_shards_.load(std::memory_order_relaxed) == 0)
            return {.result = AdmissionResult::SourceCapacity, .lease = {}};
        // Reclamation visits unrelated shards, so release the target first. Active leases keep
        // their map nodes stable; only inactive nodes participate in the reclamation lists.
        lock.unlock();
        const bool reserved = reclaim_tracking_slot(limits.max_tracked_client_ips);
        lock.lock();
        now = Clock::now();
        known_client = shard.clients.contains(client_key);
        if (!known_client && !reserved)
            return {.result = AdmissionResult::SourceCapacity, .lease = {}};
        if (known_client && reserved)
            tracked_clients_.fetch_sub(1, std::memory_order_relaxed);
    }

    decltype(shard.clients)::iterator iterator;
    bool inserted = false;
    try {
        std::tie(iterator, inserted) = shard.clients.try_emplace(client_key);
    } catch (...) {
        if (!known_client)
            tracked_clients_.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
    ClientState& state = iterator->second;
    if (inserted) {
        state.key = client_key;
        state.connection_bucket = {
            .tokens = static_cast<double>(limits.connection_burst_per_ip), .updated_at = now};
        state.message_bucket = {
            .tokens = static_cast<double>(shard.traffic_limits.message_burst), .updated_at = now};
        state.byte_bucket = {
            .tokens = static_cast<double>(shard.traffic_limits.byte_burst), .updated_at = now};
    }
    state.last_seen = now;
    if (state.active == 0) {
        unlink_inactive(shard, state);
        append_inactive(shard, state);
    }
    refill(state.connection_bucket, now, limits.connections_per_second_per_ip,
           limits.connection_burst_per_ip);
    if (state.active >= limits.max_connections_per_ip)
        return {.result = AdmissionResult::ConcurrentLimit, .lease = {}};
    if (state.connection_bucket.tokens < 1.0)
        return {.result = AdmissionResult::RateLimit, .lease = {}};
    if (!consume_global_connection(now, limits)) {
        if (inserted) {
            unlink_inactive(shard, state);
            shard.clients.erase(iterator);
            tracked_clients_.fetch_sub(1, std::memory_order_relaxed);
        }
        return {.result = AdmissionResult::GlobalRateLimit, .lease = {}};
    }
    state.connection_bucket.tokens -= 1.0;
    unlink_inactive(shard, state);
    ++state.active;
    return {.result = AdmissionResult::Accepted,
            .lease = Lease(*this, shard_index, state)};
}

bool RateLimiter::consume_traffic(const Lease& lease, std::size_t bytes,
                                  std::size_t messages, Clock::time_point now) {
    if (!lease || lease.limiter_ != this)
        return false;
    Shard& shard = shards_[lease.shard_index_];
    std::lock_guard lock(shard.mutex);
    ClientState& state = *lease.state_;
    state.last_seen = std::max(state.last_seen, now);
    const TrafficLimits& limits = shard.traffic_limits;
    refill(state.byte_bucket, now, limits.bytes_per_second, limits.byte_burst);
    refill(state.message_bucket, now, limits.messages_per_second, limits.message_burst);
    if (state.byte_bucket.tokens < static_cast<double>(bytes) ||
        state.message_bucket.tokens < static_cast<double>(messages))
        return false;
    state.byte_bucket.tokens -= static_cast<double>(bytes);
    state.message_bucket.tokens -= static_cast<double>(messages);
    return true;
}

void RateLimiter::release(std::size_t shard_index, ClientState* state) noexcept {
    Shard& shard = shards_[shard_index];
    std::lock_guard lock(shard.mutex);
    if (state->active > 0) {
        --state->active;
        if (state->active == 0) {
            state->last_seen = Clock::now();
            append_inactive(shard, *state);
        }
    }
}

std::size_t RateLimiter::shard_index_for(const ClientKey& client_key) const noexcept {
    return ClientKeyHash{}(client_key) % shards_.size();
}

bool RateLimiter::prune(Shard& shard, std::chrono::steady_clock::time_point now,
                        std::chrono::seconds retention) {
    for (std::size_t evicted = 0; evicted < kMaximumEvictionsPerAdmission; ++evicted) {
        if (shard.oldest_inactive == nullptr ||
            now - shard.oldest_inactive->last_seen <= retention)
            return true;
        evict_oldest_inactive(shard);
    }
    return false;
}

bool RateLimiter::evict_oldest_inactive(Shard& shard) {
    if (shard.oldest_inactive == nullptr)
        return false;
    const ClientKey key = shard.oldest_inactive->key;
    unlink_inactive(shard, *shard.oldest_inactive);
    shard.clients.erase(key);
    tracked_clients_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void RateLimiter::unlink_inactive(Shard& shard, ClientState& state) noexcept {
    if (!state.inactive_listed)
        return;
    if (state.previous_inactive != nullptr)
        state.previous_inactive->next_inactive = state.next_inactive;
    else
        shard.oldest_inactive = state.next_inactive;
    if (state.next_inactive != nullptr)
        state.next_inactive->previous_inactive = state.previous_inactive;
    else
        shard.newest_inactive = state.previous_inactive;
    state.previous_inactive = nullptr;
    state.next_inactive = nullptr;
    state.inactive_listed = false;
    if (shard.oldest_inactive == nullptr) {
        const auto shard_index = static_cast<std::size_t>(&shard - shards_.data());
        inactive_shards_.fetch_and(~(std::uint64_t{1} << shard_index),
                                    std::memory_order_relaxed);
    }
}

void RateLimiter::append_inactive(Shard& shard, ClientState& state) noexcept {
    state.previous_inactive = shard.newest_inactive;
    if (shard.newest_inactive != nullptr)
        shard.newest_inactive->next_inactive = &state;
    else {
        shard.oldest_inactive = &state;
        const auto shard_index = static_cast<std::size_t>(&shard - shards_.data());
        inactive_shards_.fetch_or(std::uint64_t{1} << shard_index,
                                   std::memory_order_relaxed);
    }
    shard.newest_inactive = &state;
    state.inactive_listed = true;
}

bool RateLimiter::reclaim_tracking_slot(std::size_t maximum) {
    if (reserve_tracking_slot(maximum))
        return true;
    // Full active tables are the common rejection case: never touch unrelated traffic locks.
    if (inactive_shards_.load(std::memory_order_relaxed) == 0)
        return false;
    std::lock_guard reclamation_lock(reclamation_mutex_);
    if (reserve_tracking_slot(maximum))
        return true;
    // A lowered cap can leave many inactive nodes to reclaim. Spread that work across attempts.
    for (std::size_t visited = 0; visited < kMaximumEvictionsPerAdmission; ++visited) {
        const auto candidates = std::rotr(inactive_shards_.load(std::memory_order_relaxed),
                                           static_cast<int>(next_reclamation_shard_));
        if (candidates == 0)
            break;
        const std::size_t shard_index =
            (next_reclamation_shard_ + std::countr_zero(candidates)) % shards_.size();
        next_reclamation_shard_ = (shard_index + 1) % shards_.size();
        Shard& shard = shards_[shard_index];
        std::lock_guard lock(shard.mutex);
        evict_oldest_inactive(shard);
        if (reserve_tracking_slot(maximum))
            return true;
    }
    return false;
}

bool RateLimiter::reserve_tracking_slot(std::size_t maximum) {
    std::size_t current = tracked_clients_.load(std::memory_order_relaxed);
    while (current < maximum) {
        if (tracked_clients_.compare_exchange_weak(current, current + 1,
                                                   std::memory_order_relaxed))
            return true;
    }
    return false;
}

bool RateLimiter::consume_global_connection(std::chrono::steady_clock::time_point now,
                                            const core::LimitsConfig& limits) {
    const double interval_value =
        static_cast<double>(kNanosecondsPerSecond) / limits.global_connections_per_second;
    const auto interval = static_cast<std::int64_t>(
        std::clamp(std::ceil(interval_value), 1.0,
                   static_cast<double>(kMaximumGcraIntervalNanoseconds)));
    const std::int64_t tolerance =
        saturating_multiply(limits.global_connection_burst - 1, interval);
    const std::int64_t now_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::int64_t theoretical_arrival =
        global_theoretical_arrival_nanoseconds_.load(std::memory_order_relaxed);
    while (true) {
        const std::int64_t base = std::max(theoretical_arrival, now_nanoseconds);
        if (base - now_nanoseconds > tolerance)
            return false;
        const std::int64_t replacement = saturating_add(base, interval);
        if (global_theoretical_arrival_nanoseconds_.compare_exchange_weak(
                theoretical_arrival, replacement, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return true;
    }
}

void RateLimiter::refill(TokenBucket& bucket, std::chrono::steady_clock::time_point now,
                         double rate_per_second, std::size_t burst) {
    if (bucket.updated_at == std::chrono::steady_clock::time_point{}) {
        bucket.tokens = static_cast<double>(burst);
    } else {
        now = std::max(now, bucket.updated_at);
        const double elapsed = std::chrono::duration<double>(now - bucket.updated_at).count();
        bucket.tokens = std::min(static_cast<double>(burst),
                                 bucket.tokens + elapsed * rate_per_second);
    }
    bucket.updated_at = now;
}

} // namespace erikslund::net
