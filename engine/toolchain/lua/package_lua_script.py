#!/usr/bin/env python3
"""Package statically described Lua ---@lux.method exports as canonical LXSA v5."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys
from dataclasses import dataclass


MAGIC = 0x4153584C
WIRE_VERSION = 5
SCHEMA_VERSION = 7
LUA_SOURCE_KIND = 1
SIMULATION_SCOPE = "SIMULATION"
ENTITY_SCOPE = "ENTITY"
VALUE_PASS = 0
CONST_REF_PASS = 1
OUTER_VERSION = 20260606
OUTER_HEADER_SIZE = 400
NO_LEGACY_TYPE_TAG = 0xFFFFFFFF


def fnv1a(text: str) -> int:
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


@dataclass(frozen=True)
class Semantic:
    canonical: str
    type_id: int
    default_pass: int
    return_allowed: bool
    abi_kind: int
    size: int
    alignment: int


def make_semantics(record_specs: list[str]) -> dict[str, Semantic]:
    layouts = (
        ("lux.bool", 1, 1, 1),
        ("lux.i32", 2, 4, 4),
        ("lux.u32", 3, 4, 4),
        ("lux.i64", 4, 8, 8),
        ("lux.u64", 5, 8, 8),
        ("lux.f32", 6, 4, 4),
        ("lux.f64", 7, 8, 8),
    )
    result = {
        canonical: Semantic(
            canonical,
            fnv1a(canonical),
            VALUE_PASS,
            True,
            abi_kind,
            size,
            alignment,
        )
        for canonical, abi_kind, size, alignment in layouts
    }
    for specification in record_specs:
        parts = specification.split(",")
        if len(parts) != 3:
            raise ValueError(
                "--record-type must be canonical-name,size,alignment"
            )
        canonical, size_text, alignment_text = parts
        size = int(size_text, 0)
        alignment = int(alignment_text, 0)
        if (
            not canonical
            or canonical in result
            or size <= 0
            or size > 0xFFFFFFFF
            or alignment <= 0
            or alignment > 0xFFFFFFFF
            or alignment & (alignment - 1)
        ):
            raise ValueError("invalid --record-type")
        result[canonical] = Semantic(
            canonical,
            fnv1a(canonical),
            CONST_REF_PASS,
            False,
            10,
            size,
            alignment,
        )
    return result


def load_symbol_ledger(path: pathlib.Path) -> dict[str, int]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != "lux-script-symbol-ledger" or document.get("version") != 1:
        raise ValueError("unsupported script symbol ledger")
    entries = document.get("entries")
    if not isinstance(entries, list):
        raise ValueError("script symbol ledger has no entries")
    result: dict[str, int] = {}
    symbols: set[int] = set()
    for value in entries:
        source_identity = value.get("source_identity")
        symbol = int(value.get("symbol", 0))
        if (
            not isinstance(source_identity, str)
            or not source_identity
            or symbol <= 0
            or symbol > 0xFFFFFFFFFFFFFFFF
            or source_identity in result
            or symbol in symbols
        ):
            raise ValueError("invalid script symbol ledger entry")
        result[source_identity] = symbol
        symbols.add(symbol)
    return result


@dataclass(frozen=True)
class Export:
    name: str
    symbol: int
    args: list[Semantic]
    returns: list[Semantic]


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
    scope: str,
    entry: str,
    module_name: str,
    semantics: dict[str, Semantic],
    symbols_by_source: dict[str, int],
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
        if scope == ENTITY_SCOPE:
            if owner != entry or separator != ":" or not member:
                raise ValueError(
                    f"line {line_number}: entity script export requires '{entry}:method' colon syntax"
                )
            name = member
            source_identity = f"{entry}:{name}"
        else:
            if separator or member:
                raise ValueError(
                    f"line {line_number}: global export must be a module function"
                )
            name = owner
            source_identity = f"{module_name}:{name}"
        arguments = [value.strip() for value in raw_arguments.split(",") if value.strip()]
        if "..." in arguments:
            raise ValueError(f"{name}: variadic parameters are unsupported")
        if [value[0] for value in parameters] != arguments:
            raise ValueError(f"{name}: every parameter requires an ordered @param")
        argument_names = [value[1] for value in parameters]
        argument_types: list[Semantic] = []
        for canonical in argument_names:
            if canonical not in semantics:
                raise ValueError(f"{name}: unsupported parameter type '{canonical}'")
            argument_types.append(semantics[canonical])
        if return_name is None:
            raise ValueError(f"{name}: explicit @return required (use void)")
        return_types: list[Semantic] = []
        if return_name != "void" and (
            return_name not in semantics or not semantics[return_name].return_allowed
        ):
            raise ValueError(f"{name}: unsupported return type '{return_name}'")
        if return_name != "void":
            return_types.append(semantics[return_name])
        symbol = symbols_by_source.get(source_identity, 0)
        if symbol == 0:
            raise ValueError(f"{name}: source identity '{source_identity}' is absent from symbol ledger")
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
    writer.string(module_name)
    writer.u64(0)
    writer.u64(0)
    writer.u32(len(exports))
    for function in exports:
        writer.string(function.name)
        writer.u64(function.symbol)
        writer.u32(len(function.args))
        for semantic in function.args:
            writer.string(semantic.canonical)
            writer.u64(semantic.type_id)
            writer.u8(semantic.default_pass)
            writer.u8(semantic.abi_kind)
            writer.u32(semantic.size)
            writer.u32(semantic.alignment)
        writer.u32(len(function.returns))
        for semantic in function.returns:
            writer.string(semantic.canonical)
            writer.u64(semantic.type_id)
            writer.u8(VALUE_PASS)
            writer.u8(semantic.abi_kind)
            writer.u32(semantic.size)
            writer.u32(semantic.alignment)
    writer.u32(0)
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


def wrap_typed_asset(payload: bytes, module_name: str, source_id: str) -> bytes:
    digest = bytearray(hashlib.sha256(payload).digest()[:16])
    digest[6] = (digest[6] & 0x0F) | 0x40
    digest[8] = (digest[8] & 0x3F) | 0x80
    display = module_name.encode("utf-8")[:63]
    source = source_id.encode("utf-8")[:255]
    header = bytearray(OUTER_HEADER_SIZE)
    struct.pack_into(
        "<IIQQQQ",
        header,
        0,
        MAGIC,
        OUTER_VERSION,
        OUTER_HEADER_SIZE,
        0,
        OUTER_HEADER_SIZE,
        len(payload),
    )
    header[40:56] = digest
    struct.pack_into("<I", header, 56, NO_LEGACY_TYPE_TAG)
    header[72 : 72 + len(display)] = display
    header[136 : 136 + len(source)] = source
    return bytes(header) + payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--record-type", action="append", default=[])
    parser.add_argument("--symbol-ledger", required=True, type=pathlib.Path)
    parser.add_argument("--module", required=True)
    parser.add_argument("--entry", required=True)
    parser.add_argument(
        "--scope", required=True, choices=(SIMULATION_SCOPE, ENTITY_SCOPE)
    )
    arguments = parser.parse_args()
    try:
        payload = arguments.source.read_bytes()
        source = payload.decode("utf-8")
        semantics = make_semantics(arguments.record_type)
        symbols_by_source = load_symbol_ledger(arguments.symbol_ledger)
        exports = collect_exports(
            source,
            arguments.scope,
            arguments.entry,
            arguments.module,
            semantics,
            symbols_by_source,
        )
        inner = encode(
            arguments.module,
            arguments.entry,
            exports,
            payload,
            arguments.source.name,
        )
        encoded = wrap_typed_asset(
            inner,
            arguments.module,
            arguments.source.name,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_bytes(encoded)
        return 0
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        print(f"lux-lua-import: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
