#!/usr/bin/env python3
"""Package statically described Lua exports as canonical LXSA v8."""

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
WIRE_VERSION = 9
SCHEMA_VERSION = 11
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


def make_semantics(
    record_specs: list[str], value_specs: list[str]
) -> dict[str, Semantic]:
    layouts = (
        ("lux.bool", 1, 1, 1),
        ("lux.i32", 2, 4, 4),
        ("lux.u32", 3, 4, 4),
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
    for specification in value_specs:
        parts = specification.split(",")
        if len(parts) != 4:
            raise ValueError(
                "--value-type must be canonical-name,abi-kind,size,alignment"
            )
        canonical, abi_kind_text, size_text, alignment_text = parts
        abi_kind = int(abi_kind_text, 0)
        size = int(size_text, 0)
        alignment = int(alignment_text, 0)
        if (
            not canonical
            or canonical in result
            or abi_kind < 1
            or abi_kind > 7
            or abi_kind in (4, 5)
            or size <= 0
            or size > 0xFFFFFFFF
            or alignment <= 0
            or alignment > 0xFFFFFFFF
            or alignment & (alignment - 1)
        ):
            raise ValueError("invalid --value-type")
        result[canonical] = Semantic(
            canonical,
            fnv1a(canonical),
            VALUE_PASS,
            True,
            abi_kind,
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
    lifecycle: str | None
    coroutine: bool


@dataclass(frozen=True)
class AbilitySchema:
    contract: str
    contract_id: int
    name: str
    display_name: str
    schema_version: int
    schema_hash: int


@dataclass(frozen=True)
class EventSource:
    system_name: str
    event_name: str
    system_id: int
    event_id: int
    route: int
    payload: Semantic
    payload_schema_hash: int
    payload_schema_version: int


@dataclass(frozen=True)
class PackageDescription:
    exports: list[Export]
    begin_play: int
    end_play: int
    suspension_capable_exports: list[int]
    requirements: list[AbilitySchema]
    event_sources: list[EventSource]


FUNCTION = re.compile(
    r"^\s*(?:local\s+)?function\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)(?:([:.])([A-Za-z_][A-Za-z0-9_]*))?"
    r"\s*\(([^)]*)\)"
)
PARAM = re.compile(
    r"^\s*---@param\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\S+)\s*$"
)
RETURN = re.compile(r"^\s*---@return\s+(\S+)\s*$")
REQUIRE = re.compile(r"^\s*---@lux\.requires\s+(\S+)\s*$")
EVENT = re.compile(
    r"^\s*---@lux\.event\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*$"
)
LIFECYCLE = re.compile(r"^\s*---@lux\.lifecycle\s+(begin_play|end_play)\s*$")
COROUTINE = re.compile(r"^\s*---@lux\.coroutine\s*$")


def code_identifier(value: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value))


def load_ability_schemas(paths: list[pathlib.Path]) -> dict[str, AbilitySchema]:
    result: dict[str, AbilitySchema] = {}
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("schema") != "lux-script-ability" or document.get("version") != 2:
            raise ValueError(f"unsupported Ability schema manifest '{path}'")
        abilities = document.get("abilities")
        if not isinstance(abilities, list):
            raise ValueError(f"Ability schema manifest '{path}' has no abilities")
        for value in abilities:
            contract = value.get("contract")
            contract_id = int(value.get("contract_id", 0))
            name = value.get("name")
            display_name = value.get("display_name")
            schema_version = int(value.get("schema_version", 0))
            schema_hash = int(value.get("schema_hash", 0))
            if (
                not isinstance(contract, str)
                or not contract
                or contract_id != fnv1a(contract)
                or not isinstance(name, str)
                or not code_identifier(name)
                or not isinstance(display_name, str)
                or schema_version <= 0
                or schema_hash <= 0
                or schema_hash > 0xFFFFFFFFFFFFFFFF
            ):
                raise ValueError(f"invalid Ability schema in '{path}'")
            schema = AbilitySchema(
                contract,
                contract_id,
                name,
                display_name,
                schema_version,
                schema_hash,
            )
            previous = result.get(contract)
            if previous is not None and previous != schema:
                raise ValueError(f"conflicting Ability schema for '{contract}'")
            result[contract] = schema
    return result


def load_event_schemas(paths: list[pathlib.Path]) -> dict[tuple[str, str], EventSource]:
    result: dict[tuple[str, str], EventSource] = {}
    identities: dict[tuple[int, int], EventSource] = {}
    routes = {"simulation_broadcast": 0, "entity_targeted": 1}
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("schema") != "lux-script-event" or document.get("version") != 1:
            raise ValueError(f"unsupported Event schema manifest '{path}'")
        events = document.get("events")
        if not isinstance(events, list):
            raise ValueError(f"Event schema manifest '{path}' has no events")
        for value in events:
            payload_value = value.get("payload")
            if not isinstance(payload_value, dict):
                raise ValueError(f"invalid Event payload schema in '{path}'")
            system_name = value.get("system_name")
            event_name = value.get("event_name")
            system_id = int(value.get("system_id", 0))
            event_id = int(value.get("event_id", 0))
            route_name = value.get("route")
            canonical = payload_value.get("canonical_name")
            type_id = int(payload_value.get("type_id", 0))
            abi_kind = int(payload_value.get("abi_kind", 0))
            size = int(payload_value.get("size", 0))
            alignment = int(payload_value.get("alignment", 0))
            schema_hash = int(value.get("payload_schema_hash", 0))
            schema_version = int(value.get("payload_schema_version", 0))
            is_invalid = (
                not isinstance(system_name, str)
                or not code_identifier(system_name)
                or not isinstance(event_name, str)
                or not code_identifier(event_name)
                or system_id <= 0
                or system_id > 0xFFFFFFFFFFFFFFFF
                or event_id <= 0
                or event_id > 0xFFFFFFFFFFFFFFFF
                or route_name not in routes
                or not isinstance(canonical, str)
                or not canonical
                or type_id != fnv1a(canonical)
                or abi_kind <= 0
                or abi_kind > 10
                or size <= 0
                or size > 0xFFFFFFFF
                or alignment <= 0
                or alignment > 0xFFFFFFFF
                or alignment & (alignment - 1)
                or schema_hash <= 0
                or schema_hash > 0xFFFFFFFFFFFFFFFF
                or schema_version <= 0
                or schema_version > 0xFFFFFFFF
            )
            if is_invalid:
                raise ValueError(f"invalid Event schema in '{path}'")
            payload = Semantic(canonical, type_id, VALUE_PASS, True, abi_kind, size, alignment)
            source = EventSource(
                system_name,
                event_name,
                system_id,
                event_id,
                routes[route_name],
                payload,
                schema_hash,
                schema_version,
            )
            source_key = (system_name, event_name)
            identity_key = (system_id, event_id)
            previous_source = result.get(source_key)
            previous_identity = identities.get(identity_key)
            if (previous_source is not None and previous_source != source) or (
                previous_identity is not None and previous_identity != source
            ):
                raise ValueError(f"conflicting Event schema in '{path}'")
            result[source_key] = source
            identities[identity_key] = source
    return result


def collect_exports(
    source: str,
    scope: str,
    entry: str,
    module_name: str,
    semantics: dict[str, Semantic],
    symbols_by_source: dict[str, int],
    ability_schemas: dict[str, AbilitySchema],
    event_schemas: dict[tuple[str, str], EventSource],
) -> PackageDescription:
    marked = False
    parameters: list[tuple[str, str]] = []
    return_name: str | None = None
    lifecycle: str | None = None
    coroutine = False
    exports: list[Export] = []
    symbols: set[int] = set()
    requirement_names: list[str] = []
    seen_requirements: set[str] = set()
    event_sources: list[EventSource] = []
    seen_event_sources: set[EventSource] = set()
    for line_number, line in enumerate(source.splitlines(), 1):
        stripped = line.strip()
        if match := REQUIRE.match(line):
            contract = match.group(1)
            if marked or exports:
                raise ValueError(
                    f"line {line_number}: @lux.requires must precede all exports"
                )
            if contract in seen_requirements:
                raise ValueError(f"line {line_number}: duplicate Ability requirement")
            if contract not in ability_schemas:
                raise ValueError(
                    f"line {line_number}: unknown Ability requirement '{contract}'"
                )
            seen_requirements.add(contract)
            requirement_names.append(contract)
            continue
        if match := EVENT.match(line):
            if marked or exports:
                raise ValueError(
                    f"line {line_number}: @lux.event must precede all exports"
                )
            source_key = (match.group(1), match.group(2))
            source = event_schemas.get(source_key)
            if source is None:
                raise ValueError(
                    f"line {line_number}: unknown Event source '{source_key[0]}.{source_key[1]}'"
                )
            if source in seen_event_sources:
                raise ValueError(f"line {line_number}: duplicate Event source")
            seen_event_sources.add(source)
            event_sources.append(source)
            continue
        if stripped == "---@lux.method":
            if marked:
                raise ValueError(f"line {line_number}: nested @lux.method annotation")
            marked = True
            parameters = []
            return_name = None
            lifecycle = None
            coroutine = False
            continue
        if match := LIFECYCLE.match(line):
            if not marked or lifecycle is not None:
                raise ValueError(f"line {line_number}: invalid lifecycle annotation")
            lifecycle = match.group(1)
            continue
        if COROUTINE.match(line):
            if not marked or coroutine:
                raise ValueError(f"line {line_number}: invalid coroutine annotation")
            coroutine = True
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
        exports.append(
            Export(
                name,
                symbol,
                argument_types,
                return_types,
                lifecycle,
                coroutine,
            )
        )
        marked = False
        parameters = []
        return_name = None
        lifecycle = None
        coroutine = False
    if marked:
        raise ValueError("unterminated @lux.method annotation")
    if not exports:
        raise ValueError("no ---@lux.method exports found")
    begin_play = 0
    end_play = 0
    suspension_capable_exports: list[int] = []
    for export in exports:
        if export.coroutine and export.returns:
            raise ValueError(f"{export.name}: coroutine export must return void")
        if export.lifecycle is not None and export.coroutine:
            raise ValueError(f"{export.name}: lifecycle export cannot be coroutine-capable")
        if export.lifecycle == "begin_play":
            if begin_play != 0:
                raise ValueError("duplicate BeginPlay lifecycle role")
            if export.args or export.returns:
                raise ValueError("BeginPlay lifecycle signature must be void()")
            begin_play = export.symbol
        elif export.lifecycle == "end_play":
            if end_play != 0:
                raise ValueError("duplicate EndPlay lifecycle role")
            has_reason = len(export.args) == 1 and (
                export.args[0].canonical == "lux.simulation.ScriptEndPlayReason"
                and export.args[0].default_pass == VALUE_PASS
            )
            if not has_reason or export.returns:
                raise ValueError(
                    "EndPlay lifecycle signature must be void(lux.simulation.ScriptEndPlayReason)"
                )
            end_play = export.symbol
        if export.coroutine:
            suspension_capable_exports.append(export.symbol)
    if begin_play != 0 and begin_play == end_play:
        raise ValueError("one Script symbol cannot own both lifecycle roles")
    suspension_capable_exports.sort()
    requirements = sorted(
        (ability_schemas[name] for name in requirement_names),
        key=lambda value: value.contract,
    )
    event_sources.sort(
        key=lambda value: (
            value.system_id,
            value.event_id,
            value.system_name,
            value.event_name,
        )
    )
    return PackageDescription(
        exports,
        begin_play,
        end_play,
        suspension_capable_exports,
        requirements,
        event_sources,
    )


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
    package: PackageDescription,
    payload: bytes,
    source_id: str,
) -> bytes:
    writer = Writer()
    writer.u32(MAGIC)
    writer.u32(WIRE_VERSION)
    writer.u32(SCHEMA_VERSION)
    writer.u32(LUA_SOURCE_KIND)
    writer.string(module_name)
    writer.u64(package.begin_play)
    writer.u64(package.end_play)
    writer.u32(len(package.exports))
    for function in package.exports:
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
    writer.u32(len(package.requirements))
    for requirement in package.requirements:
        writer.string(requirement.contract)
        writer.u64(requirement.contract_id)
        writer.u64(requirement.schema_hash)
    writer.u32(len(package.event_sources))
    for source in package.event_sources:
        writer.string(source.system_name)
        writer.string(source.event_name)
        writer.u64(source.system_id)
        writer.u64(source.event_id)
        writer.u32(source.route)
        writer.string(source.payload.canonical)
        writer.u64(source.payload.type_id)
        writer.u32(source.payload.abi_kind)
        writer.u32(source.payload.size)
        writer.u32(source.payload.alignment)
        writer.u64(source.payload_schema_hash)
        writer.u32(source.payload_schema_version)
    writer.string("lux-lua-static")
    writer.string("2")
    writer.string(source_id)
    writer.string("")
    writer.string("")
    writer.string(entry)
    writer.u32(len(package.suspension_capable_exports))
    for symbol in package.suspension_capable_exports:
        writer.u64(symbol)
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
    parser.add_argument("--value-type", action="append", default=[])
    parser.add_argument("--ability-schema", action="append", default=[], type=pathlib.Path)
    parser.add_argument("--event-schema", action="append", default=[], type=pathlib.Path)
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
        semantics = make_semantics(arguments.record_type, arguments.value_type)
        ability_schemas = load_ability_schemas(arguments.ability_schema)
        event_schemas = load_event_schemas(arguments.event_schema)
        symbols_by_source = load_symbol_ledger(arguments.symbol_ledger)
        package = collect_exports(
            source,
            arguments.scope,
            arguments.entry,
            arguments.module,
            semantics,
            symbols_by_source,
            ability_schemas,
            event_schemas,
        )
        inner = encode(
            arguments.module,
            arguments.entry,
            package,
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
