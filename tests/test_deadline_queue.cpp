#include <doctest/doctest.h>

#include <chrono>
#include <map>
#include <random>

#include "net/deadline_queue.hpp"

using namespace std::chrono_literals;

TEST_CASE("connect deadlines expire before the remaining fallback budget is consumed") {
    using Clock = erikslund::net::DeadlineQueue::Clock;
    const auto start = Clock::time_point(100s + 600ms);
    erikslund::net::DeadlineQueue queue;
    queue.schedule(42, 1, start + 500ms);

    REQUIRE(queue.next_deadline() == start + 500ms);
    CHECK(queue.advance(start + 499ms).empty());
    const auto expired = queue.advance(start + 500ms);
    REQUIRE(expired.size() == 1);
    CHECK(expired.front().session_id == 42);
    CHECK(expired.front().generation == 1);
    CHECK(queue.size() == 0);
    CHECK_FALSE(queue.next_deadline().has_value());
}

TEST_CASE("connect deadlines replace and cancel entries without retaining stale generations") {
    using Clock = erikslund::net::DeadlineQueue::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::DeadlineQueue queue;
    queue.schedule(1, 1, start + 5ms);
    queue.schedule(1, 2, start + 10ms);
    queue.schedule(2, 1, start + 10ms);
    queue.schedule(3, 1, start + 1ms);
    queue.cancel(3);
    queue.cancel(3);

    CHECK(queue.size() == 2);
    CHECK(queue.advance(start + 9ms).empty());
    const auto expired = queue.advance(start + 10ms);
    REQUIRE(expired.size() == 2);
    CHECK(expired[0].session_id == 1);
    CHECK(expired[0].generation == 2);
    CHECK(expired[1].session_id == 2);
    CHECK(queue.size() == 0);
}

TEST_CASE("connect deadlines retain precise ordering across many subsecond retries") {
    using Clock = erikslund::net::DeadlineQueue::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::DeadlineQueue queue;
    for (std::uint64_t generation = 1; generation <= 64; ++generation) {
        const auto deadline = start + generation * 10ms;
        queue.schedule(1, generation, deadline);
        CHECK(queue.advance(deadline - 1ns).empty());
        const auto expired = queue.advance(deadline);
        REQUIRE(expired.size() == 1);
        CHECK(expired.front().generation == generation);
    }
}

TEST_CASE("connect deadline heap matches an ordered reference through mixed churn") {
    using Clock = erikslund::net::DeadlineQueue::Clock;
    const auto start = Clock::time_point(100s);
    constexpr std::uint64_t kSessionCount = 257;
    constexpr std::uint64_t kOperations = 20'000;
    erikslund::net::DeadlineQueue queue(kSessionCount);
    std::map<std::pair<Clock::time_point, std::uint64_t>, std::uint64_t> reference;
    std::map<std::uint64_t, Clock::time_point> deadlines;
    std::mt19937_64 random(42);
    for (std::uint64_t operation = 1; operation <= kOperations; ++operation) {
        const auto session_id = random() % kSessionCount;
        if (const auto prior = deadlines.find(session_id); prior != deadlines.end()) {
            reference.erase({prior->second, session_id});
            deadlines.erase(prior);
        }
        if (random() % 4 == 0) {
            queue.cancel(session_id);
        } else {
            const auto deadline = start + std::chrono::milliseconds(random() % 1'000);
            queue.schedule(session_id, operation, deadline);
            deadlines[session_id] = deadline;
            reference[{deadline, session_id}] = operation;
        }
        CHECK(queue.size() == reference.size());
        if (!reference.empty())
            CHECK(queue.next_deadline() == reference.begin()->first.first);
    }
    const auto expired = queue.advance(start + 1s);
    REQUIRE(expired.size() == reference.size());
    std::size_t index = 0;
    for (const auto& [key, generation] : reference) {
        CHECK(expired[index].session_id == key.second);
        CHECK(expired[index].generation == generation);
        ++index;
    }
    CHECK(queue.size() == 0);
}

TEST_CASE("connect deadline heap expires one hundred thousand pending connections") {
    using Clock = erikslund::net::DeadlineQueue::Clock;
    constexpr std::uint64_t kSessionCount = 100'000;
    const auto start = Clock::time_point(100s);
    erikslund::net::DeadlineQueue queue(kSessionCount);
    for (std::uint64_t session_id = kSessionCount; session_id > 0; --session_id)
        queue.schedule(session_id, 1, start + std::chrono::microseconds(session_id));
    CHECK(queue.advance(start).empty());
    const auto expired = queue.advance(start + 1s);
    REQUIRE(expired.size() == kSessionCount);
    for (std::size_t index = 0; index < expired.size(); ++index)
        CHECK(expired[index].session_id == index + 1);
}
