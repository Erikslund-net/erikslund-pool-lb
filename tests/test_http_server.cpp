#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "api/http_server.hpp"
#include "core/config.hpp"
#include "core/service_state.hpp"
#include "routing/router.hpp"
#include "socket_test_utils.hpp"

namespace {

std::string http_request(std::uint16_t port, std::string_view request) {
    auto client = erikslund::test::connect_loopback(port);
    if (!client || !erikslund::test::send_all(client.get(), request))
        return {};
    return erikslund::test::read_to_close(client.get());
}

} // namespace

TEST_CASE("HTTP server reports readiness and backend attempt/error counters") {
    const std::uint16_t port = erikslund::test::unused_loopback_port();
    REQUIRE(port != 0);
    erikslund::core::Config config;
    config.listeners = {{.name = "unused",
                         .address = "127.0.0.1:3333",
                         .tls = false,
                         .certificate_file = {},
                         .private_key_file = {}}};
    config.pools = {{.name = "primary",
                     .backends = {{.name = "backend",
                                   .address = "127.0.0.1:13333",
                                   .health_address = {},
                                   .send_proxy_v2 = false}}}};
    config.active_pool = "primary";
    config.api_address = "127.0.0.1:" + std::to_string(port);

    erikslund::core::ServiceState state;
    auto routing = erikslund::routing::make_routing_table(config);
    auto backend = routing->pools.front().backends.front();
    backend->healthy.store(true);
    backend->metrics->connection_attempts.store(25);
    backend->metrics->connection_errors.store(2);
    state.publish(std::make_shared<const erikslund::core::Config>(config), routing);
    state.stats.initialize_workers(3);
    state.stats.io_workers.store(3);
    state.stats.worker(0).record_timeout_batch(4, std::chrono::milliseconds(1'500));
    state.stats.worker(1).record_timeout_batch(6, std::chrono::milliseconds(500));
    state.stats.tls_reload_successes.store(4);
    state.stats.tls_reload_failures.store(1);
    const std::int64_t certificate_expiry =
        std::chrono::duration_cast<std::chrono::seconds>(
            (std::chrono::system_clock::now() + std::chrono::days(5)).time_since_epoch())
            .count();
    state.tls_certificates.store(
        std::make_shared<const erikslund::core::TlsCertificateStatuses>(
            erikslund::core::TlsCertificateStatuses{{
                .listener_name = "sv1-tls",
                .expiry_timestamp_seconds = certificate_expiry,
            }}));

    erikslund::api::HttpServer server(config, state);
    std::jthread thread([&server](const std::stop_token& token) { server.run(token); });

    const std::string health = http_request(
        port, "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(health.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(health.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos);
    CHECK(health.ends_with("ok\n"));

    const std::string head = http_request(
        port, "HEAD /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(head.find("Content-Length: 3\r\n") != std::string::npos);
    CHECK(head.ends_with("\r\n\r\n"));

    const std::string metrics = http_request(
        port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(metrics.find("pool_lb_io_workers 3\n") != std::string::npos);
    CHECK(metrics.find("pool_lb_reactor_timeouts_processed_total 10\n") != std::string::npos);
    CHECK(metrics.find("pool_lb_reactor_timeout_batches_total 2\n") != std::string::npos);
    CHECK(metrics.find("pool_lb_reactor_timeout_processing_seconds_total 2\n") !=
          std::string::npos);
    CHECK(metrics.find("pool_lb_reactor_timeout_processing_seconds_max 1.5\n") !=
          std::string::npos);
    CHECK(metrics.find("pool_lb_worker_timeouts_processed_total{worker=\"0\"} 4\n") !=
          std::string::npos);
    CHECK(metrics.find("pool_lb_worker_timeout_processing_seconds_max{worker=\"1\"} 0.5\n") !=
          std::string::npos);
    CHECK(metrics.find("# HELP pool_lb_queued_bytes_limit Current process-wide queued-byte "
                       "limit.\n# TYPE pool_lb_queued_bytes_limit gauge\n"
                       "pool_lb_queued_bytes_limit " +
                       std::to_string(config.limits.max_queued_bytes) + "\n") !=
          std::string::npos);
    CHECK(metrics.find("pool_lb_tls_certificate_reloads_total{result=\"success\"} 4\n") !=
          std::string::npos);
    CHECK(metrics.find("pool_lb_tls_certificate_reloads_total{result=\"failure\"} 1\n") !=
          std::string::npos);
    CHECK(metrics.find(
              "pool_lb_tls_certificate_expiry_timestamp_seconds{listener=\"sv1-tls\"} " +
              std::to_string(certificate_expiry) + "\n") != std::string::npos);
    CHECK(metrics.find("pool_lb_backend_connection_attempts_total{pool=\"primary\","
                       "backend=\"backend\",generation=\"1\"} 25\n") != std::string::npos);
    CHECK(metrics.find("pool_lb_backend_connection_errors_total{pool=\"primary\","
                       "backend=\"backend\",generation=\"1\"} 2\n") != std::string::npos);

    const std::string status = http_request(
        port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(status.find("TLS CERTIFICATE EXPIRING") != std::string::npos);
    CHECK(status.find("TLS sv1-tls certificate</td><td class=\"warn\">expires ") !=
          std::string::npos);
    CHECK(status.find("queued bytes</td><td>0 / " +
                      std::to_string(config.limits.max_queued_bytes) +
                      " (high water 0)</td>") != std::string::npos);

    const std::int64_t expired_certificate =
        std::chrono::duration_cast<std::chrono::seconds>(
            (std::chrono::system_clock::now() - std::chrono::days(1)).time_since_epoch())
            .count();
    state.tls_certificates.store(
        std::make_shared<const erikslund::core::TlsCertificateStatuses>(
            erikslund::core::TlsCertificateStatuses{{
                .listener_name = "sv1-tls",
                .expiry_timestamp_seconds = expired_certificate,
            }}));
    const std::string expired_status = http_request(
        port, "GET /status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(expired_status.find("TLS CERTIFICATE EXPIRED") != std::string::npos);
    CHECK(expired_status.find("TLS sv1-tls certificate</td><td class=\"bad\">expired ") !=
          std::string::npos);

    const std::string method = http_request(
        port, "POST /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(method.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));

    backend->healthy.store(false);
    const std::string degraded = http_request(
        port, "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(degraded.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(degraded.ends_with("degraded\n"));
    thread.request_stop();
}

TEST_CASE("slow HTTP client does not block health checks") {
    const std::uint16_t port = erikslund::test::unused_loopback_port();
    REQUIRE(port != 0);
    erikslund::core::Config config;
    config.listeners = {{.name = "unused",
                         .address = "127.0.0.1:3333",
                         .tls = false,
                         .certificate_file = {},
                         .private_key_file = {}}};
    config.pools = {{.name = "primary",
                     .backends = {{.name = "backend",
                                   .address = "127.0.0.1:13333",
                                   .health_address = {}}}}};
    config.active_pool = "primary";
    config.api_address = "127.0.0.1:" + std::to_string(port);

    erikslund::core::ServiceState state;
    auto routing = erikslund::routing::make_routing_table(config);
    routing->pools.front().backends.front()->healthy.store(true);
    state.publish(std::make_shared<const erikslund::core::Config>(config), routing);

    erikslund::api::HttpServer server(config, state);
    std::jthread thread([&server](const std::stop_token& token) { server.run(token); });

    auto slow_client = erikslund::test::connect_loopback(port);
    REQUIRE(slow_client);
    REQUIRE(erikslund::test::send_all(slow_client.get(), "GET /healthz"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto started = std::chrono::steady_clock::now();
    const std::string health = http_request(
        port, "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(health.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(elapsed < std::chrono::seconds(1));
    thread.request_stop();
}

TEST_CASE("HTTP backend metrics preserve counters and draining sessions through route reloads") {
    const std::uint16_t port = erikslund::test::unused_loopback_port();
    REQUIRE(port != 0);
    erikslund::core::Config config;
    config.pools = {{.name = "primary",
                     .backends = {{.name = "backend", .address = "127.0.0.1:13333",
                                   .health_address = {}},
                                  {.name = "spare", .address = "127.0.0.1:13334",
                                   .health_address = {}}}}};
    config.active_pool = "primary";
    config.api_address = "127.0.0.1:" + std::to_string(port);
    const auto original_routes = erikslund::routing::make_routing_table(config);
    const auto original = original_routes->pools.front().backends.front();
    original->healthy.store(true);
    original->active_connections.store(2);
    original->metrics->active_connections.store(2);
    original->metrics->connection_attempts.store(25);
    original->metrics->connection_errors.store(2);

    erikslund::core::ServiceState state;
    state.publish(std::make_shared<const erikslund::core::Config>(config), original_routes);
    erikslund::api::HttpServer server(config, state);
    std::jthread thread([&server](const std::stop_token& token) { server.run(token); });
    const auto get = [port](std::string_view path) {
        return http_request(port, "GET " + std::string(path) +
                                      " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    };

    config.pools.front().backends.front().address = "127.0.0.2:13333";
    const auto replacement_routes = erikslund::routing::make_routing_table(config, original_routes);
    const auto replacement = replacement_routes->pools.front().backends.front();
    replacement->healthy.store(true);
    state.publish(std::make_shared<const erikslund::core::Config>(config), replacement_routes);
    const std::string before = get("/metrics");
    CHECK(before.find("pool_lb_backend_connections_active{pool=\"primary\","
                      "backend=\"backend\"} 2\n") != std::string::npos);
    CHECK(before.find("pool_lb_backend_connection_attempts_total{pool=\"primary\","
                      "backend=\"backend\",generation=\"1\"} 25\n") != std::string::npos);
    CHECK(before.find("pool_lb_backend_connection_errors_total{pool=\"primary\","
                      "backend=\"backend\",generation=\"1\"} 2\n") != std::string::npos);
    CHECK(get("/").find("primary/backend</td><td class=\"ok\">up - 2 sessions") !=
          std::string::npos);

    replacement->active_connections.fetch_add(1);
    replacement->metrics->active_connections.fetch_add(1);
    replacement->metrics->connection_attempts.fetch_add(1);
    original->active_connections.fetch_sub(1);
    original->metrics->active_connections.fetch_sub(1);
    original->metrics->connection_errors.fetch_add(1);
    const std::string after = get("/metrics");
    CHECK(after.find("pool_lb_backend_connections_active{pool=\"primary\","
                     "backend=\"backend\"} 2\n") != std::string::npos);
    CHECK(after.find("pool_lb_backend_connection_attempts_total{pool=\"primary\","
                     "backend=\"backend\",generation=\"1\"} 26\n") != std::string::npos);
    CHECK(after.find("pool_lb_backend_connection_errors_total{pool=\"primary\","
                     "backend=\"backend\",generation=\"1\"} 3\n") != std::string::npos);

    const auto restored_config = config;
    config.pools.front().backends.erase(config.pools.front().backends.begin());
    const auto removed_routes = erikslund::routing::make_routing_table(config, replacement_routes);
    state.publish(std::make_shared<const erikslund::core::Config>(config), removed_routes);
    const std::string draining = get("/metrics");
    CHECK(draining.find("pool_lb_backend_up{pool=\"primary\",backend=\"backend\"}") ==
          std::string::npos);
    CHECK(draining.find("pool_lb_backend_draining{pool=\"primary\",backend=\"backend\"} 1\n") !=
          std::string::npos);
    CHECK(draining.find("pool_lb_backend_connections_active{pool=\"primary\","
                        "backend=\"backend\"} 2\n") != std::string::npos);
    CHECK(get("/").find("primary/backend</td><td class=\"warn\">draining - 2 sessions") !=
          std::string::npos);

    const auto restored_routes =
        erikslund::routing::make_routing_table(restored_config, removed_routes);
    state.publish(std::make_shared<const erikslund::core::Config>(restored_config), restored_routes);
    const std::string restored = get("/metrics");
    constexpr std::string_view kAttemptSample =
        "pool_lb_backend_connection_attempts_total{pool=\"primary\",backend=\"backend\","
        "generation=\"1\"} 26\n";
    REQUIRE(restored.find(kAttemptSample) != std::string::npos);
    CHECK(restored.find(kAttemptSample) == restored.rfind(kAttemptSample));
    thread.request_stop();
}
