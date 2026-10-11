"""Combine functional results and target-resource evidence into a fail-closed verdict."""

import json
import math
import pathlib
import sys

MEMORY_EVENT_NAMES = ("high", "max", "oom", "oom_kill", "oom_group_kill")
REQUIRED_METRICS = ("pool_lb_connections_active", "pool_lb_io_workers", "pool_lb_queued_bytes")
DESCRIPTOR_HEADROOM = 4096


def named_counters(contents: str) -> dict[str, int]:
    counters = {}
    for line in contents.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1].isdigit():
            counters[fields[0].rstrip(":")] = int(fields[1])
    return counters


def normalize_snapshot(
    raw: dict, connections: int, backend_tuples: int, upstream_sources: int, generator_sources: int
) -> dict:
    """Interpret only evidence from the target namespaces; preserve missing values as unknown."""
    files = raw.get("files", {})
    process = named_counters(files.get("status", ""))
    failures = []
    required = connections * 2 + DESCRIPTOR_HEADROOM
    limits = next(
        (
            line.split()[3:5]
            for line in files.get("limits", "").splitlines()
            if line.startswith("Max open files ")
        ),
        [],
    )
    fd_sufficient = None
    if len(limits) == 2 and all(value == "unlimited" or value.isdigit() for value in limits):
        fd_sufficient = all(value == "unlimited" or int(value) >= required for value in limits)
    file_numbers = files.get("file_nr", "").split()
    if len(file_numbers) == 3 and all(value.isdigit() for value in file_numbers):
        allocated, _, maximum = map(int, file_numbers)
        current = raw.get("open_file_descriptors")
        if nonnegative_integer(current) and maximum - allocated < max(0, required - current):
            fd_sufficient = False
    if fd_sufficient is False:
        failures.append("target descriptor limits cannot cover the planned sessions")

    topology_sufficient = None
    usable_ports = None
    port_range = files.get("ip_local_port_range", "").split()
    if (
        len(port_range) == 2
        and all(value.isdigit() for value in port_range)
        and "ip_local_reserved_ports" in files
    ):
        start, end = map(int, port_range)
        if 0 <= start <= end <= 65535:
            reserved = set()
            for entry in files["ip_local_reserved_ports"].strip().split(","):
                if not entry:
                    continue
                bounds = list(map(int, entry.split("-")))
                reserved.update(range(max(start, bounds[0]), min(end, bounds[-1]) + 1))
            usable_ports = end - start + 1 - len(reserved)
            topology_sufficient = (
                usable_ports * backend_tuples * upstream_sources >= connections
                and usable_ports * generator_sources >= connections
            )
    if topology_sufficient is False:
        failures.append("target ephemeral port range cannot cover the declared topology")

    stat_fields = files.get("stat", "").rpartition(")")[2].split()
    start_ticks = int(stat_fields[19]) if len(stat_fields) > 19 else None
    cgroup_path = next(
        (line[3:] for line in files.get("cgroup", "").splitlines() if line.startswith("0::")),
        "unknown",
    )
    return {
        "schema_version": 1,
        "passed": not failures,
        "target": {
            "pid": raw.get("target_pid"),
            "start_ticks": start_ticks,
            "boot_id": files.get("boot_id", "unknown").strip(),
            "max_connections": connections,
        },
        "process": {
            "open_file_descriptors": raw.get("open_file_descriptors"),
            "vm_rss_kib": process.get("VmRSS"),
            "threads": process.get("Threads"),
            "voluntary_context_switches": process.get("voluntary_ctxt_switches"),
            "nonvoluntary_context_switches": process.get("nonvoluntary_ctxt_switches"),
        },
        "file_descriptors": {"required": required, "limits": limits, "sufficient": fd_sufficient},
        "tcp_ports": {
            "usable_per_source_destination_tuple": usable_ports,
            "backend_destination_tuples": backend_tuples,
            "upstream_source_addresses": upstream_sources,
            "generator_sources": generator_sources,
            "sufficient": topology_sufficient,
        },
        "cgroup": {
            "path": cgroup_path,
            "cpu_max": files.get("cpu.max", "unknown").strip(),
            "cpu_stat": named_counters(files.get("cpu.stat", "")),
            "cpuset_cpus_effective": files.get("cpuset.cpus.effective", "unknown").strip(),
            "memory_max": files.get("memory.max", "unknown").strip(),
            "memory_current": files.get("memory.current", "unknown").strip(),
            "memory_events": named_counters(files.get("memory.events", "")),
        },
        "failures": failures,
    }


def nonnegative_integer(value: object) -> bool:
    return type(value) is int and value >= 0


def evaluate(
    generator: dict,
    generator_status: int | None,
    before: dict,
    before_status: int | None,
    after: dict,
    after_status: int | None,
) -> dict:
    """Unknown evidence is inconclusive, known failures take precedence over missing data."""
    failures = []
    unavailable = []
    functional_passed = generator_status == 0 and generator.get("passed") is True
    if generator_status not in (None, 0) or generator.get("passed") is False:
        failures.append("functional workload failed")
    elif not functional_passed:
        unavailable.append("functional result unavailable")

    for label, snapshot, status in (
        ("preflight", before, before_status),
        ("postflight", after, after_status),
    ):
        if status == 1 or snapshot.get("passed") is False or snapshot.get("failures"):
            failures.append(f"{label} reports insufficient capacity")
        elif status != 0 or snapshot.get("passed") is not True:
            unavailable.append(f"{label} did not complete successfully")
        for section in ("file_descriptors", "tcp_ports"):
            sufficient = snapshot.get(section, {}).get("sufficient")
            if sufficient is False:
                failures.append(f"{label} {section} capacity insufficient")
            elif sufficient is not True:
                unavailable.append(f"{label} {section} capacity unknown")
        process = snapshot.get("process", {})
        for field in ("open_file_descriptors", "vm_rss_kib", "threads"):
            if not nonnegative_integer(process.get(field)):
                unavailable.append(f"{label} target {field} unavailable")
        cgroup = snapshot.get("cgroup", {})
        for field in ("path", "cpu_max", "cpuset_cpus_effective", "memory_max", "memory_current"):
            if cgroup.get(field) in (None, "", "unknown"):
                unavailable.append(f"{label} target cgroup {field} unavailable")

    before_target = before.get("target", {})
    after_target = after.get("target", {})
    for field in ("pid", "start_ticks", "boot_id"):
        previous, current = before_target.get(field), after_target.get(field)
        if previous in (None, "", "unknown") or current in (None, "", "unknown"):
            unavailable.append(f"target identity {field} unavailable")
        elif previous != current:
            failures.append(f"target identity changed: {field}")
    if before.get("cgroup", {}).get("path") != after.get("cgroup", {}).get("path"):
        unavailable.append("target cgroup changed during measurement")

    deltas = {}
    for name in MEMORY_EVENT_NAMES:
        previous = before.get("cgroup", {}).get("memory_events", {}).get(name)
        current = after.get("cgroup", {}).get("memory_events", {}).get(name)
        if not nonnegative_integer(previous) or not nonnegative_integer(current):
            unavailable.append(f"target memory.events {name} unavailable")
        elif current < previous:
            unavailable.append(f"target memory.events {name} counter reset")
        else:
            deltas[name] = current - previous
            if current > previous:
                failures.append(f"target memory.events {name} increased by {current - previous}")

    metrics = generator.get("metrics", {})
    for phase in ("before", "after"):
        if metrics.get(f"{phase}_error"):
            unavailable.append(f"{phase} metrics scrape failed")
        for name in REQUIRED_METRICS:
            value = metrics.get(phase, {}).get(name)
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                unavailable.append(f"{phase} metric {name} unavailable")

    verdict = "failed" if failures else "inconclusive" if unavailable else "passed"
    return {
        **generator,
        "passed": verdict == "passed",
        "functional_passed": functional_passed,
        "qualification": {
            "verdict": verdict,
            "failures": failures,
            "unavailable": unavailable,
            "memory_events_delta": deltas,
            "scope": "this generator, duration and topology only",
        },
    }


def reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


def read_json(path: pathlib.Path) -> dict:
    try:
        value = json.loads(path.read_text(), parse_constant=reject_nonfinite_json)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def read_status(path: pathlib.Path) -> int | None:
    try:
        return int(path.read_text())
    except (OSError, ValueError):
        return None


def main() -> int:
    if sys.argv[1] == "--snapshot":
        try:
            snapshot = normalize_snapshot(
                read_json(pathlib.Path(sys.argv[2])), *map(int, sys.argv[3:7])
            )
        except (AttributeError, TypeError, ValueError, IndexError):
            print("{}")
            return 3
        print(json.dumps(snapshot, sort_keys=True, allow_nan=False))
        return 0 if snapshot["passed"] else 1
    directory = pathlib.Path(sys.argv[1])
    try:
        result = evaluate(
            read_json(directory / "generator-result.json"),
            read_status(directory / "benchmark.exit-code"),
            read_json(directory / "preflight.json"),
            read_status(directory / "preflight.exit-code"),
            read_json(directory / "postflight.json"),
            read_status(directory / "postflight.exit-code"),
        )
    except (AttributeError, TypeError, ValueError):
        result = {
            "passed": False,
            "qualification": {"verdict": "inconclusive", "unavailable": ["malformed evidence"]},
        }
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    return {"passed": 0, "failed": 1, "inconclusive": 3}[result["qualification"]["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
