#include "net/timer_wheel.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace erikslund::net {

TimerWheel::TimerWheel(Clock::time_point now, std::size_t expected_entries)
    : current_tick_(tick_floor(now)) {
    locations_.reserve(expected_entries);
}

void TimerWheel::schedule(std::uint64_t session_id, std::uint64_t generation,
                          Clock::time_point deadline) {
    cancel(session_id);
    const std::uint64_t due_tick = std::max(tick_at_or_after(deadline), current_tick_ + 1);
    insert({.expiry = {.session_id = session_id, .generation = generation},
            .due_tick = due_tick});
}

void TimerWheel::cancel(std::uint64_t session_id) {
    const auto location = locations_.find(session_id);
    if (location == locations_.end())
        return;
    auto& bucket = buckets_[location->second.bucket];
    const std::size_t index = location->second.index;
    const std::size_t last = bucket.size() - 1;
    if (index != last) {
        bucket[index] = bucket[last];
        const auto swapped = locations_.find(bucket[index].expiry.session_id);
        if (swapped != locations_.end())
            swapped->second = {.bucket = location->second.bucket, .index = index};
    }
    bucket.pop_back();
    trim_bucket(bucket);
    locations_.erase(location);
}

std::vector<TimerExpiry> TimerWheel::advance(Clock::time_point now) {
    std::vector<TimerExpiry> expired;
    const std::uint64_t target_tick = tick_floor(now);
    if (target_tick <= current_tick_)
        return expired;
    const std::uint64_t ticks = std::min<std::uint64_t>(target_tick - current_tick_, kBucketCount);
    std::size_t expiry_count = 0;
    for (std::uint64_t offset = 1; offset <= ticks; ++offset)
        for (const Entry& entry : buckets_[(current_tick_ + offset) % kBucketCount])
            expiry_count += static_cast<std::size_t>(entry.due_tick <= target_tick);
    // Reserve before touching indexes: cleanup must remain safe if allocation fails. Compact
    // buckets in place so long-lived timers and bucket capacity survive ring rotations.
    expired.reserve(expiry_count);
    for (std::uint64_t offset = 1; offset <= ticks; ++offset) {
        const std::size_t bucket_index = (current_tick_ + offset) % kBucketCount;
        auto& bucket = buckets_[bucket_index];
        std::size_t retained = 0;
        for (const Entry& entry : bucket) {
            if (entry.due_tick <= target_tick) {
                expired.push_back(entry.expiry);
                locations_.erase(entry.expiry.session_id);
            } else {
                bucket[retained] = entry;
                locations_.find(entry.expiry.session_id)->second =
                    {.bucket = bucket_index, .index = retained};
                ++retained;
            }
        }
        bucket.resize(retained);
        trim_bucket(bucket);
    }
    current_tick_ = target_tick;
    return expired;
}

void TimerWheel::trim_bucket(std::vector<Entry>& bucket) noexcept {
    // A different bucket can see the fleet's peak on every tick. Retaining every high-water
    // allocation would multiply peak session memory by the ring size after repeated churn.
    if (bucket.capacity() <= kMaximumRetainedBucketEntries ||
        bucket.size() > bucket.capacity() / kBucketShrinkRatio)
        return;
    if (bucket.empty()) {
        std::vector<Entry>().swap(bucket);
        return;
    }
    try {
        std::vector<Entry>(bucket).swap(bucket);
    } catch (const std::bad_alloc&) {
        // Shrinking is best effort. Indexes stay valid and expiry/cleanup must still complete
        // if the allocator cannot provide even the smaller replacement.
        return;
    }
}

void TimerWheel::insert(Entry entry) {
    const std::size_t bucket_index = entry.due_tick % kBucketCount;
    auto& bucket = buckets_[bucket_index];
    const auto location = locations_.emplace(
        entry.expiry.session_id, Location{.bucket = bucket_index, .index = bucket.size()}).first;
    try {
        bucket.push_back(entry);
    } catch (...) {
        locations_.erase(location);
        throw;
    }
}

std::uint64_t TimerWheel::tick_at_or_after(Clock::time_point time) {
    const auto whole_seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
    std::uint64_t tick = static_cast<std::uint64_t>(whole_seconds.time_since_epoch().count());
    if (whole_seconds < time)
        ++tick;
    return tick;
}

std::uint64_t TimerWheel::tick_floor(Clock::time_point time) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count());
}

} // namespace erikslund::net
