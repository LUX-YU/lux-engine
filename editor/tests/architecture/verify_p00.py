"""Verify an explicitly supplied P00 receipt input; never a normal build/CI dependency."""
import argparse
import json
from pathlib import Path
import sys


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def identity(row):
    return tuple(row.get(k) for k in ("qualified_symbol", "legacy_path", "line", "kind", "signature"))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--records", type=Path, required=True)
    p.add_argument("--ast", type=Path, required=True)
    p.add_argument("--current-tests", type=Path, required=True)
    p.add_argument("--probes", type=Path, required=True)
    p.add_argument("--native-build", type=Path, required=True)
    args = p.parse_args()
    ledger = read(args.records / "migration-ledger.json")
    ast = read(args.ast)
    assert {identity(x) for x in ledger["members"]} == {identity(x) for x in ast["members"]}
    assert len({x["focus"] for x in ledger["members"]}) == 7
    assert len(ast["runs"]) == 7 and all(x["exit_code"] == 0 for x in ast["runs"])
    for member in ledger["members"]:
        for key in ("responsibility", "destination_module", "replacement", "first_action",
                    "removal_deadline", "disposition", "proof"):
            assert member[key], (member["id"], key)
        assert member["first_action"] <= member["removal_deadline"]
    assert len(ledger["seed_members"]) == 511
    assert all(x["status"] in ("MATCHED_FOCUS_AST", "SOURCE_REVIEWED_OUTSIDE_FOCUS")
               for x in ledger["seed_members"])
    assert all(x["exists_at_baseline"] for x in ledger["files"])
    print("PASS X00-03: seven owner AST inventories covered; no missing dispositions; seeds reconciled")

    coverage = read(args.records / "test-coverage.json")
    before = {t["name"] for t in coverage["baseline_tests"]}
    after = {t["name"] for t in read(args.current_tests)["tests"]}
    assert before <= after, before - after
    assert len(before) == 28
    probes = read(args.probes)
    assert {x["id"] for x in probes} == {"C01", "C03", "C04"}
    for result in probes:
        # Known failed contracts remain failures; setup errors/crashes are not evidence.
        assert result["exit_code"] == 1 and f'FAIL {result["id"]} intended contract' in result["output"]
        assert "PROBE_SETUP_ERROR" not in result["stderr"]
    assert not any("baseline_failures" in t for t in after)
    print("PASS X00-04: all 28 baseline test names retained; three defects remain separately recorded failures")

    cache = (args.native_build / "CMakeCache.txt").read_text(encoding="utf-8")
    assert "LUX_EDITOR_BUILD_NATIVE_TESTS:BOOL=ON" in cache
    for group in ("DESKTOP", "GPU", "TOOLCHAIN", "INSTALLED"):
        assert f"LUX_EDITOR_BUILD_{group}_TESTS:BOOL=OFF" in cache
    assert "EDITOR_TEST_LINKER:" not in cache and "EDITOR_TEST_POWERSHELL:" not in cache
    assert "deny_platform_test_tools.cmake" in cache
    graph = read(args.native_build / "editor-architecture/targets.json")
    names = {r["name"] for r in graph}
    assert {"editor_flow_source_native", "editor_flowforge", "lux_editor"} <= names
    assert not {"editor_flow_protocol", "editor_core_protocol", "consumer_protocol"} & names
    print("PASS X00-02/Q49: native configuration omits platform test tools and retains Flow/Editor product targets")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, KeyError) as error:
        print(f"P00_EVIDENCE_FAILURE: {error}", file=sys.stderr)
        sys.exit(1)
