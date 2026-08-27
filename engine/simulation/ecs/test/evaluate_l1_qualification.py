#!/usr/bin/env python3
"""Evaluate exact-SHA L1 benchmark-v9 samples against an external policy."""

from __future__ import annotations

import argparse
import csv
import glob
import math
from pathlib import Path
import statistics
import sys
import tomllib


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--input", action="append", nargs="+", required=True)
    return parser.parse_args()


def resolve_inputs(groups: list[list[str]]) -> list[Path]:
    result: list[Path] = []
    for group in groups:
        for argument in group:
            matches = sorted(Path(value) for value in glob.glob(argument))
            if not matches:
                if glob.has_magic(argument):
                    raise RuntimeError(f"input pattern matched no files: {argument}")
                matches = [Path(argument)]
            result.extend(path.resolve() for path in matches)
    return list(dict.fromkeys(result))


def summaries(
    paths: list[Path], expected_commit: str
) -> dict[tuple[str, int], dict[str, float]]:
    samples: dict[tuple[str, int], list[dict[str, float]]] = {}
    metadata = {
        "benchmark_schema_version", "git_commit", "build_type", "group",
        "metric", "size", "sample",
    }
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if row.get("benchmark_schema_version") != "9":
                    raise RuntimeError(f"{path}: benchmark schema is not v9")
                if row.get("git_commit") != expected_commit:
                    raise RuntimeError(
                        f"{path}: commit {row.get('git_commit')} does not match "
                        f"{expected_commit}"
                    )
                key = (row["metric"], int(row["size"]))
                samples.setdefault(key, []).append({
                    name: float(value)
                    for name, value in row.items()
                    if name not in metadata and value != ""
                })
    return {
        key: {
            field: statistics.median(sample[field] for sample in values)
            for field in values[0]
        }
        for key, values in samples.items()
        if values
    }


def require(values, metric: str, size: int) -> dict[str, float]:
    key = (metric, size)
    if key not in values:
        raise RuntimeError(f"missing evidence for {metric}@{size}")
    return values[key]


def main() -> int:
    args = parse_args()
    with args.policy.open("rb") as stream:
        policy = tomllib.load(stream)
    if policy.get("version") != 9 or policy.get("benchmark_schema_version") != 9:
        raise RuntimeError("qualification policy must require benchmark v9")
    values = summaries(resolve_inputs(args.input), args.expected_commit)
    failures: list[str] = []
    for rule in policy.get("scaling", []):
        small = require(values, rule["metric"], rule["small_size"])["nanoseconds"]
        large = require(values, rule["metric"], rule["large_size"])["nanoseconds"]
        exponent = math.log(large / small) / math.log(
            rule["large_size"] / rule["small_size"]
        ) if small and large else math.inf
        if exponent > rule["max_exponent"]:
            failures.append(
                f"{rule['name']}: exponent {exponent:.4f} > "
                f"{rule['max_exponent']:.4f}"
            )
    for rule in policy.get("ratio", []):
        small = require(values, rule["metric"], rule["small_size"])[
            rule["field"]
        ]
        large = require(values, rule["metric"], rule["large_size"])[
            rule["field"]
        ]
        ratio = large / small if small else math.inf
        if ratio > rule["max_ratio"]:
            failures.append(
                f"{rule['name']}: ratio {ratio:.4f} > "
                f"{rule['max_ratio']:.4f}"
            )
    for rule in policy.get("structural", []):
        value = require(values, rule["metric"], rule["size"])[rule["field"]]
        if "equals" in rule and value != rule["equals"]:
            failures.append(f"{rule['name']}: {value:g} != {rule['equals']:g}")
        if "max" in rule and value > rule["max"]:
            failures.append(f"{rule['name']}: {value:g} > {rule['max']:g}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        f"PASS: {len(policy.get('scaling', []))} scaling, "
        f"{len(policy.get('ratio', []))} ratio, "
        f"{len(policy.get('structural', []))} structural rules"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        print(f"qualification evaluation failed: {error}", file=sys.stderr)
        raise SystemExit(2)
