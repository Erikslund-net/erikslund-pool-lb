#include "net/deadline_queue.hpp"

namespace erikslund::net {

DeadlineQueue::DeadlineQueue(std::size_t expected_entries) {
    entries_.reserve(expected_entries);
    locations_.reserve(expected_entries);
}

bool DeadlineQueue::earlier(const Entry& left, const Entry& right) noexcept {
    return std::pair(left.deadline, left.expiry.session_id) <
           std::pair(right.deadline, right.expiry.session_id);
}

void DeadlineQueue::swap_entries(std::size_t left, std::size_t right) noexcept {
    std::swap(entries_[left], entries_[right]);
    locations_.find(entries_[left].expiry.session_id)->second = left;
    locations_.find(entries_[right].expiry.session_id)->second = right;
}

void DeadlineQueue::restore_order(std::size_t index) noexcept {
    if (index > 0 && earlier(entries_[index], entries_[(index - 1) / 2])) {
        while (index > 0) {
            const std::size_t parent = (index - 1) / 2;
            if (!earlier(entries_[index], entries_[parent]))
                break;
            swap_entries(index, parent);
            index = parent;
        }
        return;
    }
    while (index < entries_.size() / 2) {
        const std::size_t left = index * 2 + 1;
        const std::size_t right = left + 1;
        const std::size_t child = right < entries_.size() && earlier(entries_[right], entries_[left])
                                      ? right : left;
        if (!earlier(entries_[child], entries_[index]))
            break;
        swap_entries(index, child);
        index = child;
    }
}

void DeadlineQueue::schedule(std::uint64_t session_id, std::uint64_t generation,
                             Clock::time_point deadline) {
    const Entry entry{.expiry = {.session_id = session_id, .generation = generation},
                      .deadline = deadline};
    if (const auto existing = locations_.find(session_id); existing != locations_.end()) {
        const std::size_t index = existing->second;
        entries_[index] = entry;
        restore_order(index);
        return;
    }
    const auto [location, inserted] = locations_.emplace(session_id, entries_.size());
    try {
        entries_.push_back(entry);
    } catch (...) {
        locations_.erase(location);
        throw;
    }
    restore_order(entries_.size() - 1);
}

void DeadlineQueue::cancel(std::uint64_t session_id) {
    const auto location = locations_.find(session_id);
    if (location == locations_.end())
        return;
    const std::size_t index = location->second;
    if (index != entries_.size() - 1)
        swap_entries(index, entries_.size() - 1);
    entries_.pop_back();
    locations_.erase(location);
    if (index < entries_.size())
        restore_order(index);
}

std::vector<TimerExpiry> DeadlineQueue::advance(Clock::time_point now) {
    std::vector<TimerExpiry> expired;
    while (!entries_.empty() && entries_.front().deadline <= now) {
        const TimerExpiry expiry = entries_.front().expiry;
        expired.push_back(expiry);
        cancel(expiry.session_id);
    }
    return expired;
}

std::optional<DeadlineQueue::Clock::time_point> DeadlineQueue::next_deadline() const noexcept {
    if (entries_.empty())
        return std::nullopt;
    return entries_.front().deadline;
}

} // namespace erikslund::net
