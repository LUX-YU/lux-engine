"""Lower the existing lux-cxx metadata IR. Never reads or parses C++ source.

Annotations have already been parsed by the production meta generator. The
projection is deliberately finite: unsupported field/layout shapes fail here.
"""
import json
from pathlib import Path
import sys

ROLES = {
    "sampled_read": ("SampledTexture", "SAMPLED_READ"),
    "storage_read": ("StorageTexture", "STORAGE_READ"),
    "storage_write": ("StorageTexture", "STORAGE_WRITE"),
    "storage_read_write": ("StorageTexture", "STORAGE_READ_WRITE"),
    "sampler": ("SamplerHandle", "SAMPLER"),
    "uniform_read": ("UniformBuffer", "UNIFORM_READ"),
    "read_only_storage": ("StorageBuffer", "READ_ONLY_STORAGE"),
    "read_write_storage": ("StorageBuffer", "READ_WRITE_STORAGE"),
    "color_attachment": ("Attachment", "COLOR_ATTACHMENT"),
    "depth_stencil": ("DepthStencilAttachment", "DEPTH_STENCIL"),
    "resolve": ("ResolveAttachment", "RESOLVE"),
    "transfer_source": (("TransferTexture", "TransferBuffer"), "TRANSFER_SOURCE"),
    "transfer_destination": (("TransferTexture", "TransferBuffer"), "TRANSFER_DESTINATION"),
    "vertex": ("VertexBuffer", "VERTEX"),
    "index": ("IndexBuffer", "INDEX"),
    "indirect": ("IndirectBuffer", "INDIRECT"),
    "input_attachment": ("SampledTexture", "INPUT_ATTACHMENT"),
}
BUILTINS = {"float": ("float", "FLOAT"), "int": ("int", "INT"), "unsigned int": ("uint", "UINT")}


def generate(ir, header):
    types = {t["id"]: t for t in ir["types"]}
    decls = {d["id"]: d for d in ir["declarations"]}
    attrs = {f["id"]: f for f in ir["fields"] if f}
    roots = [decls[r["id"]] for r in ir["records"] if r and r["params"]]
    if len(roots) != 1:
        raise ValueError("exactly one LUX_PASS_PARAMS root per header required")
    root = roots[0]
    cpp = root["fq_name"]
    resources, scalars, capture, abi, glsl = [], [], [], [], []
    shader_structs = {}
    declaration_stages = []

    def scalar_type(tid):
        t = types[tid]
        if t["name"] in BUILTINS:
            if t["size"] != 4:
                raise ValueError("shader scalar must be 32 bits")
            return BUILTINS[t["name"]][0]
        if t["__kind"] == "RecordType" and t.get("decl_id"):
            d = decls[t["decl_id"]]
            name = "Lux_" + d["fq_name"].replace("::", "_")
            abi.append("static_assert(sizeof(::" + d["fq_name"] + ") == " + str(t["size"]) + ");")
            abi.append("static_assert(alignof(::" + d["fq_name"] + ") == " + str(t["align"]) + ");")
            if name not in shader_structs:
                if d.get("bases") or not d["field_decls"]:
                    raise ValueError("block must be a nonempty field-only value")
                members = []
                for fid in d["field_decls"]:
                    f = decls[fid]
                    abi.append("static_assert(offsetof(::" + d["fq_name"] + ", " + f["name"] + ") == " + str(f["offset"] // 8) + ");")
                    members.append("    " + scalar_declaration(f["type_id"], f["name"]) + ";")
                shader_structs[name] = "struct " + name + " {\n" + "\n".join(members) + "\n};"
            return name
        raise ValueError("unsupported shader value type: " + tid)

    def scalar_declaration(tid, name):
        t = types[tid]
        if t["__kind"] == "ArrayType":
            return scalar_declaration(t["element_type_id"], name + "[" + str(t["array_size"]) + "]")
        return scalar_type(tid) + " " + name

    def leaves(tid, path, offset, block=False, stack=(), stride=0, count=1, metadata=None):
        t = types[tid]
        if tid in stack:
            raise ValueError("recursive value layout")
        if t["__kind"] == "ArrayType":
            e = types[t["element_type_id"]]
            if e["__kind"] == "ArrayType":
                raise ValueError("use nested value structs instead of multidimensional C arrays")
            for i in range(t["array_size"]):
                leaves(e["id"], path + "[" + str(i) + "]", offset + i * e["size"], block,
                       (*stack, tid), e["size"], t["array_size"], metadata)
        elif t["name"] in BUILTINS:
            options = metadata or {}
            owner = options.get("scope") or "pass_local"
            frequency = options.get("frequency") or "frame"
            stages = options.get("stages", 7)
            if owner not in ("scene", "feature", "pass_local") or frequency not in ("static", "frame", "draw"):
                raise ValueError("invalid scalar ownership/frequency")
            scalars.append((path, BUILTINS[t["name"]][1], offset, t["size"], stride, count, block,
                            owner.upper(), frequency.upper(), stages))
            if not block:
                capture.append("        detail::captureScalar(result, " + str(offset) + ", parameters." + path + ");")
        elif t["__kind"] == "RecordType" and t.get("decl_id"):
            d = decls[t["decl_id"]]
            if d.get("bases") or not d["field_decls"]:
                raise ValueError("unsupported empty/inherited scalar block " + tid)
            for fid in d["field_decls"]:
                f = decls[fid]
                if f["offset"] < 0 or f["offset"] % 8:
                    raise ValueError("invalid field layout " + f["fq_name"])
                leaves(f["type_id"], path + "." + f["name"], offset + f["offset"] // 8, block, (*stack, tid), stride, count, metadata)
        else:
            raise ValueError("unsafe borrow or unsupported field " + path + ": " + tid)

    def resource(f, t, path, count, options, is_array):
        allowed = {"role", "scope", "frequency", "semantic", "for", "dimension", "format", "required", "stages"}
        if options["annotation_count"] != 1 or options["role_count"] != 1:
            raise ValueError("resource must have exactly one annotation and role: " + path)
        if set(options["arguments"]) - allowed:
            raise ValueError("unsupported resource annotation key: " + path)
        role = options["role"]
        if count <= 0:
            raise ValueError("zero/unbounded descriptor array is not an author value")
        if role not in ROLES:
            raise ValueError("unknown or conflicting resource role: " + role)
        expected, enum = ROLES[role]
        expected = (expected,) if isinstance(expected, str) else expected
        family = t.get("template_name", t["name"])
        if family not in ["lux::render::" + n for n in expected]:
            raise ValueError("role/type mismatch at " + path + ": " + family)
        scope = options["scope"] or "pass_local"
        frequency = options["frequency"] or "frame"
        if scope not in ("scene", "feature", "pass_local") or frequency not in ("static", "frame", "draw"):
            raise ValueError("invalid field owner/frequency: " + path)
        if role == "resolve" and not options["for"]:
            raise ValueError("resolve requires for=<color attachment>")
        required = options["required"]
        if not isinstance(required, bool):
            raise ValueError("required must be boolean")
        name = "p_" + path.replace(".", "_")
        suffix = "[" + str(count) + "]" if is_array else ""
        declaration_begin = len(glsl)
        alignment = 1
        stride = 0
        qualifier = ""
        if role == "sampler":
            if not options["for"]:
                raise ValueError("sampler requires explicit for=<sampled field>")
            glsl.append("uniform sampler " + name + suffix + ";")
        elif role in ("sampled_read", "storage_read", "storage_write", "storage_read_write", "input_attachment"):
            dim = options["dimension"] or "2D"
            if dim not in ("1D", "1DArray", "2D", "3D", "Cube", "2DArray", "CubeArray", "2DMS", "2DMSArray"):
                raise ValueError("invalid image dimension " + dim)
            if role == "sampled_read":
                glsl.append("uniform texture" + dim + " " + name + suffix + ";")
            elif role == "input_attachment":
                index = sum(1 for value in resources if value[2] == "INPUT_ATTACHMENT")
                glsl.append("layout(input_attachment_index=" + str(index) + ") uniform subpassInput " + name + suffix + ";")
            else:
                fmt = options["format"]
                if fmt not in ("r32f", "rgba16f", "rgba32f", "rgba8"):
                    raise ValueError("storage image requires supported format")
                qualifier = {"storage_read": "readonly ", "storage_write": "writeonly ", "storage_read_write": ""}[role]
                glsl.append("layout(" + fmt + ") " + qualifier + "uniform image" + dim + " " + name + suffix + ";")
        elif role in ("uniform_read", "read_only_storage", "read_write_storage"):
            args = t.get("template_arguments", [])
            if len(args) != 1 or args[0].get("kind") != "type":
                raise ValueError("buffer requires one reflected element type")
            tid = args[0]["type_id"]
            stride = types[tid]["size"]
            alignment = max(16 if role == "uniform_read" else 1, types[tid]["align"])
            ty = scalar_type(tid)
            leaves(tid, path + ".data", 0, True, metadata=options)
            if role == "uniform_read":
                glsl.append("layout(std140) uniform LuxBlock_" + name + " { " + ty + " data; } " + name + suffix + ";")
            else:
                qualifier = "readonly " if role == "read_only_storage" else ""
                glsl.append("layout(std430) " + qualifier + "buffer LuxBlock_" + name + " { " + ty + " data[]; } " + name + suffix + ";")
        resources.append((path, name, enum, scope.upper(), frequency.upper(), required, count, stride, options["semantic"], options["for"], options["dimension"] or "2D", options["format"], options["stages"], is_array, alignment))
        declaration_stages.extend([options["stages"]] * (len(glsl) - declaration_begin))
        for i in range(count):
            access = path + ("[" + str(i) + "]" if is_array else "")
            capture.append("        detail::captureResource(result, parameters." + access + ", resources[" + str(len(resources) - 1) + "], " + str(i) + ");")

    def walk(d, prefix="", offset=0):
        if d.get("bases"):
            raise ValueError("Params inheritance is not a supported field shape")
        for fid in d["field_decls"]:
            f = decls[fid]
            if f["visibility"] != 1 or f["offset"] < 0 or f["offset"] % 8:
                raise ValueError("Params fields must have public byte-addressable layout")
            t = types[f["type_id"]]
            path = prefix + f["name"]
            absolute = offset + f["offset"] // 8
            options = attrs[fid]
            if options["resource"] and options["role"] == "push_constant":
                if (options["annotation_count"] != 1 or options["role_count"] != 1 or not options["required"] or
                    set(options["arguments"]) - {"role", "scope", "frequency", "stages"}):
                    raise ValueError("invalid push constant annotation")
                scalar_declaration(f["type_id"], f["name"])
                leaves(f["type_id"], path, absolute, metadata=options)
            elif options["resource"]:
                count = 1
                if t["__kind"] == "ArrayType":
                    count = t["array_size"]
                    t = types[t["element_type_id"]]
                resource(f, t, path, count, options, types[f["type_id"]]["__kind"] == "ArrayType")
            elif t["__kind"] == "RecordType" and t.get("decl_id"):
                nested = decls[t["decl_id"]]
                # Scalar blocks and resource groups both recursively retain field paths.
                walk(nested, path + ".", absolute)
            else:
                scalar_type_id = f["type_id"]
                scalar_declaration(scalar_type_id, f["name"])
                leaves(scalar_type_id, path, absolute)
            abi.append("static_assert(offsetof(::" + d["fq_name"] + ", " + f["name"] + ") == " + str(f["offset"] // 8) + ");")

    walk(root)
    names = {f[0]: f for f in resources}
    for f in resources:
        if f[2] == "SAMPLER" and (f[9] not in names or names[f[9]][2] != "SAMPLED_READ"):
            raise ValueError("sampler pairing does not name a sampled field")
    for f in resources:
        if f[2] == "RESOLVE" and (f[9] not in names or names[f[9]][2] != "COLOR_ATTACHMENT"):
            raise ValueError("resolve pairing does not name a color attachment")
    def declarations_for(stage):
        pc = []
        for path, kind, offset, size, stride, count, block, owner, frequency, stages in scalars:
            if not block and stages & stage:
                name = "p_" + path.replace(".", "_").replace("[", "_").replace("]", "")
                pc.append("    layout(offset=" + str(offset) + ") " + kind.lower() + " " + name + ";")
        result = "\n".join(shader_structs.values()) + "\n" + "\n".join(
            line for line, stages in zip(glsl, declaration_stages) if stages & stage)
        if pc:
            result += "\nlayout(push_constant, std430) uniform LuxPush {\n" + "\n".join(pc) + "\n} lux_push;\n"
        return result

    declarations = declarations_for(7)
    stage_declarations = [declarations_for(stage) for stage in (1, 2, 4)]
    rt = types[root["type_id"]]
    text = ['// Generated from lux-cxx IR; do not edit.', '#pragma once', '#include "' + Path(header).name + '"', '#include <lux/engine/render/graph/Schema.hpp>', *abi,
            'static_assert(sizeof(::' + cpp + ') == ' + str(rt['size']) + ');',
            'static_assert(alignof(::' + cpp + ') == ' + str(rt['align']) + ');',
            'static_assert(std::is_standard_layout_v<::' + cpp + '>);',
            'namespace lux::render', '{', '    template <>', '    struct PassSchema<::' + cpp + '>', '    {',
            '    static constexpr std::uint32_t version = 1;',
            '    inline static constexpr std::array<rdesc::PassResourceField, ' + str(len(resources)) + '> resources{{']
    for path, name, role, owner, frequency, required, count, stride, semantic, paired, dimension, image_format, stages, is_array, alignment in resources:
        text += ['        {', '            ' + ',\n            '.join([json.dumps(path), json.dumps(name), 'rdesc::EPassFieldRole::' + role, 'rdesc::EFieldOwner::' + owner, 'rdesc::EUpdateFrequency::' + frequency, str(required).lower(), str(count), str(stride), json.dumps(semantic), json.dumps(paired), json.dumps(dimension), json.dumps(image_format), str(stages), str(is_array).lower(), str(alignment)]), '        },']
    text += ['    }};', '    inline static constexpr std::array<rdesc::PassScalarField, ' + str(len(scalars)) + '> scalars{{']
    for path, kind, offset, size, stride, count, block, owner, frequency, stages in scalars:
        text += ['        {', '            ' + ',\n            '.join([json.dumps(path), 'rdesc::EScalarKind::' + kind, *map(str, [offset,size,stride,count]), 'rdesc::EFieldOwner::' + owner, 'rdesc::EUpdateFrequency::' + frequency, str(stages)]), '        },']
    text += ['    }};', '', '    static constexpr rdesc::PassShaderContract contract() noexcept', '    {',
             '        return {' + json.dumps(cpp) + ', resources, scalars, R"LUX(' + declarations + ')LUX", ' + str(rt['size']) + ', ' + str(rt['align']) + ', {' + ', '.join('R"LUX(' + value + ')LUX"' for value in stage_declarations) + '}};', '    }',
             '', '    static detail::CapturedParameters capture(const ::' + cpp + '& parameters) noexcept', '    {',
             '        detail::CapturedParameters result;', '        result.scalars.resize(' + str(rt['size']) + ');', *capture, '        return result;', '    }', '};', '}']
    start = text.index('    template <>') + 3
    for index in range(start, len(text) - 2):
        text[index] = "\n".join("    " + line if line else "" for line in text[index].split("\n"))
    text[-2] = "    };"
    return "\n".join(text) + "\n", declarations


def main():
    ir_path, header, prefix = sys.argv[1:]
    try:
        cpp, shader = generate(json.loads(Path(ir_path).read_text()), header)
    except (ValueError, KeyError, TypeError) as error:
        print("PassSchema rejected: " + str(error), file=sys.stderr)
        return 1
    Path(prefix + ".pass.hpp").write_text(cpp, encoding="utf-8")
    Path(prefix + ".lglslh").write_text(shader, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
