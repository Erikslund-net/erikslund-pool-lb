#include <doctest/doctest.h>

#include <arpa/inet.h>

#include "core/config.hpp"
#include "core/errors.hpp"
#include "net/endpoint.hpp"
#include "routing/router.hpp"

namespace {

erikslund::core::Config routing_config() {
    erikslund::core::Config config;
    config.listeners.push_back({.name = "sv1",
                                .address = "127.0.0.1:3333",
                                .tls = false,
                                .certificate_file = {},
                                .private_key_file = {}});
    config.pools = {
        {.name = "primary",
         .backends = {{.name = "primary-a",
                       .address = "127.0.0.1:13333",
                       .health_address = {}},
                      {.name = "primary-b",
                       .address = "127.0.0.1:13334",
                       .health_address = {}}}},
        {.name = "secondary",
         .backends = {{.name = "secondary-a",
                       .address = "127.0.0.1:23333",
                       .health_address = {}}}},
    };
    config.active_pool = "primary";
    return config;
}

} // namespace

TEST_CASE("router selects a healthy least-loaded backend in the active pool") {
    auto table = erikslund::routing::make_routing_table(routing_config());
    table->pools[0].backends[0]->healthy.store(true);
    table->pools[0].backends[1]->healthy.store(true);
    table->pools[0].backends[0]->active_connections.store(10);
    const auto selected = erikslund::routing::select_backend(*table, 0);
    REQUIRE(selected);
    CHECK(selected->name == "primary-b");
}

TEST_CASE("router fails over only when configured to do so") {
    auto config = routing_config();
    auto table = erikslund::routing::make_routing_table(config);
    table->pools[1].backends[0]->healthy.store(true);
    REQUIRE(erikslund::routing::select_backend(*table, 0));
    CHECK(erikslund::routing::select_backend(*table, 1)->name == "secondary-a");
    CHECK(erikslund::routing::has_routable_backend(*table));

    config.failover = false;
    table = erikslund::routing::make_routing_table(config);
    table->pools[1].backends[0]->healthy.store(true);
    CHECK_FALSE(erikslund::routing::select_backend(*table, 0));
    CHECK_FALSE(erikslund::routing::has_routable_backend(*table));
}

TEST_CASE("routing reload reuses unchanged backend health and session state") {
    auto config = routing_config();
    auto first = erikslund::routing::make_routing_table(config);
    auto original_backend = first->pools[0].backends[0];
    original_backend->healthy.store(true);
    original_backend->active_connections.store(7);

    config.active_pool = "secondary";
    const auto replacement = erikslund::routing::make_routing_table(config, first);
    CHECK(replacement->pools[0].backends[0] == original_backend);
    CHECK(replacement->pools[0].backends[0]->healthy.load());
    CHECK(replacement->pools[0].backends[0]->active_connections.load() == 7);
    CHECK(replacement->active_pool == "secondary");
}

TEST_CASE("routing reload replaces a backend whose address now resolves elsewhere") {
    const auto config = routing_config();
    const auto first = erikslund::routing::make_routing_table(config);
    const auto original_backend = first->pools[0].backends[0];
    original_backend->healthy.store(true);
    original_backend->active_connections.store(4);
    original_backend->metrics->active_connections.store(4);
    original_backend->metrics->connection_attempts.store(25);
    original_backend->metrics->connection_errors.store(2);
    // Stands in for a hostname that moved since the last resolution: the configured text is
    // unchanged, but the address behind it is not.
    original_backend->socket_addresses = erikslund::net::resolve_endpoints(
        erikslund::net::parse_endpoint("127.0.0.2:13333"));

    const auto replacement = erikslund::routing::make_routing_table(config, first);
    const auto reloaded = replacement->pools[0].backends[0];
    CHECK(reloaded != original_backend);
    REQUIRE(reloaded->socket_addresses.size() == 1);
    CHECK(erikslund::net::address_host(reloaded->socket_addresses.front().value) == "127.0.0.1");
    CHECK_FALSE(reloaded->healthy.load());
    CHECK(reloaded->active_connections.load() == 0);
    CHECK(reloaded->metrics == original_backend->metrics);
    CHECK(reloaded->metrics->active_connections.load() == 4);
    CHECK(reloaded->metrics->connection_attempts.load() == 25);
    CHECK(reloaded->metrics->connection_errors.load() == 2);
}

TEST_CASE("backend accounting survives configured address changes and draining sessions") {
    auto config = routing_config();
    const auto first = erikslund::routing::make_routing_table(config);
    const auto original = first->pools[0].backends[0];
    original->active_connections.store(4);
    original->metrics->active_connections.store(4);
    original->metrics->connection_attempts.store(25);
    original->metrics->connection_errors.store(2);

    config.pools[0].backends[0].address = "127.0.0.2:13333";
    const auto replacement = erikslund::routing::make_routing_table(config, first);
    const auto current = replacement->pools[0].backends[0];
    REQUIRE(current != original);
    REQUIRE(current->metrics == original->metrics);
    current->healthy.store(true);
    replacement->pools[0].backends[1]->healthy.store(true);
    replacement->pools[0].backends[1]->active_connections.store(1);
    CHECK(erikslund::routing::select_backend(*replacement, 0) == current);

    current->active_connections.fetch_add(1);
    current->metrics->active_connections.fetch_add(1);
    current->metrics->connection_attempts.fetch_add(1);
    original->active_connections.fetch_sub(1);
    original->metrics->active_connections.fetch_sub(1);
    original->metrics->connection_errors.fetch_add(1);
    CHECK(current->active_connections.load() == 1);
    CHECK(current->metrics->active_connections.load() == 4);
    CHECK(current->metrics->connection_attempts.load() == 26);
    CHECK(current->metrics->connection_errors.load() == 3);
    CHECK(replacement->backend_metrics.size() == first->backend_metrics.size());
}

TEST_CASE("removed backends retain one accounting identity while previous sessions drain") {
    auto config = routing_config();
    const auto first = erikslund::routing::make_routing_table(config);
    const auto original = first->pools[0].backends[0];
    original->metrics->active_connections.store(4);
    original->metrics->connection_attempts.store(25);
    config.pools[0].backends.erase(config.pools[0].backends.begin());

    const auto removed = erikslund::routing::make_routing_table(config, first);
    REQUIRE(removed->backend_metrics.size() == 3);
    CHECK(removed->backend_metrics.back().metrics.lock() == original->metrics);
    CHECK(removed->backend_metrics.back().current_backend.expired());

    const auto restored = erikslund::routing::make_routing_table(routing_config(), removed);
    REQUIRE(restored->backend_metrics.size() == 3);
    const auto current = restored->pools[0].backends[0];
    CHECK(current != original);
    CHECK(current->metrics == original->metrics);
    CHECK(current->metrics->active_connections.load() == 4);
    CHECK(current->metrics->connection_attempts.load() == 25);
}

TEST_CASE("removed accounting identities expire after their last routing generation drains") {
    auto config = routing_config();
    auto first = erikslund::routing::make_routing_table(config);
    const std::weak_ptr<erikslund::routing::BackendMetrics> original =
        first->pools[0].backends[0]->metrics;
    const auto original_generation = original.lock()->generation;
    config.pools[0].backends.erase(config.pools[0].backends.begin());
    const auto removed = erikslund::routing::make_routing_table(config, first);
    REQUIRE(removed->backend_metrics.size() == 3);
    first.reset();
    CHECK(original.expired());

    const auto refreshed = erikslund::routing::make_routing_table(config, removed);
    CHECK(refreshed->backend_metrics.size() == 2);
    const auto restored = erikslund::routing::make_routing_table(routing_config(), refreshed);
    CHECK(restored->backend_metrics.size() == 3);
    CHECK(restored->pools[0].backends[0]->metrics->active_connections.load() == 0);
    CHECK(restored->pools[0].backends[0]->metrics->connection_attempts.load() == 0);
    CHECK(restored->pools[0].backends[0]->metrics->generation > original_generation);
    const auto unchanged = erikslund::routing::make_routing_table(routing_config(), restored);
    CHECK(unchanged->next_backend_metrics_generation == restored->next_backend_metrics_generation);
}

TEST_CASE("routing reload keeps the last known address when a backend stops resolving") {
    auto config = routing_config();
    const auto first = erikslund::routing::make_routing_table(config);
    const auto original_backend = first->pools[0].backends[0];
    original_backend->healthy.store(true);
    original_backend->active_connections.store(3);

    // What a reload sees once DNS stops answering for a name that resolved on an earlier table.
    const std::string unresolvable = "backend.invalid:13333";
    original_backend->configured_address = unresolvable;
    config.pools[0].backends[0].address = unresolvable;
    config.active_pool = "secondary";

    const auto replacement = erikslund::routing::make_routing_table(config, first);
    const auto reloaded = replacement->pools[0].backends[0];
    CHECK(reloaded == original_backend);
    CHECK(reloaded->healthy.load());
    CHECK(reloaded->active_connections.load() == 3);
    // The rest of the reload still lands.
    CHECK(replacement->active_pool == "secondary");
}

TEST_CASE("routing reload is rejected when a backend has never resolved") {
    auto config = routing_config();
    const auto first = erikslund::routing::make_routing_table(config);
    config.pools[0].backends.push_back({.name = "primary-c",
                                        .address = "backend.invalid:13333",
                                        .health_address = {}});
    CHECK_THROWS_AS(erikslund::routing::make_routing_table(config, first),
                    erikslund::core::ConfigError);
}

TEST_CASE("routing reload replaces a backend when its health endpoint changes") {
    auto config = routing_config();
    config.pools[0].backends[0].health_address = "127.0.0.1:17777";
    const auto first = erikslund::routing::make_routing_table(config);
    const auto original_backend = first->pools[0].backends[0];

    config.pools[0].backends[0].health_address = "127.0.0.1:27777";
    const auto replacement = erikslund::routing::make_routing_table(config, first);
    CHECK(replacement->pools[0].backends[0] != original_backend);
}

TEST_CASE("routing resolves and retains configured upstream source addresses") {
    auto config = routing_config();
    config.upstream_source_addresses = {"127.0.0.2", "127.0.0.3"};

    const auto table = erikslund::routing::make_routing_table(config);
    REQUIRE(table->upstream_source_addresses.size() == 2);
    for (const auto& source : table->upstream_source_addresses) {
        REQUIRE(source.family == AF_INET);
        CHECK(ntohs(reinterpret_cast<const sockaddr_in&>(source.value).sin_port) == 0);
    }
}

TEST_CASE("routing requires a family-compatible source for every backend") {
    auto config = routing_config();
    config.pools.front().backends.front().address = "[::1]:13333";
    config.upstream_source_addresses = {"127.0.0.2"};
    CHECK_THROWS_AS(erikslund::routing::make_routing_table(config),
                    erikslund::core::ConfigError);

    config.upstream_source_addresses.push_back("::1");
    CHECK_NOTHROW(erikslund::routing::make_routing_table(config));
}
