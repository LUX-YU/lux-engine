"""DataSchema projection from the existing lux-cxx IR; never parses C++ text."""

import json
from pathlib import Path
import sys

# Passed explicitly by source and installed build-tool wiring.
sys.path.insert(
    0, sys.argv[4] if len(sys.argv) > 4 else str(Path(__file__).resolve().parents[5] / "engine/toolchain/shader/cmake")
)
from GpuValues import value_declarations, gpu_layout


def generate(ir, header):
    types = {t["id"]: t for t in ir["types"]}
    decls = {d["id"]: d for d in ir["declarations"]}
    for field in ir["fields"]:
        if field and (
            field["count"] > 1
            or set(field["arguments"]) - {"shape"}
            or (field["count"] and not field["arguments"].get("shape"))
        ):
            raise ValueError("duplicate/unknown GPU field annotation")
    shapes = {f["id"]: f["arguments"]["shape"] for f in ir["fields"] if f and f["arguments"]}
    if any(not isinstance(shape, str) for shape in shapes.values()):
        raise ValueError("GPU shape must be an explicit string")
    roots = [r for r in ir["records"] if r and r["schema"]]
    if not roots:
        raise ValueError("missing LUX_RENDER_DATA")
    out = [
        "#pragma once",
        '#include "' + Path(header).name + '"',
        "#include <lux/engine/render/data/Schema.hpp>",
        "#include <array>",
        "#include <cstdint>",
        "#include <cstring>",
        "#include <tuple>",
        "#include <type_traits>",
        "namespace lux::render {",
    ]
    glsl_out = {}
    for root in roots:
        if root["count"] != 1:
            raise ValueError("exactly one data annotation required")
        d = decls[root["id"]]
        a = root["arguments"]
        cpp = "::" + d["fq_name"]
        t = types[d["type_id"]]
        allowed = {"name", "version", "scope", "authority", "cpu", "wire", "wire_version", "gpu"}
        if set(a) - allowed:
            raise ValueError("unsupported schema key")
        name, version = a.get("name"), a.get("version")
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(version, int)
            or isinstance(version, bool)
            or version <= 0
        ):
            raise ValueError("canonical name and positive schema version required")
        scope, authority = a.get("scope"), a.get("authority")
        if scope not in ("runtime", "scene", "view") or authority not in ("cpu", "gpu", "external"):
            raise ValueError("invalid scope/authority")
        gpu, wire = a.get("gpu"), a.get("wire")
        if "gpu" in a and gpu not in ("std140", "std430"):
            raise ValueError("GPU storage must be std140 or std430")
        if "wire" in a and (not isinstance(wire, str) or not wire):
            raise ValueError("Wire codec must be a nonempty string")
        cpu = a.get("cpu", authority == "cpu" and not wire)
        if not isinstance(cpu, bool):
            raise ValueError("cpu must be boolean")
        if not cpu and not wire and not gpu:
            raise ValueError("no representation")
        if authority == "gpu" and (cpu or not gpu):
            raise ValueError("GPU authority has no CPU mirror")
        if wire and (
            not isinstance(wire, str)
            or not isinstance(a.get("wire_version"), int)
            or isinstance(a["wire_version"], bool)
            or a["wire_version"] <= 0
        ):
            raise ValueError("wire requires codec and positive version")
        if not wire and "wire_version" in a:
            raise ValueError("wire version without codec")
        if d.get("bases") or not d["field_decls"]:
            raise ValueError("empty/inherited schema unsupported")
        fields = [decls[i] for i in d["field_decls"]]
        if any(f["visibility"] != 1 or f["offset"] < 0 or f["offset"] % 8 for f in fields):
            raise ValueError("schema fields must be public and byte-addressable")
        out += [
            "template <> struct DataSchema<" + cpp + "> {",
            "using Value = " + cpp + ";",
            "inline static constexpr bool validated = true;",
            "inline static constexpr std::array<DataField, " + str(len(fields)) + "> fields{{",
        ]
        out += ["{" + json.dumps(f["name"]) + ", " + json.dumps(types[f["type_id"]]["name"]) + "}," for f in fields]
        out += [
            "}};",
            "inline static constexpr auto members = std::tuple{"
            + ", ".join("&Value::" + f["name"] for f in fields)
            + "};",
        ]
        cpu_expr = (
            (
                "DataCpuRepresentation{sizeof(Value), alignof(Value), std::is_move_constructible_v<Value>, "
                "std::is_copy_constructible_v<Value>, std::is_trivially_copyable_v<Value>}"
            )
            if cpu
            else "std::nullopt"
        )
        wire_expr = (
            ("DataWireRepresentation{" + json.dumps(wire) + ", " + str(a["wire_version"]) + "}")
            if wire
            else "std::nullopt"
        )
        if gpu:
            abi, structs = [], {}
            scalar_type, _ = value_declarations(types, decls, abi, structs, shapes)
            shader_type = scalar_type(d["type_id"])
            size, alignment, leaves, copies = gpu_layout(types, decls, d["type_id"], gpu, shapes)
            declarations = "\n".join(structs.values())
            identity = json.dumps([gpu, size, alignment, leaves], separators=(",", ":"))
            out += abi
            out += ["inline static constexpr std::array<rdesc::DataGpuField, " + str(len(leaves)) + "> gpu_fields{{"]
            out += [
                "{" + ", ".join([json.dumps(p), "rdesc::EDataScalar::" + k, *map(str, [o, r, c, ar, m])]) + "},"
                for p, k, o, r, c, ar, m in leaves
            ]
            out += [
                "}};",
                "static constexpr rdesc::DataGpuLayout gpuLayout() noexcept {",
                "return {rdesc::EDataLayout::"
                + gpu.upper()
                + ", "
                + str(size)
                + ", "
                + str(alignment)
                + ", gpu_fields,",
                'R"LUX('
                + declarations
                + ')LUX", R"LUX('
                + identity
                + ')LUX", cxx::Fnv1a64::hash(R"LUX('
                + identity
                + ')LUX")};',
                "}",
                'inline static constexpr std::string_view shader_type = "' + shader_type + '";',
            ]
            if authority == "cpu":
                out += [
                    "static RenderResult<void> pack(const Value& value, std::span<std::byte> output) noexcept {",
                    "if (output.size() != " + str(size) + ") { return cxx::unexpected(RenderError{kInvalidLayout}); }",
                    "const auto source = reinterpret_cast<std::uintptr_t>(&value);",
                    "const auto destination = reinterpret_cast<std::uintptr_t>(output.data());",
                    "const bool is_overlap = destination >= source ? destination - source < sizeof(Value) : source - destination < output.size();",
                    "if (is_overlap) { return cxx::unexpected(RenderError{kInvalidLayout}); }",
                    "std::memset(output.data(), 0, output.size());",
                ]
                out += ["std::memcpy(output.data() + " + str(o) + ", &value." + p + ", 4);" for p, o in copies]
                out += ["return {};", "}"]
            glsl_out.update(structs)
        out += [
            "static constexpr DataSchemaDescriptor descriptor() noexcept {",
            "return {renderDataTypeId(" + json.dumps(name) + "), " + json.dumps(name) + ", " + str(version) + ",",
            "EDataScope::" + scope.upper() + ", EDataAuthority::" + authority.upper() + ", fields,",
            cpu_expr + ", " + wire_expr + ", " + ("gpuLayout()" if gpu else "std::nullopt") + "};",
            "}",
            "};",
        ]
    out += ["}"]
    return "\n".join(out) + "\n", "\n".join(glsl_out.values()) + "\n"


def main():
    ir, header, prefix = sys.argv[1:4]
    try:
        cpp, glsl = generate(json.loads(Path(ir).read_text(encoding="utf-8-sig")), header)
    except (ValueError, KeyError, TypeError) as e:
        print("DataSchema rejected: " + str(e), file=sys.stderr)
        return 1
    Path(prefix + ".data.hpp").write_text(cpp, encoding="utf-8")
    Path(prefix + ".lglslh").write_text(glsl, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
