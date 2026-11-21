"""Check Render C++ signature changes against a Git revision (default: HEAD)."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

from tree_sitter import Language, Parser
import tree_sitter_cpp


ROOTS = (
    "modules/function/render",
    "cmake/installed-consumers/render-extension",
    "cmake/installed-consumers/render-client",
)
EXTENSIONS = {".cpp", ".hpp", ".h", ".hh", ".cc", ".cxx", ".hxx", ".inl", ".ipp"}
ANNOTATIONS = re.compile(rb"\b(?:LUX_RENDER_PUBLIC|NRI_CALL|__stdcall|CALLBACK|WINAPI)\b")
PARAMETERS = {"parameter_declaration", "optional_parameter_declaration", "variadic_parameter_declaration", "..."}
SCOPES = {"namespace_definition", "class_specifier", "struct_specifier", "function_definition",
          "alias_declaration", "init_declarator", "assignment_expression", "field_initializer"}


def git(*args):
    return subprocess.check_output(["git", "-c", "core.safecrlf=false", *args])


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
                    while label.type == "function_declarator":
                        label = label.child_by_field_name("declarator")
                    if label:
                        names.append(spelling(label))
                    break
        parent = parent.parent
    return " / ".join(reversed(names))


def signatures(parser, source):
    root = parser.parse(ANNOTATIONS.sub(b"", source)).root_node
    found = {}
    stack = [root]
    while stack:
        node = stack.pop()
        stack.extend(reversed(node.named_children))
        if node.type != "parameter_list" or node.parent.type == "requires_expression":
            continue
        count = sum(child.type in PARAMETERS for child in node.children)
        if count > 4:
            key = (identity(node), spelling(node), count)
            found.setdefault(key, []).append(node.start_point.row + 1)
    return found


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--base", default="HEAD", help="parent or merge-base of the change under review")
    args = cli.parse_args()
    os.chdir(git("rev-parse", "--show-toplevel").decode().strip())
    base = git("rev-parse", "--verify", "--end-of-options", args.base + "^{commit}").strip().decode()
    paths = git("diff", "--name-only", "--no-renames", "-z", base, "--", *ROOTS).split(b"\0")
    paths += git("ls-files", "--others", "--exclude-standard", "-z", "--", *ROOTS).split(b"\0")
    tracked = set(git("ls-tree", "-r", "--name-only", "-z", base, "--", *ROOTS).split(b"\0"))
    parser = Parser(Language(tree_sitter_cpp.language()))
    additions = 0
    for raw_path in sorted(set(paths) - {b""}):
        path = Path(raw_path.decode())
        if path.suffix.lower() not in EXTENSIONS or not path.is_file():
            continue
        previous = git("show", f"{base}:{path.as_posix()}") if raw_path in tracked else b""
        allowed = signatures(parser, previous)
        for key, lines in signatures(parser, path.read_bytes()).items():
            for line in lines[len(allowed.get(key, ())):]:
                additions += 1
                print(f"{path.as_posix()}:{line}: {key[2]} parameters: {key[0]} {key[1]}")
    print(f"New Render signatures above four parameters: {additions}")
    return int(additions != 0)


if __name__ == "__main__":
    sys.exit(main())
