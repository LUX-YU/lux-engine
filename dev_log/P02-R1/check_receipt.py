"""P02-R1 final gate. Archive-relative evidence only; historical source is checked by implementation SHA."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile


def check(root):
    here = root / "dev_log/P02-R1"
    def read(path): return json.loads(path.read_text(encoding="utf-8-sig"))
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and ".." not in path.parts and "\\" not in name
        resolved = root.joinpath(*path.parts).resolve()
        assert resolved.is_relative_to(root)
        return resolved
    receipt = read(here / "receipt.json")
    impl = receipt["implementation_sha"]
    assert receipt["phase"] == "P02" and receipt["revision"] == "R1" and receipt["status"] == "PASS"
    assert receipt["stop_after"] == "P02" and not receipt["continuation_authorized"]
    for a,b in [(receipt["input_sha"],receipt["functional_sha"]),(receipt["functional_sha"],impl),(impl,"HEAD")]:
        subprocess.run(["git","merge-base","--is-ancestor",a,b],cwd=root,check=True)
    for item in read(here / "artifacts.json"):
        assert digest(archived(item["archive_path"]).read_bytes()) == item["sha256"], item
    for item in read(here / "files.json"):
        revision = impl + ":" + item["path"]
        if item["status"] == "D":
            assert subprocess.run(["git","cat-file","-e",revision],cwd=root,capture_output=True).returncode != 0
        else:
            assert digest(subprocess.check_output(["git","show",revision],cwd=root)) == item["git_content_sha256"]
    commands = {PurePosixPath(c["archive_log"]).stem:c for c in receipt["commands"]}
    required = ["tracked-snapshot","configure","build","no-work","ctest","install","close-throw",
                "close-contract","sessions-detail","boundaries","model-boundaries","architecture","portable",
                "historical-verifiers","package-audit","test-names","model-imports","sessions-imports",
                "clone","clone-checkout","clone-tracked","clone-configure","clone-architecture",
                "structure","C01","C03","C04"]
    scenarios = ["content","atomic","identity","changes","plugin"] + [f"mixed-{n}" for n in range(1,5)] + [f"read-{n}" for n in range(5,9)]
    required += ["model-" + s for s in scenarios]
    groups = ["editor-sessions-p01-consumer","ui-resources-editor-d2","ui-resources-external-feature",
              "ui-scene-pane-consumer","ui-views-gpu-consumer","scene-model-p02-consumer"]
    required += [g+"-"+a for g in groups for a in ["configure","build","no-work","ctest"]]
    assert set(required) <= commands.keys(), set(required)-commands.keys()
    for name,c in commands.items():
        assert c["implementation_sha"] == impl
        assert c["exit_code"] == (1 if name in ["C01","C03","C04"] else 0), name
        assert digest(archived(c["archive_log"]).read_bytes()) == c["sha256"], name
    for name in ["configure","clone-configure"]:
        assert "-DLUX_EDITOR_MIGRATION_STAGE=P02" in commands[name]["argv"]
    for name in ["architecture","clone-architecture","package-audit"]:
        args = commands[name]["argv"]
        assert args[args.index("--stage")+1] == "P02"
        assert not read(archived(commands[name]["archive_log"]))["findings"]
    for name in ["no-work"]+[g+"-no-work" for g in groups]:
        assert "ninja: no work to do" in archived(commands[name]["archive_log"]).read_text()
    for name in ["ctest"]+[g+"-ctest" for g in groups]:
        assert "100% tests passed, 0 tests failed" in archived(commands[name]["archive_log"]).read_text()
    tests = {x["name"] for x in read(here / "logs/test-names.log")["tests"]}
    original = {x["name"] for x in read(root / "dev_log/P02/test-coverage.json")["final_tests"]}
    assert tests-original == {"editor.scene_model."+s for s in scenarios[5:]} and original <= tests
    before = read(here / "negative/results.json")
    assert before["base_sha"] == receipt["input_sha"]
    cases = {x["name"]:x for x in before["cases"]}
    for scenario in scenarios[5:]:
        case = cases[scenario]
        assert digest((here / "negative" / (scenario+".log")).read_bytes()) == case["sha256"]
        assert (case["exit_code"] == 0) == (scenario in ["mixed-4","read-6"])
    assert "complete replacement payload preserved=0" in (here / "negative/mixed-1.log").read_text()
    assert "nested_apply=1" in (here / "negative/read-5.log").read_text()
    assert "live_stamp_unchanged=0 snapshot_payload_unchanged=0" in (here / "negative/read-5.log").read_text()
    assert "nested_apply=0" in (here / "logs/model-read-5.log").read_text()
    assert "live_stamp_unchanged=1 snapshot_payload_unchanged=1" in (here / "logs/model-read-5.log").read_text()
    structure = read(here / "evidence/structure.json")
    assert structure["status"] == "PASS" and structure["implementation_sha"] == impl
    assert structure["actual_closures_unchanged"] and structure["unique_compile"]
    for filename in ["boundaries","model-boundaries"]:
        cases = read(here / "evidence" / (filename+".json"))
        assert cases and all(c["passed"] and c["graph"] and c["repaired_exit_code"] == 0 for c in cases)
    assert "PASS S01 old directories rejected" in (here / "logs/boundaries.log").read_text()
    assert all(x["matched"] for x in read(here / "logs/portability/results.json"))
    old_ledger = read(root / "dev_log/P02/migration-ledger.json")
    ledger = read(here / "migration-ledger.json")
    assert ledger["implementation_sha"] == impl and ledger["current_phase"] == "P02"
    assert ledger["transition_bridges"] == old_ledger["transition_bridges"]
    assert ledger["p02_r1"]["status"] == "PASS"
    for name,marker in [("C01","visible_before=0 visible_after=1"),("C03","released_during_query=1"),
                        ("C04","create_succeeded=1 outcome_succeeded=0")]:
        assert marker in (here / "logs" / (name+".log")).read_text()
        assert receipt["known_failures"][name]["status"] == "FAIL"
    for name in ["model-imports","sessions-imports"]:
        text = (here / "logs" / (name+".log")).read_text().lower()
        assert not any(t in text for t in ["vulkan","editor_app.dll","editor_context.dll","editor_ui.dll",
                                           "project_storage.dll","scene_runtime.dll","scene_composition.dll"])
    protected = ["dev_log/P00","dev_log/P01","dev_log/P01-R1","dev_log/P02",
                 "editor/app/test/baseline_failures.cpp","editor/app/src/EditorTestAccess.cpp","editor/transition"]
    assert not subprocess.check_output(["git","diff",receipt["input_sha"],impl,"--",*protected],cwd=root)
    # Reuse the original frozen validators. Their source paths are evaluated on their own implementation trees.
    with tempfile.TemporaryDirectory(prefix="lux-p02-r1-gate-") as directory:
        subprocess.run([sys.executable,here/"scripts/check_historical_evidence.py","--source",root,
                        "--evidence",directory],cwd=root,check=True)
    print("PASS P02-R1 gate: R02-01..08, S01-01..05, G01; original X02/P01/R1 and old-product regressions; "
          "actual dependency negatives and installed consumers. C01/C03/C04 remain FAIL. Stop at P02.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root",type=Path,default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try: check(args.evidence_root.resolve())
    except FileNotFoundError as error:
        print("EVIDENCE_INCOMPLETE: "+str(error.filename),file=sys.stderr); raise SystemExit(1)
    except (AssertionError,KeyError,ValueError,OSError,subprocess.CalledProcessError) as error:
        print(str(error) or "EVIDENCE_INVALID",file=sys.stderr); raise SystemExit(1)
