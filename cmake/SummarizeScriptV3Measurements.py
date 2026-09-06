"""Summarize independent process runs; never pool frames into independent-run claims."""
import argparse
import csv
import hashlib
import json
import math
import pathlib
import statistics
from collections import defaultdict


def percentile(values, fraction):
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=pathlib.Path)
    args = parser.parse_args()
    runs = json.loads((args.root / "runs.json").read_text(encoding="utf-8-sig"))
    summaries = []
    paired = defaultdict(dict)
    for run in runs:
        if not run.get("valid"):
            continue
        groups = defaultdict(list)
        with pathlib.Path(run["output"]).open(encoding="utf-8-sig", newline="") as stream:
            for row in csv.DictReader(stream):
                groups[(row["scenario"], row.get("backend", ""), row.get("phase", ""))].append(row)
        for (scenario, backend, phase), rows in groups.items():
            times = sorted(int(row["nanoseconds"]) for row in rows)
            # Compare the full observed business sequence, not just a final checksum.
            business_fields = ("size", "seed", "sample", "calls", "ability_calls", "script_calls",
                "physics_queries", "events", "suspensions", "resumes", "continuations", "awaitables",
                "event_waiters", "lifecycle_begins", "lifecycle_ends", "checksum")
            business = [[row.get(key, "") for key in business_fields] for row in rows]
            business_hash = hashlib.sha256(json.dumps(business, separators=(",", ":")).encode()).hexdigest()
            summary = dict(case=run["case"], pair=run["pair"], variant=run["variant"],
                scenario=scenario, backend=backend, phase=phase, samples=len(times),
                p50_ns=percentile(times, .5), p90_ns=percentile(times, .9), p95_ns=percentile(times, .95),
                p99_ns=percentile(times, .99), max_ns=max(times), mean_ns=statistics.mean(times),
                stddev_ns=statistics.pstdev(times), total_ns=sum(times),
                first_checksum=rows[0].get("checksum", ""), last_checksum=rows[-1].get("checksum", ""),
                first_calls=rows[0].get("calls", ""), last_calls=rows[-1].get("calls", ""),
                first_resumes=rows[0].get("resumes", ""), last_resumes=rows[-1].get("resumes", ""),
                final_continuations=rows[-1].get("continuations", ""),
                final_awaitables=rows[-1].get("awaitables", ""),
                business_sequence_sha256=business_hash,
                binary_sha256=run["sha256"], percentile_policy="nearest-rank")
            summaries.append(summary)
            paired[(run["case"], run["pair"], scenario, backend, phase)][run["variant"]] = summary
    if not summaries:
        raise RuntimeError("No valid samples")
    with (args.root / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    deltas = []
    for (case, pair, scenario, backend, phase), values in paired.items():
        if "baseline" not in values or "final" not in values:
            continue
        before, after = values["baseline"], values["final"]
        result = dict(case=case, pair=pair, scenario=scenario, backend=backend, phase=phase,
            matching_checksum=before["last_checksum"] == after["last_checksum"],
            matching_samples=before["samples"] == after["samples"],
            matching_business=before["business_sequence_sha256"] == after["business_sequence_sha256"])
        for key in ("p50_ns", "p95_ns", "p99_ns", "total_ns"):
            result[key + "_delta"] = after[key] - before[key]
            result[key + "_percent"] = 100.0 * (after[key] / before[key] - 1) if before[key] else ""
        deltas.append(result)
    if deltas:
        with (args.root / "pairs.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(deltas[0]))
            writer.writeheader()
            writer.writerows(deltas)
    print(f"{len(summaries)} process/scenario summaries; {len(deltas)} independent paired comparisons")


if __name__ == "__main__":
    main()
