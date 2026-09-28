"""Run immutable receipt validators against source materialized from each implementation SHA.

Old receipt code is never rewritten to match the current layout. The temporary tree
contains historical editor sources, frozen evidence, and a read-only Git reference.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def verify(source, output):
    records = []
    git_dir = subprocess.check_output(
        ["git", "rev-parse", "--absolute-git-dir"], cwd=source, text=True).strip()
    for phase, script in [("P00", None), ("P01", "verify.py"),
                          ("P01-R1", "check_receipt.py"), ("P02", "check_receipt.py")]:
        receipt = json.loads((source / "dev_log" / phase / "receipt.json").read_text(encoding="utf-8-sig"))
        sha = receipt["implementation_sha"]
        subprocess.run(["git", "merge-base", "--is-ancestor", sha, "HEAD"], cwd=source, check=True)
        if phase == "P00":
            files = json.loads((source / "dev_log/P00/files.json").read_text())["changes"]
            for item in files:
                present = subprocess.run(["git", "cat-file", "-e", sha + ":" + item["path"]],
                                         cwd=source, capture_output=True).returncode == 0
                assert present == (item["status"] != "D"), item
            for item in json.loads((source / "dev_log/P00/fixture-hashes.json").read_text()):
                if ":" not in item["path"]:
                    data = subprocess.check_output(["git", "show", sha + ":" + item["path"]], cwd=source)
                    # P00 recorded this Windows checkout text hash, not its LF Git blob hash.
                    if item["path"] == "examples/render-plugin/triangle.plugin.json":
                        assert b"\r" not in data
                        data = data.replace(b"\n", b"\r\n")
                    assert hashlib.sha256(data).hexdigest() == item["sha256"], item
            log = b"PASS P00 historical tracked source existence and repository fixture hashes (not producer logs).\n"
        else:
            with tempfile.TemporaryDirectory(prefix="lux-historical-receipt-") as directory:
                root = Path(directory)
                archive = subprocess.check_output(["git", "archive", "--format=tar", sha, "editor"], cwd=source)
                with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
                    contents.extractall(root, filter="data")
                (root / ".git").write_text("gitdir: " + git_dir + "\n")
                for previous in ["P00", "P01", "P01-R1", "P02"]:
                    shutil.copytree(source / "dev_log" / previous, root / "dev_log" / previous)
                result = subprocess.run([sys.executable, root / "dev_log" / phase / script],
                                        cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log = result.stdout
                if result.returncode:
                    print(log.decode(errors="replace"))
                    raise RuntimeError(f"{phase} historical verifier exit {result.returncode}")
        path = output / (phase + ".log")
        path.write_bytes(log)
        records.append(dict(phase=phase, implementation_sha=sha, exit_code=0,
                            log=path.name, sha256=hashlib.sha256(log).hexdigest()))
        print(f"PASS {phase}: source tree {sha}", flush=True)
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    verify(args.source.resolve(), args.evidence.resolve())
