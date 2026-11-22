"""Audit source-spelled Render C++ callable parameter lists in a Git diff or a complete tree."""
import argparse
from collections import Counter
from dataclasses import asdict, dataclass
from hashlib import sha256
from importlib.metadata import version
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from tree_sitter import Language, Parser
import tree_sitter_cpp


ROOTS = ("modules/function/render", "cmake/installed-consumers/render-extension",
         "cmake/installed-consumers/render-client")
EXTENSIONS = {".c", ".cpp", ".hpp", ".h", ".hh", ".cc", ".cxx", ".hxx", ".inl", ".ipp", ".inc", ".ixx", ".cppm"}
ANNOTATIONS = re.compile(rb"\b(?:LUX_RENDER_PUBLIC|NRI_CALL|__stdcall|CALLBACK|WINAPI)\b")
PARAMETERS = {"parameter_declaration", "optional_parameter_declaration", "variadic_parameter_declaration", "..."}
MACRO_NAME = re.compile(rb"(?m)^\s*#\s*define[ \t]+([A-Za-z_]\w*)")
SCOPES = {"namespace_definition", "class_specifier", "struct_specifier", "function_definition",
          "alias_declaration", "init_declarator", "assignment_expression", "field_initializer"}


def git(*args):
    return subprocess.check_output(["git", "-c", "core.safecrlf=false", *args])


def commit(ref):
    return git("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").decode().strip()


def nul_paths(data):
    return {os.fsdecode(path) for path in data.split(b"\0") if path}


def spelling(node):
    return " ".join(node.text.decode("utf-8", errors="replace").split())


def identity(node):
    names = []
    parent = node.parent
    while parent:
        if parent.type in SCOPES or parent == node.parent:
            for field in ("name", "declarator", "left", "field"):
                label = parent.child_by_field_name(field)
                if label:
                    while label and label.type == "function_declarator":
                        label = label.child_by_field_name("declarator")
                    if label:
                        names.append(spelling(label))
                    break
        parent = parent.parent
    return " / ".join(reversed(names))


def location_kind(node):
    parent = node.parent
    while parent:
        if parent.type in {"function_declarator", "abstract_function_declarator"}:
            name = parent.child_by_field_name("declarator")
            if name and name.type in {"parenthesized_declarator", "abstract_parenthesized_declarator"}:
                return "function_type"
        if parent.type == "lambda_expression":
            return "lambda"
        if parent.type in {"alias_declaration", "type_definition", "type_descriptor", "pointer_declarator",
                           "parenthesized_declarator", "parameter_declaration"}:
            return "function_type"
        if parent.type == "function_definition":
            return "definition"
        if parent.type in {"field_declaration", "declaration"}:
            return "declaration"
        parent = parent.parent
    return "parameter_list"


@dataclass(frozen=True)
class Occurrence:
    line: int
    column: int
    kind: str
    owner: str
    signature: str
    parameters: int
    tokens: tuple

    def comparison_key(self):
        return self.kind, self.owner, self.tokens, self.parameters

    def evidence(self):
        fields = asdict(self)
        del fields["tokens"]
        return fields


def signature_tokens(node):
    """Include the declared return type and trailing qualifiers, but exclude the function body."""
    tokens = []
    parent = node.parent
    if parent and parent.type in {"function_definition", "declaration", "field_declaration"}:
        result = parent.child_by_field_name("type")
        if result:
            tokens.append(spelling(result))
    stack = [node]
    while stack:
        part = stack.pop()
        if part.child_count:
            stack.extend(reversed(part.children))
        elif part.type != "comment":
            tokens.append(part.text)
    return tuple(tokens)


def analyze(parser, source):
    # Keep original byte positions, including columns in Windows sources.
    text = ANNOTATIONS.sub(lambda match: b" " * len(match[0]), source)
    root = parser.parse(text).root_node
    stack = [root]
    gaps = []
    occurrences = []
    local_macros = set(MACRO_NAME.findall(source))
    while stack:
        node = stack.pop()
        # Anonymous missing punctuation is invisible through named_children.
        stack.extend(reversed(node.children))
        line, column = node.start_point
        if node.is_error or node.is_missing:
            gaps.append({"line": line + 1, "column": column + 1, "end_line": node.end_point.row + 1,
                         "kind": "missing" if node.is_missing else "ERROR", "token": node.type})
        # A macro invocation at namespace or class scope can emit whole declarations.
        # Tree-sitter parses its arguments as expressions, not the expanded declarations.
        if node.type == "call_expression":
            name = node.child_by_field_name("function")
            statement = node.parent
            if name and statement.type == "expression_statement" and statement.parent.type in {
                "translation_unit", "declaration_list", "field_declaration_list"
            }:
                gaps.append({"line": line + 1, "column": column + 1, "end_line": node.end_point.row + 1,
                             "kind": "unexpanded declaration", "token": spelling(name)})
        if node.type != "parameter_list" or node.parent.type == "requires_expression":
            continue
        for child in node.named_children:
            field = child.child_by_field_name("type")
            if field and field.text in local_macros:
                gaps.append({"line": child.start_point.row + 1, "column": child.start_point.column + 1,
                             "end_line": child.end_point.row + 1, "kind": "unexpanded parameter type",
                             "token": spelling(field)})
        count = sum(child.type in PARAMETERS for child in node.children)
        if count > 4:
            occurrences.append(Occurrence(line + 1, column + 1, location_kind(node), identity(node),
                                          spelling(node.parent), count, signature_tokens(node.parent)))
    return occurrences, gaps


def source_roots(args):
    roots = set(ROOTS)
    roots.update(args.include_path)
    if args.manifest:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
        for row in manifest["files"]:
            roots.add(row["path"])
            roots.update(row["final_paths"])
            roots.update(row["adaptation_paths"])
    normalized = set()
    for path in roots:
        candidate = Path(path)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise ValueError(f"Expected a repository-relative scope: {path}")
        normalized.add(candidate.as_posix().rstrip("/"))
    return sorted(path for path in normalized
                  if not any(parent.as_posix() in normalized for parent in Path(path).parents))


def select_paths(args, roots, base, target):
    def scoped(path):
        return (Path(path).suffix.lower() in EXTENSIONS and
                any(root == "." or path == root or path.startswith(root + "/") for root in roots))

    if args.full and target:
        paths = nul_paths(git("ls-tree", "-r", "--name-only", "-z", target))
    elif args.full:
        paths = nul_paths(git("ls-files", "--cached", "-z"))
    else:
        endpoints = [base, target] if target else [base]
        paths = nul_paths(git("diff", "--no-renames", "--name-only", "--diff-filter=ACMRT", "-z", *endpoints, "--"))
    if not target:
        paths.update(nul_paths(git("ls-files", "--others", "--exclude-standard", "-z")))
        paths = {path for path in paths if scoped(path) and Path(path).is_file()}
    return sorted(path for path in paths if scoped(path))


def blob_contents(revision, paths):
    if not paths or not revision:
        return {}
    # A single Git invocation for blob contents also avoids Windows command line length limits.
    tree = {}
    for entry in git("ls-tree", "-r", "-z", revision).split(b"\0"):
        if entry:
            metadata, path = entry.split(b"\t", 1)
            tree[os.fsdecode(path)] = metadata.split()[-1]
    selected = [path for path in paths if path in tree]
    data = subprocess.run(["git", "cat-file", "--batch"], check=True, capture_output=True,
                          input=b"".join(tree[path] + b"\n" for path in selected)).stdout
    contents = {}
    offset = 0
    for path in selected:
        end = data.index(b"\n", offset)
        oid, kind, size = data[offset:end].split()
        if oid != tree[path] or kind != b"blob":
            raise ValueError(f"Expected a Git source blob for {path}")
        offset = end + 1 + int(size)
        contents[path] = data[end + 1:offset]
        offset += 1
    return contents


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    mode = cli.add_mutually_exclusive_group()
    mode.add_argument("--base", help="compare with parent or fixed baseline (diff default: HEAD)")
    mode.add_argument("--full", action="store_true", help="audit all selected sources without a diff allowance")
    cli.add_argument("--revision", help="audit a committed candidate; the default reads the working tree")
    cli.add_argument("--include-path", action="append", default=[], help="additional repository-relative file or tree")
    cli.add_argument("--manifest", type=Path, help="also scan source, adaptation and final paths from file_actions.json")
    cli.add_argument("--report", type=Path, help="write a UTF-8 JSON evidence report")
    args = cli.parse_args()
    if args.report:
        args.report = args.report.resolve()
    if args.manifest:
        args.manifest = args.manifest.resolve()
    os.chdir(os.fsdecode(git("rev-parse", "--show-toplevel").rstrip(b"\r\n")))
    base = None if args.full else commit(args.base or "HEAD")
    target = commit(args.revision) if args.revision else None
    roots = source_roots(args)
    paths = select_paths(args, roots, base, target)
    old = blob_contents(base, paths)
    new = blob_contents(target, paths)
    parser = Parser(Language(tree_sitter_cpp.language()))
    findings, gaps = [], []
    file_hashes = {}
    for path in paths:
        source = new[path] if target else Path(path).read_bytes()
        file_hashes[path] = sha256(source).hexdigest()
        current, errors = analyze(parser, source)
        gaps.extend(dict(path=path, side="candidate", **e) for e in errors)
        allowed = Counter()
        if path in old:
            previous, errors = analyze(parser, old[path])
            gaps.extend(dict(path=path, side="base", **e) for e in errors)
            allowed.update(item.comparison_key() for item in previous)
        for item in current:
            key = item.comparison_key()
            if allowed[key]:
                allowed[key] -= 1
            else:
                findings.append(dict(path=path, **item.evidence()))
    if args.full and not paths:
        raise ValueError("Empty full scan scope")
    status = "INCOMPLETE" if gaps else "FAIL" if findings else "PASS"
    mode_name = "full" if args.full else "diff"
    report = {"mode": mode_name, "status": status, "base": base, "candidate": target or "WORKTREE",
              "head": commit("HEAD"), "roots": roots, "extensions": sorted(EXTENSIONS),
              "parser": {pkg: version(pkg) for pkg in ("tree-sitter", "tree-sitter-cpp")},
              "files": paths, "source_sha256": file_hashes, "over_limit_occurrences": findings, "parse_gaps": gaps,
              "occurrences_by_kind": dict(Counter(item["kind"] for item in findings)),
              "unique_api_count": None,
              "measurement": "source-spelled occurrences, not unique C++ API identities or macro expansions"}
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for item in findings:
        print(f"{item['path']}:{item['line']}: {item['parameters']} parameters ({item['kind']}): "
              f"{item['owner']} {item['signature']}")
    for gap in gaps:
        print(f"{gap['path']}:{gap['line']}:{gap['column']} [{gap['side']}] {gap['kind']} {gap['token']}")
    print(f"{mode_name} {status}: {len(paths)} files, {len(findings)} over-limit occurrences, "
          f"{len(gaps)} parse gaps")
    return 2 if gaps else int(bool(findings))


if __name__ == "__main__":
    sys.exit(main())
