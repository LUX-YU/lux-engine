"""Compiler-observed providers and a real instantiated policy-header negative control."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import editor_layering


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    repo, build = args.source.resolve(), args.build.resolve()
    rules = json.loads((repo / "editor_legacy/tests/architecture/rules.json").read_text())
    graph = json.loads((build / "editor-architecture/targets.json").read_text())
    commands = json.loads((build / "compile_commands.json").read_text())
    out = build / "layering-compiler"
    suffix = 1
    while out.exists():
        out = build / f"layering-compiler-{suffix}"
        suffix += 1
    out.mkdir()
    objects = {}
    for unit in commands:
        command = unit.get("command", "").replace("\\", "/")
        output = re.search(r'/Fo(?:"([^"]+)"|(\S+))', command)
        target = re.search(r"CMakeFiles/([^/]+)\.dir/", command)
        if output and target:
            objects[(output[1] or output[2]).rstrip('"')] = (target[1], unit["file"])
    raw = subprocess.run(["ninja", "-C", str(build), "-t", "deps"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", check=True).stdout
    (out / "ninja-dependencies.log").write_text(raw)
    dependencies, current = [], None
    for line in raw.splitlines():
        if ": #deps " in line:
            obj = line.split(": #deps ", 1)[0].replace("\\", "/")
            current = None
            if obj in objects and "(VALID)" in line:
                target, source = objects[obj]
                current = {"target": target, "source": source, "includes": []}
                dependencies.append(current)
        elif current is not None and line.startswith("    "):
            include = Path(line.strip())
            current["includes"].append(str((build / include).resolve() if not include.is_absolute() else include))
    assert dependencies, "No compiler dependency records were inspected"
    dependency_file = out / "compiler-dependencies.json"
    dependency_file.write_text(json.dumps(dependencies, indent=2))

    def inspect(file):
        findings = []
        editor_layering.check(repo, graph, {}, rules, "STRICT", lambda *f: findings.append(f), file)
        return findings

    findings = inspect(dependency_file)
    (out / "providers.json").write_text(json.dumps(findings, indent=2))
    assert not findings, json.dumps(findings[:20], indent=2)
    # These are the actual SceneSaveSource production flags and installed/development dependencies.
    # Inject one template TU with the policy role, compile it, then inspect /sourceDependencies.
    entry = next(x for x in commands if x["file"].replace("\\", "/").endswith("/src/SceneSaveSource.cpp"))
    results = []
    for phase, header, value in [
        ("positive", "lux/engine/editor/persistence/ArtifactStore.hpp", "lux::editor::persistence::CommitReceipt"),
        ("negative", "lux/engine/editor/scene/SceneSaveSource.hpp", "lux::editor::scene::SceneSaveSource"),
        ("repaired", "lux/engine/editor/persistence/ArtifactStore.hpp", "lux::editor::persistence::CommitReceipt"),
    ]:
        source = out / (phase + ".cpp")
        source.write_text('#include <' + header + '>\n'
                          'template<class T> struct Policy { static constexpr auto bytes = sizeof(T); };\n'
                          'auto instantiated = Policy<' + value + '>::bytes;\n')
        command = entry["command"].replace(entry["file"].replace("/", "\\"), source.as_posix()).replace(entry["file"], source.as_posix())
        command = re.sub(r'/Fo(?:"[^"]+"|\S+)', '/Fo"' + (out / (phase + '.obj')).as_posix() + '"', command)
        command = re.sub(r'/Fd(?:"[^"]+"|\S+)', '/Fd"' + (out / (phase + '.pdb')).as_posix() + '"', command)
        compiler_json = out / (phase + "-dependencies.json")
        command += ' /sourceDependencies "' + compiler_json.as_posix() + '"'
        compiled = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True,
                                  encoding="utf-8", errors="replace")
        (out / (phase + ".log")).write_text(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        data = json.loads(compiler_json.read_text(encoding="utf-8-sig"))["Data"]
        normalized = out / (phase + "-providers.json")
        normalized.write_text(json.dumps([{"target": "editor_persistence_legacy", "source": str(source),
                                          "includes": data["Includes"]}], indent=2))
        result = inspect(normalized)
        (out / (phase + "-findings.json")).write_text(json.dumps(result, indent=2))
        if phase == "negative":
            assert any(x[0] == "policy_instantiation_leak" and 'SceneSaveSource.hpp' in x[2] for x in result), result
        else:
            assert not result, result
        results.append({"phase": phase, "compile_exit": compiled.returncode, "findings": result})
    (out / "results.json").write_text(json.dumps({"units": len(dependencies), "template": results}, indent=2))
    print("PASS compiler providers:", len(dependencies), "TUs; actual template positive/rejection/repair")
    return 0


if __name__ == "__main__":
    sys.exit(main())
