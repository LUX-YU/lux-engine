#!/usr/bin/env python3
"""Evaluate canonical Process execution benchmark v1 CSV evidence."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    policy = json.loads(arguments.policy.read_text(encoding="utf-8"))
    rows: list[dict[str, str]] = []
    for path in arguments.csv:
        with path.open("r", encoding="utf-8", newline="") as stream:
            rows.extend(csv.DictReader(stream))

    failures: list[str] = []
    expected_schema = int(policy["schema_version"])
    expected_samples = int(policy["performance_samples"])
    expected_sizes = {int(size) for size in policy["sizes"]}
    commits = {row["git_commit"] for row in rows}
    if not rows:
        failures.append("no benchmark rows")
    if commits == {"unknown"} or len(commits) != 1:
        failures.append(f"benchmark rows do not identify one exact commit: {sorted(commits)}")

    grouped: dict[tuple[str, int], list[int]] = defaultdict(list)
    structural_checks = 0
    for row in rows:
        group = row["group"]
        size = int(row["size"])
        if int(row["benchmark_schema_version"]) != expected_schema:
            failures.append(f"{group}[{size}]: invalid schema")
        if int(row["completions"]) != size:
            failures.append(f"{group}[{size}]: completion mismatch")
        if int(row["receiver_wakeups"]) != size:
            failures.append(f"{group}[{size}]: receiver wakeup mismatch")
        if int(row["allocations"]) != 0:
            failures.append(f"{group}[{size}]: expected zero timed allocations")
        structural_checks += 3
        grouped[(group, size)].append(int(row["nanoseconds"]))

    scaling_checks = 0
    for rule in policy["scaling_rules"]:
        group = str(rule["group"])
        sizes = {size for candidate, size in grouped if candidate == group}
        if sizes != expected_sizes:
            failures.append(f"{group}: expected sizes {sorted(expected_sizes)}, got {sorted(sizes)}")
            continue
        medians: dict[int, float] = {}
        for size in sorted(sizes):
            values = grouped[(group, size)]
            if len(values) != expected_samples:
                failures.append(f"{group}[{size}]: expected {expected_samples} samples, got {len(values)}")
            medians[size] = statistics.median(values)
        sorted_sizes = sorted(medians)
        for smaller, larger in zip(sorted_sizes, sorted_sizes[1:]):
            exponent = math.log(medians[larger] / medians[smaller]) / math.log(larger / smaller)
            maximum = float(rule["maximum_exponent"])
            if exponent > maximum:
                failures.append(f"{group}[{smaller}->{larger}]: exponent {exponent:.3f} exceeds {maximum:.3f}")
            scaling_checks += 1

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"PASS: {scaling_checks} scaling, {structural_checks} structural checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
