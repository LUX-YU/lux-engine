#!/usr/bin/env python3
"""Evaluate durable L1 benchmark CSVs against an external policy."""

from __future__ import annotations

import argparse
import csv
import glob
import math
from pathlib import Path
import sys
import tomllib


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument(
        "--input",
        action="append",
        nargs="+",
        required=True,
        metavar="CSV",
        help=(
            "one or more CSV files or glob patterns; the option may be "
            "repeated"
        ),
    )
    return parser.parse_args()


def resolve_inputs(groups: list[list[str]]) -> list[Path]:
    result: list[Path] = []
    seen: set[Path] = set()
    for group in groups:
        for argument in group:
            matches = sorted(Path(path) for path in glob.glob(argument))
            if not matches:
                if glob.has_magic(argument):
                    raise RuntimeError(f"input pattern matched no files: {argument}")
                matches = [Path(argument)]
            for path in matches:
                resolved = path.resolve()
                if resolved not in seen:
                    seen.add(resolved)
                    result.append(resolved)
    return result


def summaries(paths: list[Path]) -> dict[tuple[str, int], dict[str, float]]:
    result: dict[tuple[str, int], dict[str, float]] = {}
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if row["kind"] != "summary" or row["sample"] != "median":
                    continue
                result[(row["metric"], int(row["size"]))] = {
                    key: float(value)
                    for key, value in row.items()
                    if key not in {"kind", "metric", "sample"} and value != ""
                }
    return result


def require(
    values: dict[tuple[str, int], dict[str, float]],
    metric: str,
    size: int,
) -> dict[str, float]:
    key = (metric, size)
    if key not in values:
        raise RuntimeError(f"missing median evidence for {metric}@{size}")
    return values[key]


def main() -> int:
    args = parse_args()
    with args.policy.open("rb") as stream:
        policy = tomllib.load(stream)
    if policy.get("version") != 1:
        raise RuntimeError("unsupported qualification policy version")
    values = summaries(resolve_inputs(args.input))
    failures: list[str] = []

    for rule in policy.get("relative", []):
        measured = require(values, rule["metric"], rule["size"])["nanoseconds"]
        baseline = require(values, rule["baseline"], rule["size"])["nanoseconds"]
        ratio = measured / baseline if baseline else math.inf
        if ratio > rule["max_ratio"]:
            failures.append(
                f"{rule['name']}: ratio {ratio:.4f} > {rule['max_ratio']:.4f}"
            )

    for rule in policy.get("scaling", []):
        small = require(
            values, rule["metric"], rule["small_size"]
        )["nanoseconds"]
        large = require(
            values, rule["metric"], rule["large_size"]
        )["nanoseconds"]
        exponent = math.log(large / small) / math.log(
            rule["large_size"] / rule["small_size"]
        ) if small and large else math.inf
        if exponent > rule["max_exponent"]:
            failures.append(
                f"{rule['name']}: exponent {exponent:.4f} > "
                f"{rule['max_exponent']:.4f}"
            )

    for rule in policy.get("structural", []):
        value = require(values, rule["metric"], rule["size"])[rule["field"]]
        if "equals" in rule and value != rule["equals"]:
            failures.append(
                f"{rule['name']}: {value:g} != {rule['equals']:g}"
            )
        if "max" in rule and value > rule["max"]:
            failures.append(
                f"{rule['name']}: {value:g} > {rule['max']:g}"
            )

    if failures:
        for item in failures:
            print(f"FAIL: {item}", file=sys.stderr)
        return 1
    print(f"PASS: {len(policy.get('relative', []))} relative, "
          f"{len(policy.get('scaling', []))} scaling, "
          f"{len(policy.get('structural', []))} structural rules")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        print(f"qualification evaluation failed: {error}", file=sys.stderr)
        raise SystemExit(2)
