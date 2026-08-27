#!/usr/bin/env python3
"""Static Lua ---@lux.method -> canonical LXSA v2 importer."""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys
from dataclasses import dataclass


MAGIC = 0x4153584C
WIRE_VERSION = 2
SCHEMA_VERSION = 4
LUA_SOURCE_KIND = 1
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
    result = {
        match.group(1)
        for line in path.read_text(encoding="utf-8").splitlines()
        if (match := pattern.match(line))
    }
    if not result:
        raise ValueError("semantic SSOT contains no builtin entries")
    return result


@dataclass(frozen=True)
class Export:
    name: str
    symbol: int
    args: list[str]
    returns: list[str]


FUNCTION = re.compile(
    r"^\s*(?:local\s+)?function\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)(?:([:.])([A-Za-z_][A-Za-z0-9_]*))?"
    r"\s*\(([^)]*)\)"
)
PARAM = re.compile(
    r"^\s*---@param\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\S+)\s*$"
)
RETURN = re.compile(r"^\s*---@return\s+(\S+)\s*$")


def collect_exports(
    source: str,
    model: int,
    entry: str,
    module_name: str,
    semantics: set[str],
) -> list[Export]:
    marked = False
    parameters: list[tuple[str, str]] = []
    return_name: str | None = None
    exports: list[Export] = []
    symbols: set[int] = set()
    for line_number, line in enumerate(source.splitlines(), 1):
        stripped = line.strip()
        if stripped == "---@lux.method":
            marked = True
            parameters = []
            return_name = None
            continue
        if marked and (match := PARAM.match(line)):
            parameters.append((match.group(1), match.group(2)))
            continue
        if marked and (match := RETURN.match(line)):
            if return_name is not None:
                raise ValueError(f"line {line_number}: multiple returns unsupported")
            return_name = match.group(1)
            continue
        match = FUNCTION.match(line)
        if not match:
            if marked and stripped and not stripped.startswith("---"):
                raise ValueError(
                    f"line {line_number}: @lux.method must immediately describe a function"
                )
            continue
        if not marked:
            continue
        owner, separator, member, raw_arguments = match.groups()
        if model == ENTITY_BEHAVIOR:
            if owner != entry or separator not in (":", ".") or not member:
                raise ValueError(
                    f"line {line_number}: EntityBehavior export must belong to '{entry}'"
                )
            name = member
            scope = f"{module_name}.{entry}"
        else:
            if separator or member:
                raise ValueError(
                    f"line {line_number}: global export must be a module function"
                )
            name = owner
            scope = module_name
        arguments = [value.strip() for value in raw_arguments.split(",") if value.strip()]
        if "..." in arguments:
            raise ValueError(f"{name}: variadic parameters are unsupported")
        if [value[0] for value in parameters] != arguments:
            raise ValueError(f"{name}: every parameter requires an ordered @param")
        argument_types = [value[1] for value in parameters]
        for canonical in argument_types:
            if canonical not in semantics:
                raise ValueError(f"{name}: unsupported parameter type '{canonical}'")
        if return_name is None:
            raise ValueError(f"{name}: explicit @return required (use void)")
        return_types = [] if return_name == "void" else [return_name]
        if return_types and return_types[0] not in semantics:
            raise ValueError(f"{name}: unsupported return type '{return_name}'")
        symbol = symbol_id(scope, name, argument_types, return_types)
        if symbol in symbols:
            raise ValueError(f"{name}: duplicate semantic symbol")
        symbols.add(symbol)
        exports.append(Export(name, symbol, argument_types, return_types))
        marked = False
        parameters = []
        return_name = None
    if marked:
        raise ValueError("unterminated @lux.method annotation")
    if not exports:
        raise ValueError("no ---@lux.method exports found")
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
    writer.u32(LUA_SOURCE_KIND)
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
    writer.u32(0)
    writer.string("lux-lua-static")
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
        "--model", required=True, choices=("GLOBAL_MODULE", "ENTITY_BEHAVIOR")
    )
    arguments = parser.parse_args()
    try:
        payload = arguments.source.read_bytes()
        source = payload.decode("utf-8")
        semantics = load_semantics(arguments.semantic_table)
        model = GLOBAL_MODULE if arguments.model == "GLOBAL_MODULE" else ENTITY_BEHAVIOR
        exports = collect_exports(
            source, model, arguments.entry, arguments.module, semantics
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
    except (OSError, UnicodeError, ValueError) as error:
        print(f"lux-lua-import: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
