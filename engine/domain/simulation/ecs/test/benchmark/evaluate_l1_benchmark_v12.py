#!/usr/bin/env python3
"""Evaluate canonical Lux L1 benchmark v12 CSV evidence."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


INTEGER_FIELDS = {
    "size",
    "sample",
    "nanoseconds",
    "allocations",
    "updates",
    "notifications",
    "callbacks",
    "asset_resolution_delta",
    "target_resolution_delta",
    "entities_examined",
    "target_range_lookups",
    "handlers_visited",
    "target_ranges_built",
    "dispatch_registration_lookups",
    "dispatch_handlers_built",
    "instance_creates",
    "method_prepares",
    "frame_builds",
    "bindings_removed",
    "dirty_marks",
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    return parser.parse_args()


def read_rows(paths: list[Path]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for path in paths:
        with path.open("r", encoding="utf-8", newline="") as stream:
            for source in csv.DictReader(stream):
                row: dict[str, object] = dict(source)
                for field in INTEGER_FIELDS:
                    row[field] = int(source[field])
                row["benchmark_schema_version"] = int(source["benchmark_schema_version"])
                rows.append(row)
    return rows


def require_equal(row: dict[str, object], field: str, expected: int, failures: list[str]) -> None:
    actual = int(row[field])
    if actual != expected:
        failures.append(f"{row['group']}[{row['size']}].{field}: expected {expected}, got {actual}")


def validate_row(row: dict[str, object], failures: list[str]) -> int:
    group = str(row["group"])
    size = int(row["size"])
    structural_rules = 0

    if group == "task-graph":
        expected = {"callbacks": size, "allocations": 0}
    elif group == "world":
        expected = {"updates": size, "allocations": 0}
    elif group == "command-buffer":
        expected = {"updates": size, "allocations": 0}
    elif group == "reactive-dirty":
        expected = {"updates": size, "notifications": size, "allocations": 0}
    elif group == "cpp-method-prepared":
        expected = {
            "callbacks": size,
            "handlers_visited": size,
            "method_prepares": 1,
            "frame_builds": size,
            "allocations": 0,
        }
    elif group in {"hook-global-multi", "hook-entity-multi"}:
        expected = {
            "callbacks": size,
            "handlers_visited": size,
            "dispatch_handlers_built": size,
            "frame_builds": 1,
            "allocations": 0,
        }
    elif group == "global-event":
        expected = {
            "notifications": size,
            "callbacks": size,
            "handlers_visited": size,
            "frame_builds": size,
            "allocations": 0,
        }
    elif group == "entity-targeted-event-sparse":
        occurrences = min(size, 100000)
        scripted_entities = min(size, 10000)
        expected = {
            "notifications": occurrences,
            "callbacks": occurrences * 2,
            "entities_examined": occurrences,
            "target_range_lookups": occurrences,
            "handlers_visited": occurrences * 2,
            "target_ranges_built": scripted_entities,
            "dispatch_registration_lookups": 0,
            "dispatch_handlers_built": scripted_entities + 1,
            "frame_builds": occurrences,
            "allocations": 0,
        }
    elif group == "entity-targeted-event-dense":
        expected = {
            "notifications": 1,
            "callbacks": size,
            "entities_examined": 1,
            "target_range_lookups": 1,
            "handlers_visited": size,
            "target_ranges_built": 1,
            "dispatch_registration_lookups": 0,
            "dispatch_handlers_built": size,
            "frame_builds": 1,
            "allocations": 0,
        }
    elif group == "owned-worker-event-buffer":
        expected = {
            "updates": size,
            "notifications": size,
            "callbacks": size,
            "frame_builds": size,
            "allocations": 0,
        }
    elif group == "script-detach":
        expected = {"updates": 4, "bindings_removed": 4, "allocations": 0}
    elif group == "script-dirty":
        expected = {"updates": size, "notifications": size, "dirty_marks": size, "allocations": 0}
    elif group == "script-prepare-scaling":
        expected = {
            "updates": size,
            "callbacks": size,
            "asset_resolution_delta": size,
            "target_resolution_delta": 1,
            "dispatch_handlers_built": 1,
            "instance_creates": size,
            "method_prepares": size,
        }
    elif group == "lua-prepared-call-pool":
        expected = {"updates": size * 8, "method_prepares": size * 8, "allocations": 0}
    elif group == "cpp-static-object-slab":
        expected = {"updates": size, "instance_creates": size, "allocations": 0}
    elif group == "script-artifact-export-scaling":
        expected = {"updates": size}
    elif group == "ecs-snapshot":
        expected = {"updates": size}
    else:
        failures.append(f"unknown benchmark group: {group}")
        return 0

    for field, value in expected.items():
        require_equal(row, field, value, failures)
        structural_rules += 1
    return structural_rules


def median_by_size(rows: list[dict[str, object]], group: str) -> dict[int, float]:
    values: dict[int, list[int]] = defaultdict(list)
    for row in rows:
        if row["group"] == group:
            values[int(row["size"])].append(int(row["nanoseconds"]))
    return {size: statistics.median(samples) for size, samples in values.items()}


def main() -> int:
    arguments = parse_arguments()
    policy = json.loads(arguments.policy.read_text(encoding="utf-8"))
    rows = read_rows(arguments.csv)
    failures: list[str] = []
    schema = int(policy["schema_version"])
    samples = int(policy["performance_samples"])
    commits = {str(row["git_commit"]) for row in rows}

    if not rows:
        failures.append("no benchmark rows")
    if commits == {"unknown"} or len(commits) != 1:
        failures.append(f"benchmark rows do not identify one exact commit: {sorted(commits)}")

    grouped: dict[tuple[str, int], int] = defaultdict(int)
    structural_rules = 0
    for row in rows:
        if row["benchmark_schema_version"] != schema:
            failures.append(f"{row['group']}: expected schema {schema}, got {row['benchmark_schema_version']}")
        grouped[(str(row["group"]), int(row["size"]))] += 1
        structural_rules += validate_row(row, failures)
    for key, count in grouped.items():
        if count != samples:
            failures.append(f"{key[0]}[{key[1]}]: expected {samples} samples, got {count}")

    scaling_rules = 0
    for rule in policy["scaling_rules"]:
        group = str(rule["group"])
        medians = median_by_size(rows, group)
        sizes = sorted(medians)
        if len(sizes) < 2:
            failures.append(f"{group}: scaling rule needs at least two sizes")
            continue
        smaller, larger = sizes[-2:]
        exponent = math.log(medians[larger] / medians[smaller]) / math.log(larger / smaller)
        maximum = float(rule["maximum_exponent"])
        if exponent > maximum:
            failures.append(f"{group}: exponent {exponent:.3f} exceeds {maximum:.3f}")
        scaling_rules += 1

    ratio_rules = 0
    for rule in policy["ratio_rules"]:
        group = str(rule["group"])
        medians = median_by_size(rows, group)
        smaller = int(rule["smaller_size"])
        larger = int(rule["larger_size"])
        if smaller not in medians or larger not in medians:
            failures.append(f"{group}: missing {smaller}/{larger} ratio inputs")
            continue
        ratio = medians[larger] / medians[smaller]
        maximum = float(rule["maximum_median_ratio"])
        if ratio > maximum:
            failures.append(f"{group}: median ratio {ratio:.3f} exceeds {maximum:.3f}")
        ratio_rules += 1

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"PASS: {scaling_rules} scaling, {ratio_rules} ratio, {structural_rules} structural checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
