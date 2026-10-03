"""EC2 archive gate; producer paths are diagnostic only, historical records stay at their own SHA."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
from validate_quality_evidence import archived


def check(source, directory):
    def read(name):
        return json.loads((directory / name).read_text(encoding="utf-8"))

    def git(*args):
        return subprocess.check_output(["git", *args], cwd=source)

    receipt = read("receipt.json")
    assert receipt["phase"] == receipt["migration_stage"] == receipt["stop_after"] == "EC2"
    assert receipt["layering_mode"] == "STRICT"
    impl, base = receipt["implementation_sha"], receipt["input_sha"]
    git("merge-base", "--is-ancestor", base, impl)
    git("merge-base", "--is-ancestor", impl, "HEAD")
    rules = json.loads(git("show", impl + ":editor/tests/architecture/rules.json"))
    assert rules["stages"][-1] == "EC2"
    artifacts = read("artifacts.json")
    paths = set()
    for item in artifacts:
        assert item["path"] not in paths
        paths.add(item["path"])
        archived(directory, item["path"], item["sha256"])
    assert {"commands.json", "coverage.json", "files.json", "protection.json"} <= paths
    files = read("files.json")
    assert {item["path"] for item in files} == set(git("diff", "--name-only", base, impl).decode().splitlines())
    for item in files:
        if item["deleted"]:
            assert subprocess.run(["git", "cat-file", "-e", impl + ":" + item["path"]], cwd=source,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0
        else:
            assert hashlib.sha256(git("show", impl + ":" + item["path"])).hexdigest() == item["sha256"]
    commands = read("commands.json")
    by_name = {item["name"]: item for item in commands}
    assert len(by_name) == len(commands)
    for command in commands:
        assert command["log"] in paths
        archived(directory, command["log"], command["sha256"])
    assert receipt["user_patch_applied"] is False
    assert receipt["inherited"]["P12"] == "PARTIAL_USER_WAIVER"
    assert receipt["inherited"]["linux"] == receipt["inherited"]["system_ime"] == "NOT_RUN"
    coverage = read("coverage.json")
    assert {item["id"] for item in coverage} == {f"XEC2-{i:02}" for i in range(1, 39)}
    empty = hashlib.sha256(b"").hexdigest()
    for item in coverage:
        assert item["status"] in ["PASS", "PARTIAL", "FAIL", "NOT_RUN"]
        assert item["commands"] and item["meaning"]
        for name in item["commands"]:
            command = by_name[name]
            if item["status"] == "PASS":
                assert command["exit_code"] == 0
                assert command["source_head"] == impl
                assert command["source_diff_sha256"] == empty
                assert not command["source_untracked_sha256"]
    required = ["final-tracked", "final-configure", "final-build", "final-no-work", "final-ctest",
                "final-install", "final-player-build", "final-player-ctest", "final-headers",
                "final-skeleton-headless", "final-skeleton-window", "final-skeleton-app",
                "final-sdk", "final-ec2-sdk", "final-source-audit", "final-installed-audit", "final-module-sync"]
    for name in required:
        assert by_name[name]["exit_code"] == 0
    assert "-DLUX_EDITOR_MIGRATION_STAGE=EC2" in by_name["final-configure"]["argv"]
    assert "-DLUX_EDITOR_LAYERING_MODE=STRICT" in by_name["final-configure"]["argv"]
    for name in ["final-build", "final-no-work", "final-player-build"]:
        args = by_name[name]["argv"]
        assert args[args.index("--target") + 1] == "all"
        assert args[args.index("-j") + 1] == "4" and args[-2:] == ["-k", "0"]
    no_work = by_name["final-no-work"]
    assert b"no work to do" in archived(directory, no_work["log"], no_work["sha256"])
    assert receipt["input_sha"] == "248adc4576943cab83976afd8d1d5f31b63b70a9"
    assert receipt["status"] == ("PASS" if all(item["status"] == "PASS" for item in coverage) else "PARTIAL")
    return receipt["status"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    print("EC2 archive:", check(args.source, args.archive))
