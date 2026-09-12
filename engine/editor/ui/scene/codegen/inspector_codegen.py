"""Editor-only MetaUnit -> directly compiled ImGui component implementation.

This tool consumes the existing parser's JSON. It never parses C++ or writes a runtime component target.
"""
import argparse
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import subprocess


def literal(value):
    return json.dumps(str(value), ensure_ascii=False)


def symbol(name):
    return re.sub(r"\W", "_", name) + "_" + hashlib.sha256(name.encode()).hexdigest()[:8]


def annotations(decl):
    result = {}
    for attr in decl.get("attributes", []):
        # Attribute values may contain quoted commas; do not split those.
        parts = re.findall(r'(?:[^,"\']|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')+', attr)
        for part in parts:
            key, sep, value = part.strip().partition("=")
            result[key.strip()] = value.strip().strip('"') if sep else True
    return result


class Generator:
    def __init__(self, data, config):
        self.data, self.config = data, config
        self.decls = {d["id"]: d for d in data["declarations"]}
        self.types = {t["id"]: t for t in data["types"]}
        self.functions = []
        self.active = []
        self.counter = 0
        self.custom_types = set(config.get("custom_types", []))

    def resolve(self, type_id):
        if type_id not in self.types and re.fullmatch(r"(.+?)\[(\d+)\](.*)", type_id):
            self.types[type_id] = {"id": type_id, "__kind": "UnsupportedType"}
        if type_id not in self.types:
            raise ValueError(f"missing parsed type {type_id}; include its declaration in the Editor parse input")
        t = self.types[type_id]
        array = re.fullmatch(r"(.+?)\[(\d+)\](.*)", type_id)
        if t.get("__kind") == "UnsupportedType" and array:
            element = array[1] + array[3]
            return dict(t, __kind="ArrayType", element_type_id=element)
        if t.get("__kind") in ("TypedefType", "ElaboratedType"):
            for k in ("underlying_type_id", "named_type_id", "canonical_type_id"):
                if t.get(k) and t[k] != type_id:
                    return self.resolve(t[k])
        return t

    def draw_function(self, type_id, attrs=None):
        attrs = attrs or {}
        t = self.resolve(type_id)
        name = "value_" + str(self.counter)
        self.counter += 1
        typename = t["id"]
        body = self.body(t, attrs)
        self.functions.append(
            f"lux::ui::EditResult {name}(std::type_identity_t<{typename}>& value, InspectorInteraction& state)\n{{\n"
            "    lux::ui::EditResult result;\n" + "\n".join("    " + x for x in body) +
            "\n    return result;\n}\n")
        return name

    def body(self, t, attrs):
        kind, tid = t["__kind"], t["id"]
        widget = attrs.get("widget", "color" if attrs.get("color") == "true" else "default")
        if widget not in {"default", "drag", "slider", "input", "color", "asset", "enum", "readonly", "custom"}:
            raise ValueError(f"unknown widget {widget} for {tid}")
        if attrs.get("readonly") == "true" or widget == "readonly":
            # Read-only is enforced around the complete field, including custom/container operations.
            attrs = dict(attrs, widget="default", readonly="true")
            widget = "default"
        for key in ("min", "max", "speed", "step"):
            if key in attrs and not math.isfinite(float(attrs[key])):
                raise ValueError(f"non-finite {key} for {tid}")
        if "min" in attrs and "max" in attrs and float(attrs["min"]) > float(attrs["max"]):
            raise ValueError(f"reversed range for {tid}")
        if "step" in attrs and float(attrs["step"]) <= 0:
            raise ValueError(f"non-positive step for {tid}")
        if tid in self.custom_types or widget in ("custom", "asset"):
            tag = attrs.get("widget_tag", "DefaultWidgetTag")
            return [f"result = InspectorWidget<{tid}, {tag}>::draw(value, state, "
                    f"InspectorField{{\"##value\", nullptr, {attrs.get('speed', '0.1')}, "
                    f"state.read_only || {str(attrs.get('readonly') == 'true').lower()}}});"]
        if kind == "BuiltinType":
            if tid == "bool":
                if widget not in ("default", "input"):
                    raise ValueError(f"widget {widget} cannot edit bool")
                return ['result = immediate(ImGui::Checkbox("##value", &value));']
            if widget in ("color", "asset", "enum"):
                raise ValueError(f"widget {widget} cannot edit scalar {tid}")
            floating = tid in ("float", "double")
            if tid in ("void", "long double", "wchar_t", "char16_t", "char32_t"):
                raise ValueError(f"unsupported scalar {tid}; supply an Editor widget specialization")
            unsigned = "unsigned" in tid
            dtype = ("Float" if tid == "float" else "Double") if floating else (
                ("U" if unsigned else "S") + str(t["size"] * 8))
            if not floating:
                bits = t["size"] * 8
                lower, upper = (0, 2 ** bits - 1) if unsigned else (-2 ** (bits - 1), 2 ** (bits - 1) - 1)
                for key in ("min", "max", "step"):
                    if key in attrs:
                        number = Decimal(str(attrs[key]))
                        if number != int(number) or not lower <= number <= upper:
                            raise ValueError(f"{key} does not fit {tid}")
            lines = ["const auto original = value;"]
            for key, default in (("min", "0"), ("max", "0"), ("step", "1")):
                if key in attrs:
                    lines.append(f"const {tid} {key}_value = static_cast<{tid}>({attrs[key]});")
            low = "&min_value" if "min" in attrs else "nullptr"
            high = "&max_value" if "max" in attrs else "nullptr"
            fmt = '"%.17g"' if tid == "double" else ('"%.9g"' if tid == "float" else "nullptr")
            if widget == "slider":
                if "min" not in attrs or "max" not in attrs:
                    raise ValueError(f"slider requires min/max for {tid}")
                call = f'ImGui::SliderScalar("##value", ImGuiDataType_{dtype}, &value, {low}, {high}, {fmt})'
            elif widget == "input":
                step = "&step_value" if "step" in attrs else "nullptr"
                call = f'ImGui::InputScalar("##value", ImGuiDataType_{dtype}, &value, {step}, nullptr, {fmt})'
            else:
                call = (f'ImGui::DragScalar("##value", ImGuiDataType_{dtype}, &value, '
                        f'static_cast<float>({attrs.get("speed", "0.1")}), {low}, {high}, {fmt})')
            call = call.replace('"##value"', literal(attrs.get("_item_label", "##value")))
            lines.append(f"result = edited({call});")
            invalid = (["!std::isfinite(value)"] if floating else [])
            invalid += [f"value {'<' if key == 'min' else '>'} {key}_value" for key in ("min", "max") if key in attrs]
            if invalid:
                lines += ["if (result.changed && (" + " || ".join(invalid) + "))", "{",
                          '    value = original; state.fail("The value is outside the declared range.");',
                          "    result.changed = false;", "}"]
            return lines
        if kind in ("EnumType", "ScopedEnumType"):
            if widget not in ("default", "enum", "input"):
                raise ValueError(f"widget {widget} cannot edit enum {tid}")
            decl = self.decls.get(t.get("decl_id"))
            if not decl or not decl.get("enumerators"):
                raise ValueError(f"enum {tid} lacks parsed enumerators")
            lines = ['const auto unnamed = std::to_string(static_cast<std::underlying_type_t<' + tid + '>>(value));',
                     'const char* selected = unnamed.c_str();']
            for item in decl["enumerators"]:
                lines.append(f'if (value == {tid}::{item["name"]}) selected = {literal(item["name"])};')
            lines += ['if (ImGui::BeginCombo("##value", selected))', '{']
            for item in decl["enumerators"]:
                v = tid + "::" + item["name"]
                lines += [f'    if (ImGui::Selectable({literal(item["name"])}, value == {v}))',
                          f'    {{ value = {v}; result = immediate(true); }}']
            lines += ['    ImGui::EndCombo();', '}']
            return lines
        template = t.get("template_name", "")
        args = t.get("template_arguments", [])
        if template in ("std::basic_string", "std::string") or tid in ("std::string", "std::basic_string<char>"):
            if widget not in ("default", "input"):
                raise ValueError(f"widget {widget} cannot edit string")
            if args and args[0].get("type_id") != "char":
                raise ValueError(f"string {tid} requires an Editor specialization for UTF-8 conversion")
            return ['result = edited(ImGui::InputText("##value", &value));']
        if template == "Eigen::Matrix":
            scalar = args[0]["type_id"]
            rows, cols = args[1]["integral_value"], args[2]["integral_value"]
            if rows <= 0 or cols <= 0:
                raise ValueError("dynamic Eigen matrices require a custom widget")
            if widget == "color":
                if scalar != "float" or cols != 1 or rows not in (3, 4):
                    raise ValueError("color requires a fixed float3/float4 vector")
                return [f'result = edited(ImGui::ColorEdit{rows}("##value", value.data()));']
            axes = str(attrs.get("axis_labels", "X|Y|Z|W")).split("|")
            lines = []
            for r in range(rows):
                for c in range(cols):
                    label = axes[r] if cols == 1 and r < len(axes) else f"[{r},{c}]"
                    fn = self.draw_function(scalar, dict(attrs, _item_label=label))
                    lines += [f'{{ IdScope id{{{r * cols + c}}}; merge(result, {fn}(value({r}, {c}), state)); }}']
            return lines
        if template == "Eigen::Quaternion":
            if widget not in ("default", "drag", "input"):
                raise ValueError("quaternion widget must edit rotation")
            scalar = args[0]["type_id"]
            functions = [self.draw_function(scalar, dict(attrs, speed=attrs.get("speed", "0.25"),
                         _item_label=f"{axis} (deg)")) for axis in ("X", "Y", "Z")]
            return ["auto degrees = (value.toRotationMatrix().eulerAngles(0, 1, 2) *",
                    "    (180.0 / std::numbers::pi)).eval();"] + [
                    f'{{ IdScope id{{{i}}}; merge(result, {fn}(degrees[{i}], state)); }}'
                    for i, fn in enumerate(functions)] + [
                    "if (result.changed)", "{",
                    "    const auto radians = (degrees * (std::numbers::pi / 180.0)).eval();",
                    f"    using Scalar = {scalar};",
                    "    value = Eigen::Quaternion<Scalar>{",
                    "        Eigen::AngleAxis<Scalar>{radians[0], Eigen::Matrix<Scalar, 3, 1>::UnitX()} *",
                    "        Eigen::AngleAxis<Scalar>{radians[1], Eigen::Matrix<Scalar, 3, 1>::UnitY()} *",
                    "        Eigen::AngleAxis<Scalar>{radians[2], Eigen::Matrix<Scalar, 3, 1>::UnitZ()}}.normalized();", "}"]
        if widget != "default":
            raise ValueError(f"widget {widget} cannot edit aggregate {tid}; use a custom specialization")
        if kind in ("ConstantArrayType", "ArrayType") or template in ("std::array", "std::vector", "std::deque", "std::list"):
            element = args[0]["type_id"] if args else t.get("element_type_id")
            if not element:
                raise ValueError(f"array element type unavailable: {tid}")
            fn = self.draw_function(element)
            fixed = template == "std::array" or not template
            proxy = template == "std::vector" and element == "bool"
            lines = ["std::size_t index{};", "for (auto&& item : value)", "{",
                     "    IdScope id{static_cast<int>(index)};"]
            if proxy:
                lines += ["    bool bit = item;", f"    const auto edit = {fn}(bit, state);",
                          "    if (edit.changed) item = bit;", "    merge(result, edit);"]
            else:
                lines += [f"    merge(result, {fn}(item, state));"]
            if not fixed:
                lines += [ '    const bool remove = ImGui::SmallButton("Remove");', '    traceItem("Remove");', '    if (remove)', '    {',
                          "        const auto changed = prepareContainer(value, state, [&](auto& next) {",
                          "            next.erase(std::next(next.begin(), static_cast<std::ptrdiff_t>(index)));",
                          "            return true; });",
                          "        merge(result, immediate(changed));", "        return result;", "    }"]
            lines += ["    ++index;", "}"]
            if not fixed:
                lines += ['const bool add = ImGui::SmallButton("Add");', 'traceItem("Add");', 'if (add)', '{',
                          "    merge(result, immediate(prepareContainer(value, state, [](auto& next) {",
                          "        next.emplace_back(); return true; })));", "}"]
            return lines
        if template in ("std::map", "std::unordered_map", "std::set", "std::unordered_set"):
            return self.associative(t)
        if template == "std::optional":
            fn = self.draw_function(args[0]["type_id"])
            return ["bool present = value.has_value();", 'const bool toggled = ImGui::Checkbox("Present", &present);',
                    'traceItem("Present");', 'if (toggled)', '{',
                    "    merge(result, immediate(prepareContainer(value, state, [&](auto& next) {",
                    "        if (present) next.emplace(); else next.reset(); return true; })));", "}",
                    f"if (value) merge(result, {fn}(*value, state));"]
        if template in ("std::pair", "std::tuple"):
            lines = []
            for i, arg in enumerate(self.type_arguments(args)):
                fn = self.draw_function(arg)
                lines += [f'{{ IdScope id{{{i}}}; merge(result, {fn}(std::get<{i}>(value), state)); }}']
            return lines
        if template == "std::variant":
            lines = ['const auto selected = std::to_string(value.index());',
                     'const bool open = ImGui::BeginCombo("Type", selected.c_str());',
                     'traceItem("Type");', 'if (open)', '{']
            alternatives = self.type_arguments(args)
            for i, arg in enumerate(alternatives):
                lines += [f'    const bool selected_{i} = ImGui::Selectable({literal(arg)}, value.index() == {i});',
                          f'    traceItem("alternative-{i}");', f'    if (selected_{i})', '    {',
                          '        merge(result, immediate(prepareContainer(value, state, [](auto& next) {',
                          f'            next.template emplace<{i}>(); return true; }})));', '    }']
            lines += ['    ImGui::EndCombo();', '}']
            for i, arg in enumerate(alternatives):
                fn = self.draw_function(arg)
                lines += [f'if (value.index() == {i}) merge(result, {fn}(std::get<{i}>(value), state));']
            return lines
        if kind == "RecordType" and t.get("decl_id") in self.decls:
            if "luxref::class" not in annotations(self.decls[t["decl_id"]]):
                raise ValueError(f"record {tid} requires reflection annotations or an Editor specialization")
            if tid in self.active:
                raise ValueError(f"recursive ownership type {tid} requires an Editor specialization")
            self.active.append(tid)
            result = self.record(self.decls[t["decl_id"]])
            self.active.pop()
            return result
        raise ValueError(f"unsupported type {tid}; provide InspectorWidget specialization through CUSTOM_TYPES/CUSTOM_HEADERS")

    @staticmethod
    def type_arguments(args):
        result = []
        for arg in args:
            if arg.get("kind") == "type":
                result.append(arg["type_id"])
            elif arg.get("kind") == "pack":
                result.extend(Generator.type_arguments(arg.get("elements", arg.get("pack", []))))
        return result

    def associative(self, t):
        args = t["template_arguments"]
        mapping = t["template_name"] in ("std::map", "std::unordered_map")
        key = args[0]["type_id"]
        key_fn = self.draw_function(key)
        value_fn = self.draw_function(args[1]["type_id"]) if mapping else None
        lines = ['std::size_t index{};', 'for (const auto& item : value)', '{',
                 '    IdScope id{static_cast<int>(index++)};',
                 f'    auto original_key = {"item.first" if mapping else "item"};',
                 '    IdScope key_scope{0};',
                 f'    auto& key_draft = state.input<KeyDraft<{key}>>(ImGui::GetID("key-draft"));',
                 '    if (!key_draft.active) { key_draft.original = original_key; key_draft.value = original_key; }',
                 f'    const auto key_edit = {key_fn}(key_draft.value, state);',
                 '    key_draft.active |= key_edit.began || key_edit.changed;',
                 '    if (key_edit.committed && key_draft.active)', '    {',
                 '        key_draft.active = false;',
                 '        if (equivalentKey(value, key_draft.original, key_draft.value)) return result;',
                 '        const auto& edited_key = key_draft.value;',
                 '        if (!equivalentKey(value, original_key, key_draft.original))',
                 '        { state.fail("The key changed while its input was active."); return result; }',
                 '        const auto changed = prepareContainer(value, state, [&](auto& next) {',
                 '            if (next.contains(edited_key)) return false;']
        if mapping:
            lines += ['            auto mapped = next.at(original_key); next.erase(original_key);',
                      '            next.emplace(edited_key, std::move(mapped)); return true; });']
        else:
            lines += ['            next.erase(original_key); next.insert(edited_key); return true; });']
        lines += ['        merge(result, immediate(changed)); return result;', '    }']
        if mapping:
            lines += ['    IdScope mapped_scope{1};', '    auto mapped = item.second;',
                      f'    const auto mapped_edit = {value_fn}(mapped, state);',
                      '    if (mapped_edit.changed)', '    {',
                      '        const auto changed = prepareContainer(value, state, [&](auto& next) {',
                      '            next.at(original_key) = std::move(mapped); return true; });',
                      '        merge(result, mapped_edit); result.changed = changed; return result;', '    }',
                      '    merge(result, mapped_edit);']
        lines += ['    const bool remove = ImGui::SmallButton("Remove");', '    traceItem("Remove");', '    if (remove)', '    {',
                  '        merge(result, immediate(prepareContainer(value, state, [&](auto& next) {',
                  '            next.erase(original_key); return true; }))); return result;', '    }', '}',
                  'IdScope new_key_scope{-1};',
                  f'auto& new_key = state.input<{key}>(ImGui::GetID("new-key"));',
                  f'static_cast<void>({key_fn}(new_key, state));',
                  'const bool add = ImGui::SmallButton("Add");', 'traceItem("Add");', 'if (add)', '{',
                  '    merge(result, immediate(prepareContainer(value, state, [&](auto& next) {']
        lines += ['        return next.try_emplace(new_key).second; })));' if mapping else
                  '        return next.insert(new_key).second; })));', '}']
        return lines

    def record(self, decl):
        lines = []
        for field_id in decl.get("field_decls", []):
            field = self.decls[field_id]
            attrs = annotations(field)
            if field.get("visibility") != 1 or "luxref::property::skip" in attrs:
                continue
            try:
                fn = self.draw_function(field["type_id"], attrs)
            except ValueError as error:
                raise ValueError(field["fq_name"] + ": " + str(error)) from error
            label = attrs.get("display_name", field["name"])
            readonly = attrs.get("readonly") == "true" or attrs.get("widget") == "readonly"
            lines += ['{', f'    FieldScope field{{InspectorField{{{literal(label)}, nullptr, 0.1, '
                      f'{str(readonly).lower()}}}, state.read_only, {literal(field["fq_name"])}}};',
                      f'    merge(result, {fn}(value.{field["name"]}, state));']
            if "tooltip" in attrs:
                lines += [f'    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", {literal(attrs["tooltip"])});']
            lines += ['}']
        return lines

    def component(self, name):
        decl = next((d for d in self.data["declarations"] if d.get("fq_name") == name and
                     d.get("__kind") in ("CXXRecordDecl", "RecordDecl")), None)
        if not decl:
            raise ValueError(f"component not found in parsed input: {name}")
        attrs = annotations(decl)
        if attrs.get("component") != "true" or attrs.get("editor") != "true":
            raise ValueError(f"component is not editor-visible: {name}")
        suffix = symbol(name)
        fn = self.draw_function(decl["type_id"])
        includes = '\n'.join(f'#include <{h}>' for h in self.config.get('custom_headers', []))
        text = f'''// Generated Editor-only ImGui implementation for {name}.
#include "{self.config['name']}.inspector.generated.hpp"
#include <InspectorWidget.hpp>
#include <numbers>
#include <tuple>
{includes}
namespace lux::editor::ui::generated
{{
namespace
{{
using namespace generated_support;
{''.join(self.functions)}
}}
lux::ui::EditResult draw_{suffix}({name}& value, InspectorInteraction& state)
{{
    try {{ return {fn}(value, state); }}
    catch (const std::bad_alloc&) {{ state.fail("Not enough memory to prepare this input."); return {{}}; }}
}}
namespace
{{
void read_{suffix}(sessions::SceneSession& session, sessions::SceneEntityRef target, lux::ui::Frame& frame)
{{
    auto copy = session.readComponent<{name}>(target);
    if (!copy) {{ frame.textMuted("Component is unavailable at this owner window"); return; }}
    InspectorInteraction state;
    state.read_only = true;
    state.asset_source = &session;
    state.asset_path = [](void* source, lux::asset::AssetId asset) {{
        const auto result = static_cast<sessions::SceneSession*>(source)->readAssetPath(asset);
        return result && *result ? **result : std::string{{asset.isNull() ? "<none>" : "<unresolved asset>"}};
    }};
    static_cast<void>(draw_{suffix}(*copy, state));
}}
}}
ComponentReadBinding binding_{suffix}()
{{
    return {{lux::cxx::typeToken<{name}>(), {literal(attrs['schema'])}, {attrs['version']},
        {literal(attrs.get('display_name', decl['name']))}, read_{suffix}, {{}}}};
}}
}}
'''
        return suffix, text


def generate(config, data):
    if len(set(config["components"])) != len(config["components"]):
        raise ValueError("duplicate component in one Editor generation job")
    outputs = {}
    declarations, bindings = [], []
    for component in config["components"]:
        generator = Generator(data, config)
        suffix, text = generator.component(component)
        outputs[suffix + ".inspector.generated.cpp"] = text
        declarations += [f"lux::ui::EditResult draw_{suffix}({component}&, InspectorInteraction&);",
                         f"ComponentReadBinding binding_{suffix}();"]
        bindings.append(f"binding_{suffix}()")
    outputs[config["name"] + ".inspector.generated.hpp"] = (
        '#pragma once\n#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>\n'
        '#include <lux/engine/editor/ui/scene/InspectorInteraction.hpp>\n#include <array>\n'
        f'#include <{config["logical_path"]}>\nnamespace lux::editor::ui::generated\n{{\n' +
        '\n'.join(declarations) + f'\ninline auto {config["name"]}Bindings()\n{{\n'
        '    return std::array{' + ', '.join(bindings) + '};\n}\n}\n')
    return outputs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--ir", required=True)
    parser.add_argument("--meta-generator")
    parser.add_argument("--meta-config")
    args = parser.parse_args()
    # The installed parser emits JSON beside its projection but does not declare it in its output list.
    # This Editor rule owns that sidecar dependency and can recover a removed sidecar without stale input.
    if not Path(args.ir).exists() and args.meta_generator and args.meta_config:
        subprocess.run([args.meta_generator, args.meta_config], check=True)
    config = json.loads(Path(args.config).read_text(encoding="utf-8-sig"))
    data = json.loads(Path(args.ir).read_text(encoding="utf-8-sig"))
    outputs = generate(config, data)  # All semantic validation completes before publishing any output.
    root = Path(config["output_root"])
    root.mkdir(parents=True, exist_ok=True)
    for name, text in outputs.items():
        path = root / name
        encoded = text.encode("utf-8")
        if not path.exists() or path.read_bytes() != encoded:
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_bytes(encoded)
            temporary.replace(path)
    print(f"Editor Inspector: {len(config['components'])} component implementation(s); no runtime target output")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"EDITOR_INSPECTOR_CODEGEN: {error}", file=sys.stderr)
        sys.exit(1)
