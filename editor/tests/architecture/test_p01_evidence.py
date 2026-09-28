"""Run the one P01 verifier in a relocated, shallow Git fixture; prohibit producer evidence IO."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve()
    baseline = "a653a89bc3cfae25fbf2e0777b2b757b93583ba5"
    records = []
    with tempfile.TemporaryDirectory(prefix="lux-p01-evidence-") as temporary:
        root = Path(temporary).resolve()
        assert root.is_relative_to(Path(tempfile.gettempdir()).resolve())
        def git(*command):
            subprocess.run(["git", "-C", str(root), *map(str, command)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        git("init", "-q")
        git("fetch", "--depth=3", "--no-tags", source, baseline)
        git("update-ref", "refs/heads/review-fixture", baseline)
        git("symbolic-ref", "HEAD", "refs/heads/review-fixture")
        archive = root / "snapshot.tar"
        subprocess.run(["git", "-C", str(source), "archive", baseline,
                        "--output=" + str(archive), "dev_log/P00", "dev_log/P01", "editor/history"], check=True)
        with tarfile.open(archive) as contents:
            contents.extractall(root, filter="data")
        shutil.copyfile(source / "dev_log/P01/verify.py", root / "dev_log/P01/verify.py")
        correction = source / "dev_log/P01-R1"
        if correction.exists():
            shutil.copytree(correction, root / "dev_log/P01-R1")
        runner = root / "run.py"
        runner.write_text("""import runpy, sys
from pathlib import Path
def reject_producer(event, args):
    if event == 'open' and isinstance(args[0], (str, bytes)):
        value = str(args[0]).replace(chr(92), '/').lower()
        if value.startswith('e:/syncforder/coderepos/build/p01-evidence/'):
            raise PermissionError('PRODUCER_PATH_READ_FORBIDDEN: ' + str(args[0]))
sys.addaudithook(reject_producer)
runpy.run_path(str(Path(__file__).parent / 'dev_log/P01/verify.py'), run_name='__main__')
""", encoding="utf-8")
        def verify(name, expected, marker=""):
            result = subprocess.run([sys.executable, str(runner)], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            path = args.evidence / (name + ".log")
            path.write_bytes(result.stdout)
            text = result.stdout.decode("utf-8", errors="replace")
            matched = result.returncode == 0 if expected == 0 else result.returncode != 0 and marker in text
            records.append(dict(id=name, exit_code=result.returncode, matched=matched,
                                marker=marker, log=str(path)))
            print(f"{'PASS' if matched else 'FAIL'} {name}: exit={result.returncode}", flush=True)
        verify("X01-R3-01-relocated", 0)
        receipt_path = root / "dev_log/P01/receipt.json"
        receipt = json.loads(receipt_path.read_text())
        for index, command in enumerate(receipt["commands"]):
            command["log"] = f"Q:\\unavailable-producer\\arbitrary-{index}.txt"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        verify("X01-R3-02-windows-provenance", 0)
        index_path = root / "dev_log/P01-R1/evidence-index.json"
        if index_path.exists():
            entry = json.loads(index_path.read_text())["P01-inventory"]
            evidence = root / entry["archive_path"]
            held = evidence.with_suffix(".held")
            assert evidence.resolve().is_relative_to(root)
            evidence.rename(held)
            verify("X01-R3-03-missing", 1, "EVIDENCE_INCOMPLETE")
            held.rename(evidence)
            original = evidence.read_bytes()
            evidence.write_bytes(original + b" ")
            verify("X01-R3-03-tampered", 1, "EVIDENCE_HASH_MISMATCH")
            evidence.write_bytes(original)
            log = root / receipt["commands"][0]["archive_log"]
            held_log = log.with_suffix(".held")
            assert log.resolve().is_relative_to(root)
            log.rename(held_log)
            verify("X01-R3-03-missing-log", 1, "EVIDENCE_INCOMPLETE")
            held_log.rename(log)
            verify("restored", 0)
        else:
            records.append(dict(id="X01-R3-03", matched=False, reason="No archive index exists"))
            print("FAIL X01-R3-03: required archive index absent")
    (args.evidence / "results.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    return 0 if all(x["matched"] for x in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
