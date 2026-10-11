#include <doctest/doctest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>

#include "net/endpoint.hpp"
#include "net/rate_limiter.hpp"

using erikslund::net::AdmissionResult;
using erikslund::net::RateLimiter;

namespace {

erikslund::core::LimitsConfig test_limits() {
    erikslund::core::LimitsConfig limits;
    limits.max_connections_per_ip = 1;
    limits.connections_per_second_per_ip = 100.0;
    limits.connection_burst_per_ip = 10;
    limits.global_connections_per_second = 1'000.0;
    limits.global_connection_burst = 1'000;
    limits.max_tracked_client_ips = 100;
    limits.messages_per_second_per_ip = 0.001;
    limits.message_burst_per_ip = 1;
    limits.bytes_per_second_per_ip = 1.0;
    limits.byte_burst_per_ip = 64;
    return limits;
}

erikslund::net::ClientKey client_key(std::uint8_t final_octet) {
    sockaddr_storage address{};
    auto& ipv4 = reinterpret_cast<sockaddr_in&>(address);
    ipv4.sin_family = AF_INET;
    ipv4.sin_addr.s_addr = htonl(0xc0000200U | final_octet);
    return erikslund::net::client_rate_limit_key(address, 64);
}

} // namespace

TEST_CASE("rate limiter enforces and releases the concurrent IP cap") {
    const auto limits = test_limits();
    RateLimiter limiter(limits);
    auto first = limiter.acquire(client_key(1), limits);
    CHECK(first.result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::ConcurrentLimit);
    first.lease.reset();
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::Accepted);
}

TEST_CASE("rate limiter isolates token buckets by client IP") {
    auto limits = test_limits();
    limits.max_connections_per_ip = 10;
    limits.connections_per_second_per_ip = 0.001;
    limits.connection_burst_per_ip = 1;
    RateLimiter limiter(limits);
    auto first = limiter.acquire(client_key(1), limits);
    CHECK(first.result == AdmissionResult::Accepted);
    first.lease.reset();
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::RateLimit);
    CHECK(limiter.acquire(client_key(2), limits).result == AdmissionResult::Accepted);
}

TEST_CASE("rate limiter enforces a process-wide admission budget before source allocation") {
    auto limits = test_limits();
    limits.global_connections_per_second = 0.001;
    limits.global_connection_burst = 1;
    RateLimiter limiter(limits);
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(client_key(2), limits).result == AdmissionResult::GlobalRateLimit);
    CHECK(limiter.tracked_clients() == 1);
}

TEST_CASE("source rejection does not drain the process-wide admission budget") {
    auto limits = test_limits();
    limits.max_connections_per_ip = 1;
    limits.global_connections_per_second = 0.001;
    limits.global_connection_burst = 2;
    RateLimiter limiter(limits);
    auto first = limiter.acquire(client_key(1), limits);
    REQUIRE(first.result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::ConcurrentLimit);
    CHECK(limiter.acquire(client_key(2), limits).result == AdmissionResult::Accepted);
}

TEST_CASE("rate limiter bounds tracked source state") {
    auto limits = test_limits();
    limits.max_tracked_client_ips = 1;
    RateLimiter limiter(limits);
    auto first = limiter.acquire(client_key(1), limits);
    CHECK(first.result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(client_key(2), limits).result == AdmissionResult::SourceCapacity);
    CHECK(limiter.tracked_clients() == 1);
}

TEST_CASE("rate limiter applies ongoing message and byte budgets across sessions") {
    auto limits = test_limits();
    RateLimiter limiter(limits);
    auto admission = limiter.acquire(client_key(1), limits);
    REQUIRE(admission.result == AdmissionResult::Accepted);
    CHECK(limiter.consume_traffic(admission.lease, 32, 1, RateLimiter::Clock::now()));
    CHECK_FALSE(
        limiter.consume_traffic(admission.lease, 1, 1, RateLimiter::Clock::now()));
    CHECK_FALSE(
        limiter.consume_traffic(admission.lease, 33, 0, RateLimiter::Clock::now()));
}

TEST_CASE("admission leases remain valid while the source table rehashes") {
    auto limits = test_limits();
    limits.max_connections_per_ip = 512;
    limits.max_tracked_client_ips = 512;
    limits.global_connection_burst = 512;
    limits.connection_burst_per_ip = 512;
    RateLimiter limiter(limits);
    auto retained = limiter.acquire(client_key(1), limits);
    REQUIRE(retained.result == AdmissionResult::Accepted);
    for (std::uint16_t index = 2; index < 250; ++index) {
        auto transient = limiter.acquire(client_key(static_cast<std::uint8_t>(index)), limits);
        CHECK(transient.result == AdmissionResult::Accepted);
    }
    CHECK(limiter.consume_traffic(retained.lease, 1, 0, RateLimiter::Clock::now()));
}

TEST_CASE("moved admission leases release their source slot exactly once") {
    const auto limits = test_limits();
    RateLimiter limiter(limits);
    {
        auto admission = limiter.acquire(client_key(1), limits);
        REQUIRE(admission.result == AdmissionResult::Accepted);
        RateLimiter::Lease retained = std::move(admission.lease);
        CHECK(retained);
        CHECK_FALSE(admission.lease);
        CHECK(limiter.acquire(client_key(1), limits).result ==
              AdmissionResult::ConcurrentLimit);
    }
    CHECK(limiter.acquire(client_key(1), limits).result == AdmissionResult::Accepted);
}

TEST_CASE("active admission state is never evicted at source capacity") {
    constexpr std::size_t kRateLimiterShardCount = 64;
    auto limits = test_limits();
    limits.max_tracked_client_ips = 1;
    RateLimiter limiter(limits);
    const auto retained_key = client_key(1);
    const auto replacement_key = client_key(65);
    REQUIRE(erikslund::net::ClientKeyHash{}(retained_key) % kRateLimiterShardCount ==
            erikslund::net::ClientKeyHash{}(replacement_key) % kRateLimiterShardCount);
    auto retained = limiter.acquire(retained_key, limits);
    REQUIRE(retained.result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(replacement_key, limits).result ==
          AdmissionResult::SourceCapacity);
    retained.lease.reset();
    CHECK(limiter.acquire(replacement_key, limits).result == AdmissionResult::Accepted);
}

TEST_CASE("traffic accounting rejects a lease owned by another limiter") {
    const auto limits = test_limits();
    RateLimiter owner(limits);
    RateLimiter unrelated(limits);
    auto admission = owner.acquire(client_key(1), limits);
    REQUIRE(admission.result == AdmissionResult::Accepted);
    CHECK_FALSE(unrelated.consume_traffic(
        admission.lease, 1, 0, RateLimiter::Clock::now()));
    CHECK(owner.consume_traffic(
        admission.lease, 1, 0, RateLimiter::Clock::now()));
}

TEST_CASE("old and new sessions share the current lowered IP traffic policy") {
    using namespace std::chrono_literals;
    auto original = test_limits();
    original.max_connections_per_ip = 2;
    original.messages_per_second_per_ip = 10.0;
    original.message_burst_per_ip = 10;
    original.bytes_per_second_per_ip = 100.0;
    original.byte_burst_per_ip = 100;
    RateLimiter limiter(original);
    auto old_session = limiter.acquire(client_key(1), original);
    REQUIRE(old_session.result == AdmissionResult::Accepted);
    const auto start = RateLimiter::Clock::now();
    REQUIRE(limiter.consume_traffic(old_session.lease, 100, 10, start));

    auto replacement = original;
    replacement.messages_per_second_per_ip = 1.0;
    replacement.message_burst_per_ip = 2;
    replacement.bytes_per_second_per_ip = 10.0;
    replacement.byte_burst_per_ip = 20;
    const auto changed_at = start + 5s;
    limiter.update_traffic_limits(replacement, changed_at);
    auto new_session = limiter.acquire(client_key(1), original);
    REQUIRE(new_session.result == AdmissionResult::Accepted);
    CHECK(limiter.consume_traffic(old_session.lease, 10, 1, changed_at));
    CHECK(limiter.consume_traffic(new_session.lease, 10, 1, changed_at));
    CHECK_FALSE(limiter.consume_traffic(old_session.lease, 0, 1, changed_at));
    CHECK_FALSE(limiter.consume_traffic(new_session.lease, 1, 0, changed_at));
    CHECK(limiter.consume_traffic(old_session.lease, 10, 1, changed_at + 1s));
    CHECK_FALSE(limiter.consume_traffic(new_session.lease, 0, 1, changed_at + 1s));
    CHECK_FALSE(limiter.consume_traffic(old_session.lease, 1, 0, changed_at + 1s));
}

TEST_CASE("an increased IP traffic rate does not refill elapsed time at the new rate") {
    using namespace std::chrono_literals;
    auto original = test_limits();
    original.messages_per_second_per_ip = 1.0;
    original.message_burst_per_ip = 10;
    original.bytes_per_second_per_ip = 10.0;
    original.byte_burst_per_ip = 100;
    RateLimiter limiter(original);
    auto session = limiter.acquire(client_key(1), original);
    REQUIRE(session.result == AdmissionResult::Accepted);
    const auto start = RateLimiter::Clock::now();
    REQUIRE(limiter.consume_traffic(session.lease, 100, 10, start));

    auto replacement = original;
    replacement.messages_per_second_per_ip = 1'000.0;
    replacement.bytes_per_second_per_ip = 1'000.0;
    limiter.update_traffic_limits(replacement, start + 100ms);
    CHECK_FALSE(limiter.consume_traffic(session.lease, 0, 1, start + 100ms));
    CHECK_FALSE(limiter.consume_traffic(session.lease, 2, 0, start + 100ms));
    CHECK(limiter.consume_traffic(session.lease, 1, 0, start + 100ms));
    CHECK_FALSE(limiter.consume_traffic(session.lease, 1, 0, start + 100ms));
    CHECK(limiter.consume_traffic(session.lease, 1, 1, start + 101ms));
}

TEST_CASE("capacity reclamation recovers inactive sources from unrelated shards") {
    constexpr std::size_t kRateLimiterShardCount = 64;
    auto limits = test_limits();
    limits.max_tracked_client_ips = 2;
    RateLimiter limiter(limits);
    REQUIRE(erikslund::net::ClientKeyHash{}(client_key(1)) % kRateLimiterShardCount !=
            erikslund::net::ClientKeyHash{}(client_key(3)) % kRateLimiterShardCount);
    auto inactive = limiter.acquire(client_key(1), limits);
    auto active = limiter.acquire(client_key(2), limits);
    REQUIRE(inactive.result == AdmissionResult::Accepted);
    REQUIRE(active.result == AdmissionResult::Accepted);
    inactive.lease.reset();
    auto newcomer = limiter.acquire(client_key(3), limits);
    REQUIRE(newcomer.result == AdmissionResult::Accepted);
    CHECK(limiter.tracked_clients() == 2);
    CHECK(limiter.consume_traffic(active.lease, 1, 0, RateLimiter::Clock::now()));
    CHECK(limiter.acquire(client_key(4), limits).result == AdmissionResult::SourceCapacity);
}

TEST_CASE("a lowered tracking cap reclaims multiple inactive shards and preserves active leases") {
    auto limits = test_limits();
    limits.max_tracked_client_ips = 4;
    RateLimiter limiter(limits);
    auto retained = limiter.acquire(client_key(1), limits);
    REQUIRE(retained.result == AdmissionResult::Accepted);
    for (std::uint8_t octet = 2; octet <= 4; ++octet)
        REQUIRE(limiter.acquire(client_key(octet), limits).result == AdmissionResult::Accepted);
    REQUIRE(limiter.tracked_clients() == 4);
    limits.max_tracked_client_ips = 2;
    auto newcomer = limiter.acquire(client_key(5), limits);
    REQUIRE(newcomer.result == AdmissionResult::Accepted);
    CHECK(limiter.tracked_clients() == 2);
    CHECK(limiter.consume_traffic(retained.lease, 1, 0, RateLimiter::Clock::now()));
}

TEST_CASE("concurrent source churn reclaims inactive nodes without displacing active leases") {
    constexpr std::size_t kWorkerCount = 8;
    constexpr std::size_t kCyclesPerWorker = 200;
    auto limits = test_limits();
    limits.max_tracked_client_ips = kWorkerCount + 1;
    limits.connection_burst_per_ip = 10'000;
    limits.global_connection_burst = 10'000;
    RateLimiter limiter(limits);
    auto retained = limiter.acquire(client_key(1), limits);
    REQUIRE(retained.result == AdmissionResult::Accepted);
    std::barrier start(static_cast<std::ptrdiff_t>(kWorkerCount));
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> workers;
    for (std::size_t worker_index = 0; worker_index < kWorkerCount; ++worker_index) {
        workers.emplace_back([&, worker_index] {
            start.arrive_and_wait();
            for (std::size_t cycle = 0; cycle < kCyclesPerWorker; ++cycle) {
                const auto octet = static_cast<std::uint8_t>(
                    2 + worker_index * 2 + cycle % 2);
                auto admission = limiter.acquire(client_key(octet), limits);
                if (admission.result != AdmissionResult::Accepted ||
                    !limiter.consume_traffic(admission.lease, 0, 0, RateLimiter::Clock::now()) ||
                    limiter.tracked_clients() > limits.max_tracked_client_ips)
                    failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    workers.clear();
    CHECK(failures.load() == 0);
    CHECK(limiter.consume_traffic(retained.lease, 1, 0, RateLimiter::Clock::now()));
}

TEST_CASE("concurrent established leases cannot reinstall a superseded IP traffic policy") {
    constexpr std::size_t kWorkerCount = 8;
    constexpr std::size_t kConsumptionsPerWorker = 100;
    auto limits = test_limits();
    limits.max_connections_per_ip = kWorkerCount;
    limits.message_burst_per_ip = kWorkerCount * kConsumptionsPerWorker;
    limits.byte_burst_per_ip = kWorkerCount * kConsumptionsPerWorker;
    RateLimiter limiter(limits);
    std::vector<RateLimiter::Admission> sessions;
    for (std::size_t worker_index = 0; worker_index < kWorkerCount; ++worker_index) {
        sessions.push_back(limiter.acquire(client_key(1), limits));
        REQUIRE(sessions.back().result == AdmissionResult::Accepted);
    }
    const auto now = RateLimiter::Clock::now();
    auto replacement = limits;
    replacement.message_burst_per_ip = 1;
    replacement.byte_burst_per_ip = 1;
    std::barrier start(static_cast<std::ptrdiff_t>(kWorkerCount + 1));
    std::barrier published(static_cast<std::ptrdiff_t>(kWorkerCount + 1));
    std::atomic<std::size_t> post_reload_accepted{0};
    std::vector<std::jthread> workers;
    for (std::size_t worker_index = 0; worker_index < kWorkerCount; ++worker_index) {
        workers.emplace_back([&, worker_index] {
            start.arrive_and_wait();
            for (std::size_t attempt = 0; attempt < kConsumptionsPerWorker; ++attempt)
                limiter.consume_traffic(sessions[worker_index].lease, 0, 0, now);
            published.arrive_and_wait();
            if (limiter.consume_traffic(sessions[worker_index].lease, 1, 1, now))
                post_reload_accepted.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start.arrive_and_wait();
    limiter.update_traffic_limits(replacement, now);
    published.arrive_and_wait();
    workers.clear();
    CHECK(post_reload_accepted.load() == 1);
}

TEST_CASE("unchanged traffic policy reloads preserve exhausted balances and refill rates") {
    using namespace std::chrono_literals;
    auto limits = test_limits();
    limits.messages_per_second_per_ip = 1.0;
    limits.message_burst_per_ip = 10;
    limits.bytes_per_second_per_ip = 10.0;
    limits.byte_burst_per_ip = 100;
    RateLimiter limiter(limits);
    auto session = limiter.acquire(client_key(1), limits);
    REQUIRE(session.result == AdmissionResult::Accepted);
    const auto start = RateLimiter::Clock::now();
    REQUIRE(limiter.consume_traffic(session.lease, 100, 10, start));
    // Other limits do not change the shared traffic policy or restore its token balances.
    ++limits.max_connections_per_ip;
    limiter.update_traffic_limits(limits, start + 500ms);
    CHECK_FALSE(limiter.consume_traffic(session.lease, 10, 1, start + 500ms));
    CHECK(limiter.consume_traffic(session.lease, 10, 1, start + 1s));
    CHECK_FALSE(limiter.consume_traffic(session.lease, 1, 0, start + 1s));
    CHECK_FALSE(limiter.consume_traffic(session.lease, 0, 1, start + 1s));
}

TEST_CASE("large tracking cap reductions reclaim bounded batches without evicting active leases") {
    constexpr std::size_t kOriginalSources = 200;
    auto limits = test_limits();
    limits.max_tracked_client_ips = kOriginalSources;
    RateLimiter limiter(limits);
    auto retained = limiter.acquire(client_key(1), limits);
    REQUIRE(retained.result == AdmissionResult::Accepted);
    for (std::size_t index = 2; index <= kOriginalSources; ++index)
        REQUIRE(limiter.acquire(client_key(static_cast<std::uint8_t>(index)), limits).result ==
                AdmissionResult::Accepted);
    REQUIRE(limiter.tracked_clients() == kOriginalSources);
    limits.max_tracked_client_ips = 2;
    CHECK(limiter.acquire(client_key(201), limits).result == AdmissionResult::SourceCapacity);
    CHECK(limiter.tracked_clients() > limits.max_tracked_client_ips);
    CHECK(limiter.tracked_clients() < kOriginalSources);
    RateLimiter::Admission newcomer;
    constexpr std::size_t kMaximumAttempts = 8;
    for (std::size_t attempt = 0; attempt < kMaximumAttempts && !newcomer.lease; ++attempt)
        newcomer = limiter.acquire(client_key(201), limits);
    REQUIRE(newcomer.result == AdmissionResult::Accepted);
    CHECK(limiter.tracked_clients() == 2);
    CHECK(limiter.consume_traffic(retained.lease, 1, 0, RateLimiter::Clock::now()));
}

TEST_CASE("reactivated sources clear reclamation hints and remain protected at capacity") {
    auto limits = test_limits();
    limits.max_tracked_client_ips = 2;
    limits.max_connections_per_ip = 2;
    RateLimiter limiter(limits);
    auto first = limiter.acquire(client_key(1), limits);
    auto second = limiter.acquire(client_key(65), limits);
    REQUIRE(first.result == AdmissionResult::Accepted);
    REQUIRE(second.result == AdmissionResult::Accepted);
    first.lease.reset();
    second.lease.reset();
    first = limiter.acquire(client_key(1), limits);
    second = limiter.acquire(client_key(65), limits);
    REQUIRE(first.result == AdmissionResult::Accepted);
    REQUIRE(second.result == AdmissionResult::Accepted);
    CHECK(limiter.acquire(client_key(2), limits).result == AdmissionResult::SourceCapacity);
    second.lease.reset();
    auto newcomer = limiter.acquire(client_key(2), limits);
    REQUIRE(newcomer.result == AdmissionResult::Accepted);
    CHECK(limiter.consume_traffic(first.lease, 1, 0, RateLimiter::Clock::now()));
}
