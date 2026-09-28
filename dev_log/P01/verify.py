"""Verify this frozen P01 evidence snapshot. Does not replace build/runtime/ownership review."""
from collections import Counter
import argparse
import sys
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class EvidenceError(Exception):
    pass


def check_digest(path, expected):
    if digest(path) != expected:
        raise EvidenceError("EVIDENCE_HASH_MISMATCH: " + str(path))


def verify(evidence_root, index_path):
    def artifact(relative):
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or "\\" in relative:
            raise EvidenceError("INVALID_ARCHIVE_PATH: " + relative)
        resolved = evidence_root.joinpath(*path.parts).resolve()
        if not resolved.is_relative_to(evidence_root):
            raise EvidenceError("INVALID_ARCHIVE_PATH: " + relative)
        return resolved

    receipt = read(HERE / "receipt.json")
    assert receipt["phase"] == "P01" and receipt["status"] == "PASS"
    assert receipt["stop_after"] == "P01" and not receipt["continuation_authorized"]
    for key in ["input_sha", "implementation_sha"]:
        assert re.fullmatch("[0-9a-f]{40}", receipt[key])
    subprocess.run(["git", "merge-base", "--is-ancestor", receipt["input_sha"], receipt["implementation_sha"]],
                   cwd=ROOT, check=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", receipt["implementation_sha"], "HEAD"],
                   cwd=ROOT, check=True)
    assert receipt["input_sha"] == "a7234602aaeab86bb947214dcd2ea4eb1b314cfa"

    results = {x["id"]: x for x in receipt["test_results"]}
    for number in range(1, 7):
        assert results[f"X01-{number:02}"]["status"] == "PASS"
    for name in ["Q06", "Q07", "Q08", "Q09", "Q10", "Q45", "INSTALLED", "CTEST", "P01_GATE"]:
        assert results[name]["status"] == "PASS"
    for command in receipt["commands"]:
        path = artifact(command["archive_log"])
        check_digest(path, command["sha256"])
        expected = 1 if path.stem in ["C01", "C03", "C04"] else 0
        assert command["exit_code"] == expected, command
        assert command["implementation_sha"] == receipt["implementation_sha"]

    logs = artifact("dev_log/P01/logs")
    for path in logs.glob("*-no-work.log"):
        assert "ninja: no work to do" in path.read_text(), path
    assert len(list(logs.glob("*-no-work.log"))) == 6
    assert "100% tests passed, 0 tests failed out of 35" in (logs / "final-ctest.log").read_text()
    installed = [x for x in logs.glob("*-ctest.log") if x.name != "final-ctest.log"]
    assert len(installed) == 5
    for path in installed:
        assert "100% tests passed, 0 tests failed" in path.read_text(), path
    for name in ["copy", "move"]:
        text = (logs / f"scope-{name}.log").read_text()
        assert "C2280" in text and "EditScope::EditScope" in text
    assert "error " not in (logs / "scope-valid.log").read_text()
    for name, marker in [("C01", "visible_before=0 visible_after=1"),
                         ("C03", "released_during_query=1"),
                         ("C04", "create_succeeded=1 outcome_succeeded=0")]:
        text = (logs / f"{name}.log").read_text()
        assert marker in text and f"FAIL {name} intended contract" in text
        assert results[name]["status"] == "FAIL"
    assert not subprocess.check_output(
        ["git", "diff", receipt["input_sha"], receipt["implementation_sha"], "--",
         "editor/app/test/baseline_failures.cpp", "editor/app/test/EditorTestAccess.cpp"], cwd=ROOT)

    old = read(ROOT / "dev_log/P00/test-coverage.json")
    coverage = read(HERE / "test-coverage.json")
    names = lambda values: {x["name"] if isinstance(x, dict) else x for x in values}
    assert names(old["final_tests"]) <= names(coverage["final_tests"])
    assert len(coverage["final_tests"]) == 35
    ledger = read(HERE / "migration-ledger.json")
    for item in ledger["members"]:
        if item["removal_deadline"] == "P01":
            assert item["status"] in ["REMOVED", "MOVED"]
    for item in ledger["seed_members"]:
        if item["delete_by"] == "P01":
            assert item["status"] == "REMOVED"
    for item in ledger["files"]:
        if item["delete_by"] == "P01":
            assert item["status"] == "MOVED"
            assert not (ROOT / item["path"]).exists()
            assert (ROOT / item["current_path"]).is_file()
    assert len(ledger["introduced_members"]) == 8
    assert all(x["removal_deadline"] == "P12" for x in ledger["introduced_members"])
    assert ledger["transition_bridges"][0]["delete_by"] == "P12"
    assert not ledger["transition_bridges"][0]["installed"]
    proof = read(HERE / "history-member-proof.json")
    assert proof["history_baseline_members"] == 46 and proof["history_current_members"] == 41
    assert len(proof["removed"]) == 5
    assert all(x["exit_code"] == 0 for x in proof["ast_commands"])
    index = read(index_path)
    entry = index["P01-inventory"]
    assert entry["proof_path"] == "dev_log/P01/history-member-proof.json"
    check_digest(artifact(entry["proof_path"]), entry["proof_sha256"])
    assert entry["sha256"] == proof["raw_inventory"]["sha256"]
    assert entry["producer_path"] == proof["raw_inventory"]["path"]
    inventory = artifact(entry["archive_path"])
    check_digest(inventory, entry["sha256"])
    ast = read(inventory)
    key = lambda x: (x["qualified_symbol"], x["signature"], x["kind"])
    original = read(ROOT / "dev_log/P00/migration-ledger.json")
    before = Counter(map(key, original["members"]))
    after = Counter(map(key, ast["members"]))
    assert before - after == Counter(map(tuple, proof["removed"]))
    assert after - before == Counter(map(tuple, proof["added_legacy_checkpoint_members"]))

    architecture = read(logs / "architecture-current.log")
    assert architecture["status"] == "PASS" and not architecture["findings"]
    architecture_command = next(x for x in receipt["commands"]
                                if x["archive_log"] == "dev_log/P01/logs/architecture-current.log")
    assert architecture_command["argv"][-2:] == ["--stage", "P01"]
    package = read(logs / "package-audit.log")
    assert package["stage"] == "P01" and not package["findings"]
    for name in ["sessions-imports.log", "session-library-imports.log"]:
        text = (logs / name).read_text().lower()
        assert not any(x in text for x in ["vulkan", "editor_app.dll", "editor_context.dll", "editor_ui.dll",
                                          "project_storage.dll", "scene_runtime.dll"])
    assert "lux_engine_edit_history.dll" in (logs / "sessions-imports.log").read_text()

    for item in read(HERE / "files.json"):
        if item["git_blob"]:
            revision = receipt["implementation_sha"] + ":" + item["paths"][-1]
            blob = subprocess.check_output(["git", "rev-parse", revision], cwd=ROOT, text=True).strip()
            assert blob == item["git_blob"], revision
    print("PASS P01 gate: X01-01..06; scoped Q06..10/Q45; 35 CTest; five installed groups; "
          "46->41 history members; all P01 deadlines resolved; C01/C03/C04 remain FAIL; stop before P02.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, default=ROOT)
    parser.add_argument("--evidence-index", type=Path)
    args = parser.parse_args()
    evidence_root = args.evidence_root.resolve()
    index_path = args.evidence_index or evidence_root / "dev_log/P01-R1/evidence-index.json"
    try:
        verify(evidence_root, index_path)
    except FileNotFoundError as error:
        print("EVIDENCE_INCOMPLETE: " + str(error.filename), file=sys.stderr)
        raise SystemExit(1)
    except (EvidenceError, AssertionError, KeyError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error) or "EVIDENCE_INVALID", file=sys.stderr)
        raise SystemExit(1)
