"""P00 inventory of the seven legacy owners using an existing clang-cl and compile database.

No compiler/tool installation. AST evidence and lexical reference candidates stay distinct.
Run from a VS developer environment. Outputs belong to the requested evidence directory,
never to generated production headers. The V4 seed remains a separate immutable input.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess


FOCUS = {
    "Editor.cpp": "lux::editor::Editor::Impl",
    "SceneEditor.cpp": "lux::editor::scene::SceneEditor::Impl",
    "MaterialEditor.cpp": "lux::editor::material::MaterialEditor::Impl",
    "FlowForgeEditor.cpp": "lux::editor::flowforge::FlowForgeEditor::Impl",
    "EditorContext.cpp": "lux::editor::EditorContext",
    "PaneManager.cpp": "lux::editor::PaneManager",
    "EditHistory.cpp": "lux::editor::editing::EditHistory",
}
MEMBERS = {"FieldDecl", "VarDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl",
           "CXXConversionDecl", "FunctionTemplateDecl", "TypeAliasDecl", "TypedefDecl", "EnumDecl"}


def documents(text):
    decoder = json.JSONDecoder()
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset < len(text):
            value, offset = decoder.raw_decode(text, offset)
            yield value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--clang-cl", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    database = json.loads((args.build / "compile_commands.json").read_text(encoding="utf-8"))
    all_members = []
    runs = []
    contents = {}

    def location(node, inherited):
        loc = node.get("loc", node.get("range", {}).get("begin", {}))
        loc = loc.get("expansionLoc", loc.get("spellingLoc", loc))
        file = loc.get("file", inherited)
        path = Path(file).resolve()
        try:
            relative = path.relative_to(repo).as_posix()
        except ValueError:
            return "", 0, file
        if path not in contents:
            contents[path] = path.read_bytes() if path.exists() else b""
        line = loc.get("line")
        if not line and "offset" in loc:
            line = contents[path][:loc["offset"]].count(b"\n") + 1
        return relative, line or 0, file

    for filename, qualified in FOCUS.items():
        entry, = [c for c in database if Path(c["file"]).name == filename]
        # Retain the actual defines/include paths/language mode; discard output/codegen flags.
        command = entry["command"]
        command = command[command.index(" /nologo"):]
        command = re.sub(r" /Fo[^ ]+| /Fd[^ ]+| /FS| /Zi| /O2| /Ob1| -c ", " ", command)
        command = f'"{args.clang_cl}"' + command + (
            " /clang:-fsyntax-only /clang:-Xclang /clang:-ast-dump=json"
            " /clang:-Xclang /clang:-ast-dump-filter=" + qualified)
        ast_file = args.output / (filename + ".ast.json")
        with ast_file.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=entry["directory"], stdout=stream, stderr=subprocess.PIPE,
                                    text=True, encoding="utf-8", errors="replace")
        (args.output / (filename + ".diagnostics.txt")).write_text(result.stderr, encoding="utf-8")
        runs.append({"source": entry["file"], "command": command, "exit_code": result.returncode,
                     "ast_sha256": hashlib.sha256(ast_file.read_bytes()).hexdigest()})
        if result.returncode:
            raise RuntimeError(f"AST parse failed: {filename}: {result.stderr[:1000]}")
        roots = list(documents(ast_file.read_text(encoding="utf-8")))
        members = []
        ids = {}
        record_names = {}

        def record(node, owner, file):
            for child in node.get("inner", []):
                if child.get("isImplicit") or not child.get("name"):
                    continue
                path, line, child_file = location(child, file)
                kind, name = child["kind"], child["name"]
                if kind in MEMBERS:
                    item = {"qualified_symbol": owner + "::" + name, "name": name, "kind": kind,
                            "legacy_path": path, "line": line, "signature": child.get("type", {}).get("qualType", ""),
                            "references": [], "proof": "verified_ast_declaration", "focus": qualified}
                    members.append(item)
                    ids[child["id"]] = item
                if kind == "CXXRecordDecl" and child.get("completeDefinition"):
                    record(child, owner + "::" + name, child_file)

        last_file = entry["file"]
        for node in roots:
            _, _, last_file = location(node, last_file)
            if node["kind"] != "CXXRecordDecl" or not node.get("completeDefinition"):
                continue
            parent = record_names.get(node.get("parentDeclContextId"))
            name = node["name"]
            owner = parent + "::" + name if parent else (
                qualified if name == qualified.rsplit("::", 1)[-1] else qualified + "::" + name)
            record_names[node["id"]] = owner
            record(node, owner, last_file)

        def references(node, file):
            path, line, file = location(node, file)
            identifier = node.get("referencedMemberDecl", node.get("referencedDecl", {}).get("id"))
            previous = node.get("previousDecl")
            if previous in ids:
                ids[node["id"]] = ids[previous]
                if node.get("inner"):
                    ids[previous]["references"].append({"path": path, "line": line, "proof": "ast_definition"})
            if identifier in ids and path:
                ids[identifier]["references"].append({"path": path, "line": line, "proof": "ast_reference"})
            for child in node.get("inner", []):
                references(child, file)

        last_file = entry["file"]
        for node in roots:
            _, _, last_file = location(node, last_file)
            references(node, last_file)
        for item in members:
            unique = {(r["path"], r["line"], r["proof"]): r for r in item["references"]}
            item["references"] = list(unique.values())
        all_members.extend(members)
        print(f"AST {qualified}: {len(members)} explicit members", flush=True)

    # Broader consumers: candidate occurrences are not promoted to resolved AST references.
    files = subprocess.check_output(["git", "-C", str(repo), "ls-files", "-z"]).decode().split("\0")
    tokens = defaultdict(list)
    catalogue = []
    scopes = ("editor/", "engine/", "modules/function/ui/", "cmake/", "examples/")
    for name in filter(None, files):
        path = repo / name
        if not name.startswith(scopes) or not path.is_file():
            continue
        raw = path.read_bytes()
        entry = {"path": name, "sha256": hashlib.sha256(raw).hexdigest()}
        if path.suffix in (".hpp", ".cpp", ".h", ".py", ".cmake") or path.name == "CMakeLists.txt":
            text = raw.decode("utf-8-sig", errors="replace")
            for line, content in enumerate(text.splitlines(), 1):
                for token in set(re.findall(r"\b[A-Za-z_]\w*\b", content)):
                    tokens[token].append({"path": name, "line": line})
            entry["declaration_candidates"] = re.findall(
                r"\b(?:class|struct|enum\s+class|using)\s+([A-Za-z_]\w*)", text)
        catalogue.append(entry)
    for item in all_members:
        name = item["name"].lstrip("~")
        item["text_candidates"] = tokens.get(name, [])
    (args.output / "inventory.json").write_text(json.dumps({"runs": runs, "members": all_members,
        "files": catalogue, "limitations": ["Only the seven focus translation units are AST parsed.",
        "Text candidates include homonyms/comments; they are not proven symbol references.",
        "Other translation units and generated/installed consumers require the accompanying review."]},
        ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
