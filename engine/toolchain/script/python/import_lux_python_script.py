#!/usr/bin/env python3
"""Static, execution-free Python @lux.method -> canonical LXSA v2 importer."""

from __future__ import annotations

import argparse
import ast
import pathlib
import re
import struct
import sys
from dataclasses import dataclass


MAGIC = 0x4153584C
WIRE_VERSION = 2
SCHEMA_VERSION = 4
PYTHON_SOURCE_KIND = 2
GLOBAL_MODULE = 0
ENTITY_BEHAVIOR = 1
VALUE_PASS = 0


def fnv1a(text: str) -> int:
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def symbol_id(scope: str, name: str, args: list[str], returns: list[str]) -> int:
    value = 14695981039346656037

    def append(text: str) -> None:
        nonlocal value
        for byte in text.encode("utf-8"):
            value ^= byte
            value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        value ^= 0xFF
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    append(scope)
    append(name)
    for canonical in args:
        append(canonical)
        value ^= VALUE_PASS
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    value ^= 0xFE
    value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for canonical in returns:
        append(canonical)
        value ^= VALUE_PASS
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value or 1


def load_semantics(path: pathlib.Path) -> set[str]:
    pattern = re.compile(
        r'^\s*LUX_SCRIPT_BUILTIN\([^,]+,[^,]+,\s*"([^"]+)"\s*,'
    )
    result: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            result.add(match.group(1))
    if not result:
        raise ValueError("semantic SSOT contains no builtin entries")
    return result


def dotted_name(node: ast.expr) -> str | None:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        parent = dotted_name(node.value)
        return f"{parent}.{node.attr}" if parent else None
    return None


def annotation_name(node: ast.expr | None) -> str | None:
    if node is None:
        return None
    if isinstance(node, ast.Constant):
        if node.value is None:
            return "None"
        if isinstance(node.value, str):
            return node.value
    return dotted_name(node)


def is_lux_method(node: ast.expr) -> bool:
    if isinstance(node, ast.Call):
        node = node.func
    return dotted_name(node) == "lux.method"


@dataclass(frozen=True)
class Export:
    name: str
    symbol: int
    args: list[str]
    returns: list[str]


def collect_exports(
    tree: ast.Module,
    model: int,
    entry: str,
    module_name: str,
    semantics: set[str],
) -> list[Export]:
    if model == GLOBAL_MODULE:
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)]
        scope = module_name
        hide_self = False
    else:
        classes = [
            node
            for node in tree.body
            if isinstance(node, ast.ClassDef) and node.name == entry
        ]
        if len(classes) != 1:
            raise ValueError(f"entity entry class '{entry}' was not found exactly once")
        functions = [
            node
            for node in classes[0].body
            if isinstance(node, ast.FunctionDef)
        ]
        scope = f"{module_name}.{entry}"
        hide_self = True

    exports: list[Export] = []
    symbols: set[int] = set()
    for function in functions:
        if not any(is_lux_method(value) for value in function.decorator_list):
            continue
        if function.args.vararg or function.args.kwarg or function.args.kwonlyargs:
            raise ValueError(f"{function.name}: variadic/keyword-only parameters are unsupported")
        parameters = list(function.args.posonlyargs) + list(function.args.args)
        if hide_self:
            if not parameters or parameters[0].arg != "self":
                raise ValueError(f"{function.name}: EntityBehavior method requires hidden self")
            parameters = parameters[1:]
        argument_types: list[str] = []
        for parameter in parameters:
            canonical = annotation_name(parameter.annotation)
            if canonical not in semantics:
                raise ValueError(
                    f"{function.name}.{parameter.arg}: explicit Lux canonical type required"
                )
            argument_types.append(canonical)
        result = annotation_name(function.returns)
        if result is None:
            raise ValueError(f"{function.name}: explicit return annotation required")
        return_types: list[str] = []
        if result not in ("None", "NoneType"):
            if result not in semantics:
                raise ValueError(f"{function.name}: unsupported return type '{result}'")
            return_types.append(result)
        symbol = symbol_id(scope, function.name, argument_types, return_types)
        if symbol in symbols:
            raise ValueError(f"{function.name}: duplicate semantic symbol")
        symbols.add(symbol)
        exports.append(Export(function.name, symbol, argument_types, return_types))
    if not exports:
        raise ValueError("no @lux.method exports found")
    return exports


class Writer:
    def __init__(self) -> None:
        self.data = bytearray()

    def u8(self, value: int) -> None:
        self.data += struct.pack("<B", value)

    def u32(self, value: int) -> None:
        self.data += struct.pack("<I", value)

    def u64(self, value: int) -> None:
        self.data += struct.pack("<Q", value)

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8")
        self.u32(len(encoded))
        self.data += encoded


def encode(
    module_name: str,
    model: int,
    entry: str,
    exports: list[Export],
    payload: bytes,
    source_id: str,
) -> bytes:
    writer = Writer()
    writer.u32(MAGIC)
    writer.u32(WIRE_VERSION)
    writer.u32(SCHEMA_VERSION)
    writer.u32(PYTHON_SOURCE_KIND)
    writer.u32(model)
    writer.string(module_name)
    writer.u32(len(exports))
    for function in exports:
        writer.string(function.name)
        writer.u64(function.symbol)
        writer.u32(len(function.args))
        for canonical in function.args:
            writer.string(canonical)
            writer.u64(fnv1a(canonical))
            writer.u8(VALUE_PASS)
        writer.u32(len(function.returns))
        for canonical in function.returns:
            writer.string(canonical)
            writer.u64(fnv1a(canonical))
            writer.u8(VALUE_PASS)
    writer.u32(0)  # dependencies
    writer.string("lux-python-ast")
    writer.string("1")
    writer.string(source_id)
    writer.string("")
    writer.string("")
    writer.string(entry)
    writer.u64(len(payload))
    writer.data += payload
    return bytes(writer.data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--semantic-table", required=True, type=pathlib.Path)
    parser.add_argument("--module", required=True)
    parser.add_argument("--entry", required=True)
    parser.add_argument(
        "--model",
        required=True,
        choices=("GLOBAL_MODULE", "ENTITY_BEHAVIOR"),
    )
    arguments = parser.parse_args()
    try:
        payload = arguments.source.read_bytes()
        source = payload.decode("utf-8")
        tree = ast.parse(source, filename=str(arguments.source))
        semantics = load_semantics(arguments.semantic_table)
        model = GLOBAL_MODULE if arguments.model == "GLOBAL_MODULE" else ENTITY_BEHAVIOR
        exports = collect_exports(
            tree,
            model,
            arguments.entry,
            arguments.module,
            semantics,
        )
        encoded = encode(
            arguments.module,
            model,
            arguments.entry,
            exports,
            payload,
            arguments.source.name,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_bytes(encoded)
        return 0
    except (OSError, UnicodeError, SyntaxError, ValueError) as error:
        print(f"lux-python-import: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
