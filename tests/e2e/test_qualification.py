"""Regression tests for benchmark resource qualification, independent of the workload driver."""

import copy
import pathlib
import unittest
import unittest.mock

import qualification


def snapshot() -> dict:
    return {
        "passed": True,
        "failures": [],
        "target": {"pid": 1, "start_ticks": 100, "boot_id": "fixture-boot"},
        "process": {"open_file_descriptors": 10, "vm_rss_kib": 100, "threads": 4},
        "file_descriptors": {"sufficient": True},
        "tcp_ports": {"sufficient": True},
        "cgroup": {
            "path": "/",
            "cpu_max": "200000 100000",
            "cpuset_cpus_effective": "0-7",
            "memory_max": "536870912",
            "memory_current": "1000000",
            "memory_events": dict.fromkeys(qualification.MEMORY_EVENT_NAMES, 0),
        },
    }


def workload() -> dict:
    metrics = dict.fromkeys(qualification.REQUIRED_METRICS, 1.0)
    return {"passed": True, "metrics": {"before": metrics.copy(), "after": metrics.copy()}}


class QualificationTests(unittest.TestCase):
    def test_nonfinite_or_nonobject_json_is_unavailable_evidence(self) -> None:
        for document in ('{"passed":NaN}', '{"value":Infinity}', "null", "[]", "invalid"):
            with self.subTest(document=document):
                with unittest.mock.patch("pathlib.Path.read_text", return_value=document):
                    self.assertEqual(qualification.read_json(pathlib.Path("unused.json")), {})

    def test_native_snapshot_preserves_missing_evidence(self) -> None:
        normalized = qualification.normalize_snapshot({}, 1000, 1, 1, 1)
        self.assertIsNone(normalized["process"]["vm_rss_kib"])
        self.assertIsNone(normalized["file_descriptors"]["sufficient"])
        self.assertIsNone(normalized["tcp_ports"]["sufficient"])
        self.assertEqual(normalized["cgroup"]["memory_events"], {})
        result = qualification.evaluate(workload(), 0, normalized, 0, normalized, 0)
        self.assertEqual(result["qualification"]["verdict"], "inconclusive")

    def test_native_snapshot_checks_limits_reserved_ports_and_process_identity(self) -> None:
        raw = {
            "target_pid": 1,
            "open_file_descriptors": 10,
            "files": {
                "limits": "Max open files            65536                65536                files\n",
                "stat": "1 (worker (fixture)) S " + "0 " * 18 + "123 0 0",
                "status": "VmRSS:\t100 kB\nThreads:\t4\n",
                "ip_local_port_range": "32768 60999\n",
                "ip_local_reserved_ports": "32768-32770,32770,61000\n",
                "memory.events": "high 0\nmax 3\noom 1\noom_kill 0\noom_group_kill 0\n",
                "cgroup": "0::/\n",
            },
        }
        normalized = qualification.normalize_snapshot(raw, 1000, 1, 1, 1)
        self.assertTrue(normalized["passed"])
        self.assertEqual(normalized["target"]["start_ticks"], 123)
        self.assertEqual(normalized["process"]["vm_rss_kib"], 100)
        self.assertEqual(normalized["tcp_ports"]["usable_per_source_destination_tuple"], 28229)
        self.assertEqual(normalized["cgroup"]["memory_events"]["max"], 3)
        normalized = qualification.normalize_snapshot(raw, 100_000, 1, 1, 1)
        self.assertFalse(normalized["passed"])
        self.assertFalse(normalized["file_descriptors"]["sufficient"])
        self.assertFalse(normalized["tcp_ports"]["sufficient"])

    def test_host_descriptor_exhaustion_fails_even_with_a_sufficient_process_limit(self) -> None:
        raw = {
            "open_file_descriptors": 10,
            "files": {"limits": "Max open files 65536 65536 files", "file_nr": "9999 0 10000"},
        }
        normalized = qualification.normalize_snapshot(raw, 1000, 1, 1, 1)
        self.assertFalse(normalized["file_descriptors"]["sufficient"])

    def test_complete_successful_evidence_passes(self) -> None:
        result = qualification.evaluate(workload(), 0, snapshot(), 0, snapshot(), 0)
        self.assertTrue(result["passed"])
        self.assertTrue(result["functional_passed"])

    def test_capacity_failures_override_functional_success(self) -> None:
        for phase in ("before", "after"):
            for section in ("file_descriptors", "tcp_ports"):
                with self.subTest(phase=phase, section=section):
                    before, after = snapshot(), snapshot()
                    (before if phase == "before" else after)[section]["sufficient"] = False
                    result = qualification.evaluate(workload(), 0, before, 0, after, 0)
                    self.assertEqual(result["qualification"]["verdict"], "failed")
                    self.assertTrue(result["functional_passed"])

    def test_memory_event_increases_fail_but_historical_events_do_not(self) -> None:
        for name in qualification.MEMORY_EVENT_NAMES:
            with self.subTest(name=name):
                before = snapshot()
                before["cgroup"]["memory_events"][name] = 10
                after = copy.deepcopy(before)
                result = qualification.evaluate(workload(), 0, before, 0, after, 0)
                self.assertTrue(result["passed"])
                after["cgroup"]["memory_events"][name] += 1
                result = qualification.evaluate(workload(), 0, before, 0, after, 0)
                self.assertFalse(result["passed"])
                self.assertEqual(result["qualification"]["memory_events_delta"][name], 1)

    def test_missing_and_reset_counters_are_inconclusive(self) -> None:
        for value in (None, "unknown", -1, True, 0):
            with self.subTest(value=value):
                before, after = snapshot(), snapshot()
                before["cgroup"]["memory_events"]["oom"] = 1
                after["cgroup"]["memory_events"]["oom"] = value
                result = qualification.evaluate(workload(), 0, before, 0, after, 0)
                self.assertEqual(result["qualification"]["verdict"], "inconclusive")

    def test_missing_target_or_failed_collector_is_not_a_pass(self) -> None:
        for before, status in (({}, None), (snapshot(), 2), (snapshot(), 137)):
            with self.subTest(status=status):
                result = qualification.evaluate(workload(), 0, before, status, snapshot(), 0)
                self.assertEqual(result["qualification"]["verdict"], "inconclusive")

    def test_reported_preflight_failure_is_not_ignored(self) -> None:
        before = snapshot()
        before["passed"] = False
        result = qualification.evaluate(workload(), 0, before, 1, snapshot(), 0)
        self.assertEqual(result["qualification"]["verdict"], "failed")

    def test_restarted_target_cannot_reuse_old_resource_evidence(self) -> None:
        for field, replacement in (("pid", 2), ("start_ticks", 101), ("boot_id", "new-boot")):
            with self.subTest(field=field):
                after = snapshot()
                after["target"][field] = replacement
                result = qualification.evaluate(workload(), 0, snapshot(), 0, after, 0)
                self.assertEqual(result["qualification"]["verdict"], "failed")

    def test_missing_required_metric_is_inconclusive(self) -> None:
        for value in (None, float("nan"), float("inf"), -1):
            with self.subTest(value=value):
                generator = workload()
                generator["metrics"]["after"]["pool_lb_queued_bytes"] = value
                result = qualification.evaluate(generator, 0, snapshot(), 0, snapshot(), 0)
                self.assertEqual(result["qualification"]["verdict"], "inconclusive")

    def test_known_workload_failure_takes_precedence_over_missing_resources(self) -> None:
        generator = workload()
        generator["passed"] = False
        result = qualification.evaluate(generator, 1, {}, None, {}, None)
        self.assertEqual(result["qualification"]["verdict"], "failed")
        self.assertFalse(result["functional_passed"])

    def test_generator_exit_code_and_json_must_agree(self) -> None:
        result = qualification.evaluate(workload(), 1, snapshot(), 0, snapshot(), 0)
        self.assertFalse(result["passed"])
        self.assertFalse(result["functional_passed"])


if __name__ == "__main__":
    unittest.main()
