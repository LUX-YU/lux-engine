#!/usr/bin/env python3
"""Read-only P10Q inventory. Candidate search is NOT an architecture/quality gate.

Reads tracked source and, optionally, existing CMake File API replies. It does not
configure/build the project, use the network, change Git, or write into source.
Only the explicitly selected output file is created. --self-test uses an isolated
temporary Git repository; its result is not a lux-engine test result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".h", ".inl", ".cmake", ".py"}
DEFAULT_SCOPES = ["editor", "engine/process", "modules/core/object"]
PATTERNS = {
    # Heuristic occurrences include comments/strings: review each candidate.
    "raw_void_pointer": r"\b(?:const\s+)?void\s*\*",
    "std_function": r"\bstd::function\s*<",
    "potential_parallel_executor": r"\bstd::(?:async|jthread)\b|\bstd::thread\s*[({]",
    "potential_post_cpp20_api": r"\bstd::(?:expected|scope_exit|move_only_function|print|println)\b|\bstd::ranges::to\b",
    "platform_specific_reference": r"\b(?:Windows\.h|windows\.h|HWND|SendInput|LoadLibrary[A-W]*|dlopen|unistd\.h)\b",
    "machine_absolute_path": r"\b[A-Za-z]:[\\/]",
    "private_include_reference": r"(?:pinclude|sinclude|/detail/)",
    "static_start_member": r"\bstatic\b[^;{}]{0,400}\bstart\s*\(",
}


def git(repo: Path, *args: str) -> bytes:
    p = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=False)
    if p.returncode:
        detail = p.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {detail}")
    return p.stdout


def decode_path(value: bytes) -> str:
    # Git paths are bytes on POSIX. Invalid UTF-8 is represented explicitly;
    # no malformed path is silently substituted and then read as another file.
    return value.decode("utf-8", "surrogateescape")


def category(path: str) -> str:
    parts = set(Path(path).parts)
    if "dev_log" in parts or "reference" in parts:
        return "historical_material"
    if "test" in parts or "tests" in parts or "installed-consumers" in parts:
        return "test_or_consumer"
    if "codegen" in parts or "generated" in parts:
        return "generator_or_generated_candidate"
    if "transition" in parts:
        return "registered_transition_candidate"
    if Path(path).name == "CMakeLists.txt" or Path(path).suffix == ".cmake":
        return "build_configuration"
    if Path(path).suffix == ".py":
        return "script"
    return "production_candidate"


def summarize_files(repo: Path, scopes: list[str]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    names = git(repo, "ls-files", "-z", "--", *scopes).split(b"\0")
    rows: list[dict[str, Any]] = []
    by_category: dict[str, dict[str, int]] = {}
    for raw in names:
        if not raw:
            continue
        name = decode_path(raw)
        path = repo / name
        if path.suffix not in SOURCE_SUFFIXES and path.name != "CMakeLists.txt":
            continue
        row: dict[str, Any] = {"path": name, "category": category(name)}
        # Deleted tracked files and symlinks remain visible, not counted as empty.
        if path.is_symlink():
            row["read_status"] = "SYMLINK_NOT_FOLLOWED"
            rows.append(row)
            continue
        if not path.is_file():
            row["read_status"] = "MISSING_OR_NONREGULAR"
            rows.append(row)
            continue
        try:
            data = path.read_bytes()
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError:
            row["read_status"] = "NON_UTF8_NOT_SCANNED"
            rows.append(row)
            continue
        except OSError as exc:
            row["read_status"] = "READ_ERROR"
            row["error"] = str(exc)
            rows.append(row)
            continue
        lines = text.splitlines()
        nonblank = sum(bool(line.strip()) for line in lines)
        matches: dict[str, list[int]] = {}
        for rule, pattern in PATTERNS.items():
            hits = [text.count("\n", 0, m.start()) + 1 for m in re.finditer(pattern, text)]
            if hits:
                matches[rule] = sorted(set(hits))
        row.update(
            read_status="READ",
            sha256=hashlib.sha256(data).hexdigest(),
            bytes=len(data),
            lines=len(lines),
            nonblank_lines=nonblank,
            short_file_candidate=nonblank <= 40,
            heuristic_line_matches=matches,
        )
        totals = by_category.setdefault(row["category"], {"files": 0, "lines": 0, "short_candidates": 0})
        totals["files"] += 1
        totals["lines"] += len(lines)
        totals["short_candidates"] += int(nonblank <= 40)
        rows.append(row)
    return rows, {"by_category": by_category, "tracked_matching_paths": len(rows)}


def safe_reply_json(reply: Path, name: str) -> dict[str, Any]:
    path = (reply / name).resolve()
    if not path.is_relative_to(reply.resolve()):
        raise RuntimeError("CMake reply JSON path escapes the selected reply directory")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"Expected a JSON object: {path}")
    return value


def read_cmake_reply(reply: Path) -> dict[str, Any]:
    indices = list(reply.glob("index-*.json"))
    if not indices:
        raise RuntimeError(f"No CMake File API index found in {reply}")
    index = max(indices, key=lambda p: (p.stat().st_mtime_ns, p.name))
    root = safe_reply_json(reply, index.name)
    models = [o for o in root.get("objects", []) if o.get("kind") == "codemodel"]
    if not models:
        raise RuntimeError("Existing CMake reply has no codemodel object; create the query in the implementation environment")
    targets: list[dict[str, Any]] = []
    for model_ref in models:
        model = safe_reply_json(reply, model_ref["jsonFile"])
        for config in model.get("configurations", []):
            for ref in config.get("targets", []):
                target = safe_reply_json(reply, ref["jsonFile"])
                targets.append({
                    "configuration": config.get("name", ""),
                    "name": target.get("name"),
                    "id": target.get("id"),
                    "type": target.get("type"),
                    "artifacts": [a.get("path") for a in target.get("artifacts", [])],
                    "dependencies": [d.get("id") for d in target.get("dependencies", [])],
                    "source_count": len(target.get("sources", [])),
                })
    return {"index": index.name, "targets": targets,
            "limitation": "Configured target metadata only; not a runtime module-load or SDK ABI audit"}


def inventory(repo: Path, scopes: list[str], base: str | None, reply: Path | None) -> dict[str, Any]:
    repo = repo.resolve()
    toplevel = Path(git(repo, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if toplevel != repo:
        raise RuntimeError("--repo must name the Git working-tree root, not a subdirectory")
    for scope in scopes:
        if not scope or Path(scope).is_absolute() or ".." in Path(scope).parts or ":" in scope:
            raise ValueError(f"Scope must be a repository-relative path: {scope!r}")
    rows, totals = summarize_files(repo, scopes)
    report: dict[str, Any] = {
        "schema": "lux.p10q.inventory.v1",
        "assessment": "NOT_A_GATE",
        "repo": str(repo),
        "head": git(repo, "rev-parse", "HEAD").decode().strip(),
        "scope": scopes,
        "worktree_porcelain_z": git(repo, "status", "--porcelain=v1", "-z").decode("utf-8", "surrogateescape"),
        "read_basis": "Current tracked working-tree bytes, not presumed equal to HEAD",
        "limitations": [
            "Matches may occur in comments or strings and require human review",
            "Short-file candidates are not defects or automatic deletion instructions",
            "This is not AST, callgraph, complexity, timing, dynamic-loading, or correctness analysis",
            "Untracked file content is not scanned; Git status still reports its presence",
            "No build, configure, dependency download, checkout, reset, or source modification is performed",
        ],
        "files": rows,
        "summary": totals,
    }
    if base:
        resolved = git(repo, "rev-parse", "--verify", f"{base}^{{commit}}").decode().strip()
        report["base"] = resolved
        report["base_to_head_changed_paths"] = [
            decode_path(p) for p in git(repo, "diff", "--name-only", "-z", resolved, "HEAD", "--", *scopes).split(b"\0") if p
        ]
    if reply is not None:
        report["cmake_file_api"] = read_cmake_reply(reply.resolve())
    else:
        report["cmake_file_api"] = {"status": "NOT_SUPPLIED", "targets": []}
    return report


def self_test() -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="lux-p10q-inventory-test-") as directory:
        repo = Path(directory)
        subprocess.run(["git", "init", "-q", str(repo)], check=True, capture_output=True)
        (repo / "editor/src").mkdir(parents=True)
        (repo / "editor/test").mkdir()
        (repo / "editor/src/短文件.hpp").write_text("#pragma once\nusing Example = int;\n", encoding="utf-8")
        (repo / "editor/src/sample.cpp").write_text(
            "// A match is only a candidate.\nvoid* owner = nullptr;\nstd::function<void()> action;\n", encoding="utf-8")
        (repo / "editor/test/test.cpp").write_text("#include <Windows.h>\n", encoding="utf-8")
        git(repo, "add", "--", "editor")
        git(repo, "-c", "user.name=P10Q Script Test", "-c", "user.email=inventory-test@example.invalid", "commit", "-qm", "synthetic fixture")
        reply = repo / "reply"
        reply.mkdir()
        (reply / "index-1.json").write_text(json.dumps({"objects": [{"kind": "codemodel", "jsonFile": "model.json"}]}))
        (reply / "model.json").write_text(json.dumps({"configurations": [{"name": "Debug", "targets": [{"jsonFile": "target.json"}]}]}))
        (reply / "target.json").write_text(json.dumps({"name": "synthetic", "id": "synthetic::1", "type": "STATIC_LIBRARY", "sources": []}))
        before = (repo / "editor/src/sample.cpp").read_bytes()
        report = inventory(repo, ["editor"], "HEAD", reply)
        assert report["summary"]["tracked_matching_paths"] == 3
        assert report["summary"]["by_category"]["test_or_consumer"]["files"] == 1
        row = next(r for r in report["files"] if r["path"].endswith("sample.cpp"))
        assert row["heuristic_line_matches"]["raw_void_pointer"] == [2]
        assert report["cmake_file_api"]["targets"][0]["type"] == "STATIC_LIBRARY"
        assert report["base_to_head_changed_paths"] == []
        assert (repo / "editor/src/sample.cpp").read_bytes() == before
        (repo / "editor/src/sample.cpp").unlink()
        missing = inventory(repo, ["editor"], None, None)
        assert next(r for r in missing["files"] if r["path"].endswith("sample.cpp"))["read_status"] == "MISSING_OR_NONREGULAR"
        try:
            safe_reply_json(reply, "../outside.json")
            raise AssertionError("Path traversal was not rejected")
        except RuntimeError:
            pass
    return {"status": "PASS", "scope": "inventory.py synthetic local Git/JSON fixture only",
            "engine_test_executed": False, "checks": ["unicode_paths", "classification", "candidate_matches", "cmake_metadata", "fixed_base", "read_only_content", "missing_tracked_file", "reply_path_boundary"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--scope", action="append", help="Repeat for repository-relative directories; defaults to editor/process/object")
    parser.add_argument("--base", help="Optional existing base commit; no fetch is performed")
    parser.add_argument("--cmake-reply", type=Path, help="Existing <build>/.cmake/api/v1/reply directory")
    parser.add_argument("--force", action="store_true", help="Allow overwriting the explicitly selected report file")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            print(json.dumps(self_test(), ensure_ascii=False, indent=2))
            return 0
        if args.repo is None or args.output is None:
            parser.error("--repo and --output are required unless using --self-test")
        report = inventory(args.repo, args.scope or DEFAULT_SCOPES, args.base, args.cmake_reply)
        output = args.output.resolve()
        # Never overwrite a tracked source, even with --force.
        repo_root = args.repo.resolve()
        if output.is_relative_to(repo_root):
            rel = output.relative_to(repo_root).as_posix()
            if git(repo_root, "ls-files", "-z", "--", rel).strip(b"\0"):
                raise ValueError("Output path is tracked; refuse to overwrite project source or historical evidence")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w" if args.force else "x", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=True, indent=2)
            stream.write("\n")
        unread = sum(r["read_status"] != "READ" for r in report["files"])
        print(f"Inventory written: {output}; {len(report['files'])} tracked candidate paths; {unread} unscanned. NOT_A_GATE.")
        return 0 if not unread else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"Inventory failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
