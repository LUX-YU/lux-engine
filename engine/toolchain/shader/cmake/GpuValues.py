"""Existing metadata IR value declarations shared by PassSchema and DataSchema.
No C++ parsing; unsupported types fail at the generator boundary.
"""

BUILTINS = {"float": ("float", "FLOAT"), "int": ("int", "INT"), "unsigned int": ("uint", "UINT")}


def value_declarations(types, decls, abi, shader_structs, shapes=None):
    shapes = shapes or {}

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
                    abi.append(
                        "static_assert(offsetof(::"
                        + d["fq_name"]
                        + ", "
                        + f["name"]
                        + ") == "
                        + str(f["offset"] // 8)
                        + ");"
                    )
                    members.append("    " + scalar_declaration(f["type_id"], f["name"], shapes.get(fid, "")) + ";")
                shader_structs[name] = "struct " + name + " {\n" + "\n".join(members) + "\n};"
            return name
        raise ValueError("unsupported shader value type: " + tid)

    def scalar_declaration(tid, name, shape=""):
        t = types[tid]
        if shape:
            glsl, _, _ = shaped_type(types, tid, shape)
            return glsl + " " + name
        if t.get("template_name") == "std::array":
            args = t["template_arguments"]
            return scalar_declaration(args[0]["type_id"], name + "[" + str(args[1]["integral_value"]) + "]")
        if t["__kind"] == "ArrayType":
            return scalar_declaration(t["element_type_id"], name + "[" + str(t["array_size"]) + "]")
        return scalar_type(tid) + " " + name

    return scalar_type, scalar_declaration


def array_type(types, tid):
    t = types[tid]
    if t["__kind"] == "ArrayType":
        return t["element_type_id"], t["array_size"]
    if t.get("template_name") == "std::array":
        a = t["template_arguments"]
        return a[0]["type_id"], a[1]["integral_value"]
    return None


def shaped_type(types, tid, shape):
    # Explicit field annotation, not inference from sizeof or array shape.
    if shape in ("vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4"):
        rows, columns = int(shape[-1]), 1
    elif shape in ("mat2", "mat3", "mat4"):
        rows = columns = int(shape[-1])
    else:
        raise ValueError("unsupported explicit GPU shape: " + shape)
    a = array_type(types, tid)
    if not a or a[1] != (columns if columns > 1 else rows):
        raise ValueError("shape/C++ array mismatch")
    scalar = a[0]
    if columns > 1:
        a = array_type(types, scalar)
        if not a or a[1] != rows:
            raise ValueError("matrix requires column-major nested array")
        scalar = a[0]
    expected = "int" if shape.startswith("ivec") else "unsigned int" if shape.startswith("uvec") else "float"
    if types[scalar]["name"] != expected or types[scalar]["size"] != 4:
        raise ValueError("shape/scalar mismatch")
    return shape, rows, columns


def gpu_layout(types, decls, tid, storage, shapes):
    if storage not in ("std140", "std430"):
        raise ValueError("GPU storage must be std140 or std430")

    def align(n, a):
        return (n + a - 1) // a * a

    def visit(tid, path, shape="", stack=()):
        if tid in stack:
            raise ValueError("recursive GPU field")
        t = types[tid]
        if shape or t["name"] in BUILTINS:
            rows, columns = shaped_type(types, tid, shape)[1:] if shape else (1, 1)
            scalar = (
                "SINT"
                if shape.startswith("ivec") or t["name"] == "int"
                else "UINT"
                if shape.startswith("uvec") or t["name"] == "unsigned int"
                else "FLOAT"
            )
            alignment = 4 if rows == 1 else 8 if rows == 2 else 16
            stride = 0
            if columns > 1:
                alignment = max(alignment, 16) if storage == "std140" else alignment
                stride = align(rows * 4, alignment)
            size = columns * stride if columns > 1 else rows * 4
            copies = []
            for c in range(columns):
                for r in range(rows):
                    suffix = f"[{c}][{r}]" if columns > 1 else f"[{r}]" if shape else ""
                    copies.append((path + suffix, c * stride + r * 4))
            return size, alignment, [(path, scalar, 0, rows, columns, 0, stride)], copies
        a = array_type(types, tid)
        if a:
            if array_type(types, a[0]):
                raise ValueError("multidimensional arrays require explicit matrix shape or nested value records")
            if a[1] <= 0:
                raise ValueError("GPU array must have fixed positive capacity")
            size, alignment, _, _ = visit(a[0], path + "[0]", stack=(*stack, tid))
            if storage == "std140":
                alignment = max(alignment, 16)
            stride = align(size, alignment)
            fields, copies = [], []
            for i in range(a[1]):
                _, _, nested, moves = visit(a[0], path + f"[{i}]", stack=(*stack, tid))
                fields += [(p, k, o + i * stride, r, c, stride if not ar else ar, m) for p, k, o, r, c, ar, m in nested]
                copies += [(p, o + i * stride) for p, o in moves]
            return stride * a[1], alignment, fields, copies
        if t["__kind"] != "RecordType" or not t.get("decl_id"):
            raise ValueError("GPU nested record must be explicitly marked; unsupported type: " + tid)
        d = decls[t["decl_id"]]
        if d.get("bases") or not d["field_decls"]:
            raise ValueError("GPU record must be a nonempty field-only value")
        end, alignment, fields, copies = 0, 1, [], []
        for fid in d["field_decls"]:
            f = decls[fid]
            if f["visibility"] != 1 or f["offset"] < 0 or f["offset"] % 8:
                raise ValueError("GPU field must be accessible and byte-addressable")
            p = path + "." + f["name"] if path else f["name"]
            size, a, nested, moves = visit(f["type_id"], p, shapes.get(fid, ""), (*stack, tid))
            end = align(end, a)
            fields += [(p, k, o + end, r, c, ar, m) for p, k, o, r, c, ar, m in nested]
            copies += [(p, o + end) for p, o in moves]
            end += size
            alignment = max(alignment, a)
        if storage == "std140":
            alignment = max(alignment, 16)
        return align(end, alignment), alignment, fields, copies

    return visit(tid, "")
