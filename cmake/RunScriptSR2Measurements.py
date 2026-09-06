"""SR-2 bounded paired regression, using separately installed baseline and candidate executables.

Run in the VS developer environment. Performance and allocation diagnostics are separate processes.
Neither mode claims to intercept allocations made inside every Windows DLL.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ("baseline", "candidate", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    cases = (
        ("cpp-update", "scene-cpp-update-heavy", False, False),
        ("event-fanout", "scene-event-fanout", False, False),
        ("lua-update", "scene-lua-update-heavy", False, True),
        ("lua-event", "scene-lua-event", False, True),
        ("flow-update", "scene-flowforge-update-heavy", True, False),
        ("flow-event", "scene-flowforge-event", True, False),
    )
    records = []
    comparisons = []
    business = ("scenario", "backend", "size", "seed", "sample", "phase", "active_instances", "calls",
                "ability_calls", "events", "suspensions", "resumes", "continuations", "awaitables",
                "event_waiters", "queue_depth", "external_queue_depth", "external_queue_capacity_failures",
                "lifecycle_begins", "lifecycle_ends", "checksum", "workers", "errors", "failures",
                "frame", "started", "completed")
    for name, group, tool, lua in cases:
        for mode, pairs, warmups, frames in (("performance", 5, 60, 300), ("diagnostic", 1, 1, 3)):
            for pair in range(pairs):
                observations = {}
                for variant in (("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")):
                    prefix = Path(getattr(args, variant))
                    binary = prefix / ("t/bin" if tool else "d/bin")
                    exe = binary / ("flowforge_script_runtime_benchmark.exe" if tool else "script_runtime_benchmark.exe")
                    label = f"{name}-{mode}-{pair}-{variant}"
                    output = root / (label + ".csv")
                    command = [str(exe), "--group", group, "--mode", mode, "--size", "10000",
                               "--warmups", str(warmups), "--frames", str(frames), "--seed", "1592598566",
                               "--resume-budget", "2000", "--output", str(output)]
                    if not tool:
                        command += ["--workers", "0"]
                    fixture = prefix / "t/share/lux-engine/lua/lua_runtime_benchmark_fixture.lxsa"
                    if lua:
                        # Qualification stores generated fixtures beside each profile's build, not in the SDK.
                        fixture = prefix.parent.parent / "build/RelWithDebInfo" / prefix.name / (
                            "t/engine/toolchain/lua/lua_runtime_benchmark_fixture.lxsa")
                        command += ["--lua-artifact", str(fixture)]
                        if mode == "diagnostic":
                            command += ["--vm-accounting", "on"]
                    env = dict(os.environ)
                    clean_path = [p for p in env["PATH"].split(";") if "CodeRepos" not in p and "vcpkg" not in p]
                    env["PATH"] = ";".join([str(binary), str(prefix / "d/bin"), str(prefix / "t/bin"),
                                             "D:/Development/vcpkg/installed/x64-windows/bin"] + clean_path)
                    print(label, flush=True)
                    started = time.time()
                    with (root / (label + ".log")).open("w") as log:
                        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env)
                    rows = list(csv.DictReader(output.open(newline=""))) if output.exists() else []
                    valid = result.returncode == 0 and len(rows) >= frames
                    record = dict(case=name, mode=mode, pair=pair, variant=variant, command=command,
                                  executable_sha256=digest(exe), start_epoch=started,
                                  process_seconds=time.time() - started, exit_code=result.returncode,
                                  rows=len(rows), valid=valid)
                    if lua:
                        record["fixture_sha256"] = digest(fixture)
                    if valid:
                        record["median_ns"] = statistics.median(float(r["nanoseconds"]) for r in rows)
                        record["allocation_totals"] = {key: sum(int(r[key]) for r in rows) if key in rows[0] else None
                                                       for key in ("allocations", "vm_allocations", "vm_reallocations")}
                        observations[variant] = [{k: r[k] for k in business if k in r} for r in rows]
                    records.append(record)
                    (root / "runs.json").write_text(json.dumps(records, indent=2))
                comparisons.append(dict(case=name, mode=mode, pair=pair,
                                        business_equal=len(observations) == 2 and
                                        observations["baseline"] == observations["candidate"]))
                (root / "business-comparisons.json").write_text(json.dumps(comparisons, indent=2))
    if not all(r["valid"] for r in records) or not all(c["business_equal"] for c in comparisons):
        raise SystemExit("At least one run or business comparison is invalid; inspect raw evidence.")


if __name__ == "__main__":
    main()
