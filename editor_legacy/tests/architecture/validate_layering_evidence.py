"""Verify a portable P10Q-structure archive against its immutable implementation Git objects."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from validate_quality_evidence import archived, read, GROUPS


def check(source, directory):
    receipt = read(directory / "receipt.json")
    assert receipt["phase"] == "P10Q-structure"
    assert receipt["migration_stage"] == receipt["stop_after"] == "P10Q"
    assert receipt["layering_mode"] == "STRICT"
    assert receipt["continuation_authorized"] is False
    base = receipt["input_sha"]
    impl = receipt["implementation_sha"]
    assert base == "f7c27f9375cbf8dd8af37b30a6027a460de26213"

    def git(*args):
        return subprocess.check_output(["git", *args], cwd=source)

    git("merge-base", "--is-ancestor", base, impl)
    git("merge-base", "--is-ancestor", impl, "HEAD")
    assert not git("diff", "--name-only", base, impl, "--", "dev_log").strip()
    assert git("show", base + ":editor_legacy/project/src/ProjectBuilder.cpp") == git(
        "show", impl + ":editor_legacy/authoring/project/src/ProjectBuilder.cpp")

    items = read(directory / "artifacts.json")
    artifacts = {item["path"]: item["sha256"] for item in items}
    assert len(artifacts) == len(items) and artifacts
    for name, digest in artifacts.items():
        archived(directory, name, digest)

    def data(name):
        return archived(directory, name, artifacts[name])

    def document(name):
        return json.loads(data(name))

    files = document("files.json")
    assert {item["path"] for item in files} == set(git("diff", "--name-only", base, impl).decode().splitlines())
    for item in files:
        if item["deleted"]:
            assert subprocess.run(["git", "cat-file", "-e", impl + ":" + item["path"]], cwd=source,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0
        else:
            assert hashlib.sha256(git("show", impl + ":" + item["path"])).hexdigest() == item["sha256"]
    plan = document("file-plan.json")
    assert len(plan) == 518
    for item in plan:
        assert item["final_action"] and item["final_evidence"]
    commands = {item["name"]: item for item in receipt["commands"]}
    assert len(commands) == len(receipt["commands"])
    for command in commands.values():
        assert command["implementation_sha"] == impl
        assert command["sha256"] == artifacts[command["log"]]

    def passed(name):
        assert commands[name]["exit_code"] == 0, name
        return commands[name]

    for name in ["configure", "build", "no-work", "ctest", "install", "tracked-snapshot",
                 "cpu-configure", "cpu-build", "cpu-no-work", "cpu-ctest", "player-configure",
                 "player-build", "player-no-work", "player-ctest", "clang-public-headers",
                 "clang-configure", "clang-build", "clang-ctest", "regenerate", "regenerate-no-work"]:
        passed(name)
    for name in ["configure", "cpu-configure", "player-configure"]:
        argv = commands[name]["argv"]
        assert "-DLUX_EDITOR_MIGRATION_STAGE=P10Q" in argv
        assert "-DLUX_EDITOR_LAYERING_MODE=STRICT" in argv
    for name in ["build", "no-work", "cpu-build", "player-build", "regenerate", "regenerate-no-work"]:
        argv = commands[name]["argv"]
        assert argv[argv.index("--target") + 1] == "all"
        assert argv[argv.index("-j") + 1] == "4" and argv[-2:] == ["-k", "0"]
    for name in ["no-work", "cpu-no-work", "player-no-work", "regenerate-no-work"]:
        assert b"ninja: no work to do" in data(commands[name]["log"])

    current = json.loads(data(passed("test-names")["log"]))
    previous = document("tests-before.json")
    old_names = {item["name"] for item in previous["tests"]}
    new_names = {item["name"] for item in current["tests"]}
    assert len(old_names) == 204 and old_names <= new_names
    mapping = document("behavior-map.json")
    assert {item["old_test"] for item in mapping} == old_names
    assert all(item["new_test"] in new_names and item["assertions"] for item in mapping)
    details = data("logs/ctest-details.log").decode(errors="replace")
    for name in ["editor.scene_views_gpu", "editor.desktop_native_input"]:
        assert name in details

    groups = GROUPS + ["quality", "layering-tasks", "layering-save_core", "layering-project",
                       "layering-layout", "gpu-ui", "editor-scene-pane"]
    assert receipt["consumer_groups"] == groups
    for group in groups:
        for suffix in ["configure", "build", "no-work", "ctest"]:
            passed(group + "-" + suffix)
    negatives = document("evidence/layering-negatives/results.json")
    assert {item["id"] for item in negatives} == {f"N{i:02}" for i in range(1, 15)}
    for item in negatives:
        assert item["passed"]
        phases = item["phases"]
        assert all(phases[p]["exit_code"] == 0 for p in ["positive", "positive-build", "repaired", "repaired-build"])
        assert phases["negative"]["exit_code"] != 0
        for phase in phases.values():
            raw = data("evidence/layering-negatives/" + phase["log"])
            if phase["exit_code"] != 0:
                assert item["expected_rule"].encode() in raw
    assert {item["id"] for item in receipt["coverage"]} == {f"XL{i:02}" for i in range(1, 25)}
    for item in receipt["coverage"]:
        assert item["status"] == "PASS" and item["evidence"] and item["observations"]
        for name in item["evidence"]:
            data(name)
    assert receipt["status"] == "PASS"
    scope = document("scope-amendment.json")
    assert scope["Linux"] == scope["system_IME"] == "NOT_RUN"
    assert scope["old_P10Q"] == scope["old_performance"] == "PARTIAL"
    for defect in ["C01", "C03", "C04"]:
        assert receipt["known_failures"][defect]["status"] == "FAIL"
        assert receipt["known_failures"][defect]["owner"]
        assert commands[defect]["exit_code"] == 1
    return receipt["status"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    print("P10Q-structure archive verified:", check(args.source.resolve(), args.archive.resolve()))
