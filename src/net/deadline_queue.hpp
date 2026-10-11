#pragma once
// Exact cancellable deadlines for pending connects, separate from coarse session idle timers.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/timer_wheel.hpp"

namespace erikslund::net {

class DeadlineQueue {
public:
    using Clock = std::chrono::steady_clock;

    explicit DeadlineQueue(std::size_t expected_entries = 0);

    void schedule(std::uint64_t session_id, std::uint64_t generation,
                  Clock::time_point deadline);
    void cancel(std::uint64_t session_id);
    std::vector<TimerExpiry> advance(Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> next_deadline() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

private:
    struct Entry {
        TimerExpiry expiry;
        Clock::time_point deadline;
    };

    static bool earlier(const Entry& left, const Entry& right) noexcept;
    void swap_entries(std::size_t left, std::size_t right) noexcept;
    void restore_order(std::size_t index) noexcept;

    std::vector<Entry> entries_;
    std::unordered_map<std::uint64_t, std::size_t> locations_;
};

} // namespace erikslund::net
