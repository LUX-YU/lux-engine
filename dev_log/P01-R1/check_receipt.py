"""Check this frozen R1 run; delegate original P01 evidence to its single verifier.

This checks recorded executions and Git invariants, not a substitute for rerunning
the engine, consumers or the real CMake fixtures. No producer path is opened.
"""
from pathlib import Path, PurePosixPath
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def archive(relative):
    path = PurePosixPath(relative)
    assert not path.is_absolute() and ".." not in path.parts and "\\" not in relative, relative
    resolved = ROOT.joinpath(*path.parts).resolve()
    assert resolved.is_relative_to(ROOT), relative
    return resolved


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check():
    receipt = read(HERE / "receipt.json")
    assert receipt["phase"] == "P01" and receipt["revision"] == "R1" and receipt["status"] == "PASS"
    assert receipt["stop_after"] == "P01" and not receipt["continuation_authorized"]
    before, implementation = receipt["input_sha"], receipt["implementation_sha"]
    subprocess.run(["git", "merge-base", "--is-ancestor", before, implementation], cwd=ROOT, check=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", implementation, "HEAD"], cwd=ROOT, check=True)
    for item in read(HERE / "artifacts.json"):
        path = archive(item["archive_path"])
        assert digest(path) == item["sha256"], "EVIDENCE_HASH_MISMATCH: " + str(path)
    for item in read(HERE / "files.json"):
        blob = subprocess.check_output(["git", "rev-parse", implementation + ":" + item["path"]],
                                       cwd=ROOT, text=True).strip()
        assert blob == item["git_blob"], item["path"]

    commands = receipt["commands"]
    by_name = {PurePosixPath(x["archive_log"]).stem: x for x in commands}
    for command in commands:
        path = archive(command["archive_log"])
        assert digest(path) == command["sha256"], "EVIDENCE_HASH_MISMATCH: " + str(path)
        expected = 1 if path.stem in ["C01", "C03", "C04"] else 0
        assert command["exit_code"] == expected and command["implementation_sha"] == implementation, path
    required = ["configure", "build", "no-work", "ctest", "install", "close-throw", "close-contract",
                "sessions-detail", "boundaries", "architecture", "portable", "historical-p01-verifier",
                "package-audit", "sessions-imports", "session-library-imports"]
    assert set(required) <= by_name.keys()
    assert "-DLUX_EDITOR_MIGRATION_STAGE=P01" in by_name["configure"]["argv"]
    for name in ["architecture", "package-audit"]:
        argv = by_name[name]["argv"]
        assert argv[argv.index("--stage") + 1] == "P01"
    groups = ["editor-sessions-p01-consumer", "ui-resources-editor-d2", "ui-resources-external-feature",
              "ui-scene-pane-consumer", "ui-views-gpu-consumer"]
    for group in groups:
        for action in ["configure", "build", "no-work", "ctest"]:
            assert group + "-" + action in by_name
        assert "ninja: no work to do" in archive(by_name[group + "-no-work"]["archive_log"]).read_text()
        assert "100% tests passed, 0 tests failed" in archive(by_name[group + "-ctest"]["archive_log"]).read_text()
    assert "ninja: no work to do" in (HERE / "logs/no-work.log").read_text()
    assert "100% tests passed, 0 tests failed out of 37" in (HERE / "logs/ctest.log").read_text()
    old_names = {x["name"] for x in read(ROOT / "dev_log/P01/test-coverage.json")["final_tests"]}
    new_names = {x["name"] for x in read(HERE / "test-coverage.json")["final_tests"]}
    assert old_names <= new_names and new_names - old_names == {
        "editor.sessions_close_throw", "editor.sessions_close_contract"}
    for name in ["copy", "move"]:
        text = (HERE / f"logs/scope-{name}.log").read_text(encoding="utf-8")
        assert "C2280" in text and "EditScope::EditScope" in text

    results = {x["id"]: x for x in receipt["test_results"]}
    required_ids = [f"X01-{n:02}" for n in range(1, 7)]
    required_ids += [f"X01-R{group}-{n:02}" for group, count in [(1, 5), (2, 5), (3, 3)]
                     for n in range(1, count + 1)]
    required_ids += ["Q06", "Q07", "Q08", "Q09", "Q10", "Q45", "P01_GATE", "CTEST", "INSTALLED"]
    assert all(results[key]["status"] == "PASS" for key in required_ids)
    before_close = read(HERE / "logs/development/before-close-results.json")
    assert {x["mode"]: x["exit_code"] for x in before_close} == {"close-throw": 86, "close-contract": 1}
    for name in ["close-throw", "close-contract"]:
        assert "before=1 after=1 long_binding=32768" in (HERE / f"logs/{name}.log").read_text()
    pre_boundary = {x["id"]: x for x in read(HERE / "logs/development/before-boundaries.json")}
    boundary = {x["id"]: x for x in read(HERE / "evidence/boundaries.json")}
    for number in range(1, 6):
        key = f"X01-R2-{number:02}"
        assert pre_boundary[key]["exit_code"] == 0 and not pre_boundary[key]["passed"]
        item = boundary[key]
        assert item["exit_code"] != 0 and item["matched_expected_rule"] and item["repaired_exit_code"] == 0
        assert item["graph"] and item["log"] and item["repaired_log"]
    assert all(x["passed"] for x in boundary.values())
    assert all(x["matched"] for x in read(HERE / "logs/final/portability/results.json"))
    for name, marker in [("X01-R3-03-missing", "EVIDENCE_INCOMPLETE"),
                         ("X01-R3-03-tampered", "EVIDENCE_HASH_MISMATCH"),
                         ("X01-R3-03-missing-log", "EVIDENCE_INCOMPLETE")]:
        assert marker in (HERE / f"logs/final/portability/{name}.log").read_text()
    for name in ["X01-R3-01-relocated", "X01-R3-02-windows-provenance"]:
        assert "PRODUCER_PATH_READ_FORBIDDEN" in (HERE / f"logs/before-portability/{name}.log").read_text()

    protected = ["editor/history", "editor/contracts", "editor/transition", "editor/sessions/src/SessionState.cpp",
                 "editor/sessions/src/PersistenceCheckpoint.cpp", "editor/tools", "editor/assets",
                 "editor/app/test/baseline_failures.cpp", "editor/app/test/EditorTestAccess.cpp"]
    assert not subprocess.check_output(["git", "diff", before, implementation, "--", *protected], cwd=ROOT)
    changed_history = subprocess.check_output(["git", "diff", "--name-only", before, implementation,
                                              "--", "dev_log/P01"], cwd=ROOT, text=True).splitlines()
    assert changed_history == ["dev_log/P01/verify.py"]
    old_ledger = read(ROOT / "dev_log/P01/migration-ledger.json")
    ledger = read(HERE / "migration-ledger.json")
    for key in ["members", "seed_members", "files", "introduced_members", "transition_bridges",
                "supplemental_removals", "legacy_api_adaptations"]:
        assert ledger[key] == old_ledger[key], key
    assert not receipt["new_owners"] and ledger["current_phase"] == "P01"
    for name, marker in [("C01", "visible_before=0 visible_after=1"),
                         ("C03", "released_during_query=1"),
                         ("C04", "create_succeeded=1 outcome_succeeded=0")]:
        text = (HERE / f"logs/{name}.log").read_text()
        assert marker in text and f"FAIL {name} intended contract" in text
        assert results[name]["status"] == "FAIL"
    for name in ["architecture", "package-audit"]:
        assert not read(HERE / f"logs/{name}.log")["findings"]
    for name in ["sessions-imports", "session-library-imports"]:
        text = (HERE / f"logs/{name}.log").read_text().lower()
        assert not any(token in text for token in ["vulkan", "editor_app.dll", "editor_context.dll",
                                                   "editor_ui.dll", "project_storage.dll", "scene_runtime.dll"])
    # Reuse the original inventory/hash/AST/deadline verifier, not a parallel copy.
    subprocess.run([sys.executable, ROOT / "dev_log/P01/verify.py", "--evidence-root", ROOT],
                   cwd=ROOT, check=True)
    print("PASS P01 R1 gate: X01-01..06 + R1/R2/R3; scoped Q06..10/Q45; original 35 + 2 tests; "
          "SDK + five consumer groups; before failures archived; owners/bridge/deadlines unchanged; "
          "C01/C03/C04 remain FAIL; stop at P01.")


if __name__ == "__main__":
    try:
        check()
    except FileNotFoundError as error:
        print("EVIDENCE_INCOMPLETE: " + str(error.filename), file=sys.stderr)
        raise SystemExit(1)
    except (AssertionError, KeyError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error) or "EVIDENCE_INVALID", file=sys.stderr)
        raise SystemExit(1)
