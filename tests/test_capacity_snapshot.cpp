#include <doctest/doctest.h>

#include <limits>

#include <unistd.h>

#include <glaze/glaze.hpp>

#include "core/capacity_snapshot.hpp"
#include "core/errors.hpp"

TEST_CASE("capacity snapshots read target process metadata without starting the service") {
    const auto snapshot = erikslund::core::read_capacity_snapshot(::getpid());
    CHECK(snapshot.target_pid == ::getpid());
    CHECK(snapshot.open_file_descriptors.has_value());
    CHECK(snapshot.files.contains("limits"));
    CHECK(snapshot.files.contains("stat"));
    CHECK(snapshot.files.contains("status"));
    CHECK(snapshot.files.at("limits").contains("Max open files"));
    CHECK(snapshot.files.at("status").contains("VmRSS:"));
    erikslund::core::CapacitySnapshot decoded;
    const auto json = erikslund::core::capacity_snapshot_json(::getpid());
    REQUIRE_FALSE(glz::read_json(decoded, json));
    CHECK(decoded.target_pid == snapshot.target_pid);
    CHECK(decoded.files.at("limits") == snapshot.files.at("limits"));
}

TEST_CASE("unavailable target process evidence is absent rather than reported as zero") {
    const auto snapshot = erikslund::core::read_capacity_snapshot(std::numeric_limits<int>::max());
    CHECK_FALSE(snapshot.open_file_descriptors.has_value());
    CHECK_FALSE(snapshot.files.contains("limits"));
    CHECK_FALSE(snapshot.files.contains("stat"));
    CHECK_FALSE(snapshot.files.contains("memory.events"));
    CHECK_THROWS_AS(erikslund::core::read_capacity_snapshot(0), erikslund::core::ConfigError);
}
