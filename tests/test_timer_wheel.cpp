#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "net/timer_wheel.hpp"

using namespace std::chrono_literals;

TEST_CASE("timer wheel emits only deadlines reached by the current tick") {
    using Clock = erikslund::net::TimerWheel::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::TimerWheel wheel(start);
    wheel.schedule(42, 7, start + 2s + 200ms);

    CHECK(wheel.advance(start + 2s + 999ms).empty());
    const auto expired = wheel.advance(start + 3s);
    REQUIRE(expired.size() == 1);
    CHECK(expired.front().session_id == 42);
    CHECK(expired.front().generation == 7);
}

TEST_CASE("timer wheel carries long deadlines across ring rotations") {
    using Clock = erikslund::net::TimerWheel::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::TimerWheel wheel(start);
    const auto delay = std::chrono::seconds(erikslund::net::TimerWheel::kBucketCount + 2);
    wheel.schedule(9, 3, start + delay);

    CHECK(wheel.advance(start + delay - 1s).empty());
    const auto expired = wheel.advance(start + delay);
    REQUIRE(expired.size() == 1);
    CHECK(expired.front().session_id == 9);
    CHECK(expired.front().generation == 3);
}

TEST_CASE("timer generations let reactors ignore replaced deadlines") {
    using Clock = erikslund::net::TimerWheel::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::TimerWheel wheel(start);
    wheel.schedule(5, 1, start + 1s);
    wheel.schedule(5, 2, start + 2s);

    CHECK(wheel.size() == 1);
    CHECK(wheel.advance(start + 1s).empty());
    const auto second = wheel.advance(start + 2s);
    REQUIRE(second.size() == 1);
    CHECK(second.front().generation == 2);
    CHECK(wheel.size() == 0);
}

TEST_CASE("timer cancellation removes closed sessions immediately") {
    using Clock = erikslund::net::TimerWheel::Clock;
    const auto start = Clock::time_point(100s);
    erikslund::net::TimerWheel wheel(start);
    wheel.schedule(11, 1, start + 1h);
    REQUIRE(wheel.size() == 1);
    wheel.cancel(11);
    CHECK(wheel.size() == 0);
    CHECK(wheel.advance(start + 1h).empty());
}

TEST_CASE("timer wheel preserves cancellable long timers when compacting a mixed bucket") {
    using Wheel = erikslund::net::TimerWheel;
    const auto start = Wheel::Clock::time_point(100s);
    const auto rotation = std::chrono::seconds(Wheel::kBucketCount);
    Wheel wheel(start);
    wheel.schedule(1, 1, start + 1s);
    wheel.schedule(2, 1, start + rotation + 1s);
    wheel.schedule(3, 1, start + 1s);
    wheel.schedule(4, 1, start + rotation + 1s);
    const auto expired = wheel.advance(start + 1s);
    REQUIRE(expired.size() == 2);
    CHECK(expired[0].session_id == 1);
    CHECK(expired[1].session_id == 3);
    wheel.cancel(2);
    wheel.schedule(4, 2, start + 2 * rotation + 1s);
    CHECK(wheel.advance(start + rotation + 1s).empty());
    const auto last = wheel.advance(start + 2 * rotation + 1s);
    REQUIRE(last.size() == 1);
    CHECK(last.front().session_id == 4);
    CHECK(last.front().generation == 2);
}

TEST_CASE("timer wheel catches up across a long suspension without walking every elapsed tick") {
    using Wheel = erikslund::net::TimerWheel;
    const auto start = Wheel::Clock::time_point(100s);
    Wheel wheel(start);
    wheel.schedule(1, 1, start + 1s);
    wheel.schedule(2, 1, start + 100'000h);
    REQUIRE(wheel.advance(start + 99'999h).size() == 1);
    CHECK(wheel.size() == 1);
    wheel.cancel(2);
    wheel.schedule(3, 1, start + 100'001h);
    REQUIRE(wheel.advance(start + 100'002h).size() == 1);
    CHECK(wheel.size() == 0);
}

TEST_CASE("timer bucket shrink preserves the index of a surviving long deadline") {
    using Wheel = erikslund::net::TimerWheel;
    const auto start = Wheel::Clock::time_point(100s);
    constexpr std::uint64_t kShortTimers = 256;
    Wheel wheel(start);
    for (std::uint64_t session_id = 1; session_id <= kShortTimers; ++session_id)
        wheel.schedule(session_id, 1, start + 1s);
    wheel.schedule(kShortTimers + 1, 1,
                   start + std::chrono::seconds(Wheel::kBucketCount) + 1s);
    REQUIRE(wheel.advance(start + 1s).size() == kShortTimers);
    CHECK(wheel.size() == 1);
    wheel.cancel(kShortTimers + 1);
    CHECK(wheel.size() == 0);
    wheel.schedule(kShortTimers + 1, 2, start + 2s);
    const auto expired = wheel.advance(start + 2s);
    REQUIRE(expired.size() == 1);
    CHECK(expired.front().generation == 2);
}

TEST_CASE("timer wheel expires a clustered one hundred thousand session population") {
    using Clock = erikslund::net::TimerWheel::Clock;
    constexpr std::size_t kSessionCount = 100'000;
    const auto start = Clock::time_point(100s);
    erikslund::net::TimerWheel wheel(start, kSessionCount);
    for (std::uint64_t session_id = 1; session_id <= kSessionCount; ++session_id)
        wheel.schedule(session_id, 7, start + 5s);

    REQUIRE(wheel.size() == kSessionCount);
    CHECK(wheel.advance(start + 4s).empty());
    const auto expired = wheel.advance(start + 5s);
    REQUIRE(expired.size() == kSessionCount);
    CHECK(expired.front().session_id == 1);
    CHECK(expired.back().session_id == kSessionCount);
    CHECK(expired.front().generation == 7);
    CHECK(wheel.size() == 0);
}
