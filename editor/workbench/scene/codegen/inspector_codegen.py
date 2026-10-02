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
        self.counter = 0
        self.custom_elements = dict(item.split("=", 1) for item in config.get("custom_elements", []))
        self.custom_types = set(config.get("custom_types", [])) | self.custom_elements.keys()

    def resolve(self, type_id):
        if type_id not in self.types and re.fullmatch(r"(.+?)\[(\d+)\](.*)", type_id):
            self.types[type_id] = {"id": type_id, "__kind": "UnsupportedType"}
        if type_id not in self.types:
            raise ValueError(f"missing parsed type {type_id}; include its declaration in the Editor parse input")
        t = self.types[type_id]
        array = re.fullmatch(r"(.+?)\[(\d+)\](.*)", type_id)
        if t.get("__kind") == "UnsupportedType" and array:
            element = array[1] + array[3]
            return dict(t, __kind="ArrayType", element_type_id=element, array_size=int(array[2]))
        if t.get("__kind") in ("TypedefType", "ElaboratedType"):
            for k in ("underlying_type_id", "named_type_id", "canonical_type_id"):
                if t.get(k) and t[k] != type_id:
                    return self.resolve(t[k])
        return t

    def draw_function(self, type_id, attrs=None):
        attrs = attrs or {}
        t = self.resolve(type_id)
        attrs = self.validate_field(t, attrs)
        name = "value_" + str(self.counter)
        self.counter += 1
        typename = t["id"]
        body = self.body(t, attrs)
        aggregates = {"std::array", "std::vector", "std::deque", "std::list", "std::map", "std::unordered_map",
                      "std::set", "std::unordered_set", "std::optional", "std::variant", "std::pair", "std::tuple"}
        aggregate = (t.get("template_name") in aggregates or t.get("__kind") in ("ConstantArrayType", "ArrayType") or
                     (t.get("__kind") == "RecordType" and
                      "luxref::class" in annotations(self.decls.get(t.get("decl_id"), {}))))
        if not aggregate or typename in self.custom_types or attrs.get("widget") in ("asset", "custom"):
            body.insert(0, "ReadOnlyScope read_only{state.readOnly()};")
        body = [re.sub(r'ImGui::SmallButton\(("[^"]*")\)', r'readOnlyButton(\1, state)', line) for line in body]
        self.functions.append(
            f"lux::ui::EditResult {name}(std::type_identity_t<{typename}>& value, InspectorInteraction& state)\n{{\n"
            "    lux::ui::EditResult result;\n" + "\n".join("    " + x for x in body) +
            "\n    return result;\n}\n")
        return name

    @staticmethod
    def scalar_row(items, tooltip=None):
        lines = ['{', '    ImGui::BeginGroup();',
                 '    const auto& style = ImGui::GetStyle();', '    const float labels_width =']
        for i, (label, _, _) in enumerate(items):
            lines.append(f'        ImGui::CalcTextSize({literal(label)}).x' +
                         (';' if i + 1 == len(items) else ' +'))
        lines += [f'    const float spacing = style.ItemInnerSpacing.x * {len(items)}.0F +',
                  f'        style.ItemSpacing.x * {len(items) - 1}.0F;',
                  '    const float item_width = std::max(1.0F,',
                  f'        (ImGui::GetContentRegionAvail().x - labels_width - spacing) / {len(items)}.0F);']
        for i, (_, call, identity) in enumerate(items):
            if i:
                lines.append('    ImGui::SameLine();')
            lines += ['    {', f'        IdScope id{{{identity}}};',
                      '        ImGui::SetNextItemWidth(item_width);', f'        merge(result, {call});']
            if tooltip:
                lines += ['        if (ImGui::IsItemHovered())', f'            ImGui::SetTooltip({literal(tooltip)});']
            lines.append('    }')
        return lines + ['    ImGui::EndGroup();', '}']

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
            tag = attrs.get("widget_tag", "DefaultControlTag")
            return [f"result = TInspectorControl<{tid}, {tag}>::draw(value, state, "
                    f"InspectorField{{\"##value\", nullptr, {attrs.get('speed', '0.1')}, "
                    f"state.readOnly() || {str(attrs.get('readonly') == 'true').lower()}}});"]
        if kind == "BuiltinType":
            if tid == "bool":
                if widget not in ("default", "input"):
                    raise ValueError(f"widget {widget} cannot edit bool")
                return ['const auto original = value;', 'result = immediate(ImGui::Checkbox("##value", &value));', 'result.changed = state.changed(value, original, result.changed);']
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
            lines += ["result.changed = state.changed(value, original, result.changed);"]
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
                          f'    {{ if (!state.readOnly()) {{ value = {v}; result = immediate(true); }} }}']
            lines += ['    ImGui::EndCombo();', '}']
            return lines
        template = t.get("template_name", "")
        args = t.get("template_arguments", [])
        if template in ("std::basic_string", "std::string") or tid in ("std::string", "std::basic_string<char>"):
            if widget not in ("default", "input"):
                raise ValueError(f"widget {widget} cannot edit string")
            if args and args[0].get("type_id") != "char":
                raise ValueError(f"string {tid} requires an Editor specialization for UTF-8 conversion")
            return ['result = editString("##value", value, state);']
        if template == "Eigen::Matrix":
            scalar = args[0]["type_id"]
            rows, cols = args[1]["integral_value"], args[2]["integral_value"]
            if rows <= 0 or cols <= 0:
                raise ValueError("dynamic Eigen matrices require a custom widget")
            if widget == "color":
                if scalar != "float" or cols != 1 or rows not in (3, 4):
                    raise ValueError("color requires a fixed float3/float4 vector")
                return ['const auto original = value;', f'result = edited(ImGui::ColorEdit{rows}("##value", value.data()));', 'result.changed = state.changed(value, original, result.changed);']
            axes = str(attrs.get("axis_labels", "X|Y|Z|W")).split("|")
            lines = []
            vector = rows == 1 or cols == 1
            items = []
            for r in range(rows):
                for c in range(cols):
                    index = r * cols + c
                    label = axes[index] if vector and index < len(axes) else f"[{r},{c}]"
                    fn = self.draw_function(scalar, dict(attrs, _item_label=label))
                    items.append((label, f'{fn}(value({r}, {c}), state)', index))
                if not vector:
                    lines += self.scalar_row(items)
                    items = []
            if vector:
                lines += self.scalar_row(items)
            return lines
        if template == "Eigen::Quaternion":
            if widget not in ("default", "drag", "input"):
                raise ValueError("quaternion widget must edit rotation")
            scalar = args[0]["type_id"]
            functions = [self.draw_function(scalar, dict(attrs, speed=attrs.get("speed", "0.25"),
                         _item_label=axis)) for axis in ("X", "Y", "Z")]
            return ["auto degrees = (value.toRotationMatrix().eulerAngles(0, 1, 2) *",
                    "    (180.0 / std::numbers::pi)).eval();"] + self.scalar_row([
                    (axis, f'{fn}(degrees[{i}], state)', i)
                    for i, (axis, fn) in enumerate(zip(("X", "Y", "Z"), functions))], "Degrees") + [
                    "if (result.changed)", "{",
                    "    const auto radians = (degrees * (std::numbers::pi / 180.0)).eval();",
                    f"    using Scalar = {scalar};",
                    "    value = Eigen::Quaternion<Scalar>{",
                    "        Eigen::AngleAxis<Scalar>{radians[0], Eigen::Matrix<Scalar, 3, 1>::UnitX()} *",
                    "        Eigen::AngleAxis<Scalar>{radians[1], Eigen::Matrix<Scalar, 3, 1>::UnitY()} *",
                    "        Eigen::AngleAxis<Scalar>{radians[2], Eigen::Matrix<Scalar, 3, 1>::UnitZ()}}.normalized();", "}"]
        if widget != "default":
            raise ValueError(f"widget {widget} cannot edit aggregate {tid}; use a custom specialization")
        if tid == "std::monostate":
            return ['ImGui::TextUnformatted("Empty");']
        raise ValueError(f"unsupported type {tid}; provide TInspectorControl specialization through CUSTOM_TYPES/CUSTOM_HEADERS")

    @staticmethod
    def type_arguments(args):
        result = []
        for arg in args:
            if arg.get("kind") == "type":
                result.append(arg["type_id"])
            elif arg.get("kind") == "pack":
                result.extend(Generator.type_arguments(arg.get("elements", arg.get("pack", []))))
        return result

    def fixed_children(self, resolved, properties):
        tid, kind = resolved['id'], resolved.get('__kind')
        if tid in self.custom_types or properties.get('widget') in ('asset', 'custom'):
            return None
        template, args = resolved.get('template_name', ''), resolved.get('template_arguments', [])
        if kind in ('ConstantArrayType', 'ArrayType') or template == 'std::array':
            element = args[0]['type_id'] if args else resolved.get('element_type_id')
            count = args[1]['integral_value'] if args else resolved.get('array_size', resolved.get('size_value'))
            if not element or count is None: raise ValueError(f'fixed array extent unavailable: {tid}')
            return [(element, properties, f'[{i}]', f'[{i}]', '({})[' + str(i) + ']') for i in range(int(count))]
        if template in ('std::pair', 'std::tuple'):
            return [(item, {}, f'/{i}', f'[{i}]', 'std::get<' + str(i) + '>({})')
                    for i, item in enumerate(self.type_arguments(args))]
        if template == 'Eigen::Matrix' and properties.get('widget') != 'color' and properties.get('color') != 'true':
            rows, cols = args[1]['integral_value'], args[2]['integral_value']
            if rows <= 0 or cols <= 0: raise ValueError('dynamic Eigen matrices require an explicit control')
            axes = str(properties.get('axis_labels', 'X|Y|Z|W')).split('|')
            children = []
            for row in range(rows):
                for column in range(cols):
                    index = row * cols + column
                    label = axes[index] if (rows == 1 or cols == 1) and index < len(axes) else f'[{row},{column}]'
                    children.append((args[0]['type_id'], properties, f'[{row},{column}]', label,
                                     '({})(' + str(row) + ', ' + str(column) + ')'))
            return children
        record = self.decls.get(resolved.get('decl_id'), {})
        if kind == 'RecordType' and 'luxref::class' in annotations(record):
            children = []
            for field_id in record.get('field_decls', []):
                field = self.decls[field_id]
                attrs = annotations(field)
                if field.get('visibility') != 1 or 'luxref::property::skip' in attrs: continue
                children.append((field['type_id'], attrs, '.' + field['name'],
                                 attrs.get('display_name', field['name']), '({}).' + field['name']))
            return children
        return None

    def aggregate_element(self, component, resolved, access):
        tid, template = resolved['id'], resolved.get('template_name', '')
        args = resolved.get('template_arguments', [])
        if template in ('std::vector', 'std::deque', 'std::list'):
            child = self.element_factory(component, args[0]['type_id'])
            return f'TSequenceFieldElement<InspectorInteraction, {component}, {tid}, {access}, {child}>', ''
        if template in ('std::optional', 'std::variant'):
            optional = template == 'std::optional'
            values = [args[0]['type_id']] if optional else self.type_arguments(args)
            factories = ', '.join(self.element_factory(component, item) for item in values)
            options = '{0, "Absent"}, {1, "Present"}' if optional else ', '.join(
                '{' + f'{index}, {literal(str(index) + ": " + item)}' + '}' for index, item in enumerate(values))
            return f'TAlternativeFieldElement<InspectorInteraction, {component}, {tid}, {access}, {str(optional).lower()}, {factories}>', ', std::vector<lux::ui::ChoiceOption>{' + options + '}'
        if template in ('std::map', 'std::unordered_map', 'std::set', 'std::unordered_set'):
            key = self.resolve(args[0]['type_id'])
            scalar_key = key.get('__kind') in ('BuiltinType', 'EnumType', 'ScopedEnumType') or key.get('template_name') in ('std::basic_string', 'std::string')
            if not scalar_key: raise ValueError('Associative keys require a scalar/string or an explicit custom Element: ' + tid)
            mapping = template in ('std::map', 'std::unordered_map')
            child = self.element_factory(component, args[1]['type_id']) if mapping else 'void'
            return f'TAssociativeFieldElement<InspectorInteraction, {component}, {tid}, {access}, {child}>', ''
        return None

    def validate_field(self, t, attrs, stack=()):
        """Check the parsed field without allocating symbols or emitting any C++."""
        attrs = dict(attrs)
        kind, tid = t['__kind'], t['id']
        widget = attrs.get('widget', 'color' if attrs.get('color') == 'true' else 'default')
        if widget not in {'default', 'drag', 'slider', 'input', 'color', 'asset', 'enum', 'readonly', 'custom'}:
            raise ValueError(f'unknown widget {widget} for {tid}')
        if attrs.get('readonly') == 'true' or widget == 'readonly':
            widget = 'default'
            attrs['readonly'] = 'true'
        attrs['widget'] = widget
        for key in ('min', 'max', 'speed', 'step'):
            if key in attrs and not math.isfinite(float(attrs[key])):
                raise ValueError(f'non-finite {key} for {tid}')
        if 'min' in attrs and 'max' in attrs and Decimal(str(attrs['min'])) > Decimal(str(attrs['max'])):
            raise ValueError(f'reversed range for {tid}')
        if 'step' in attrs and Decimal(str(attrs['step'])) <= 0:
            raise ValueError(f'non-positive step for {tid}')
        if tid in self.custom_types or widget in ('custom', 'asset'):
            return attrs
        if tid == 'std::monostate': return attrs
        if tid in stack:
            raise ValueError(f'recursive ownership type {tid} requires an Editor specialization')
        nested = stack + (tid,)
        def check(child, properties=None):
            self.validate_field(self.resolve(child), properties or {}, nested)
        if kind == 'BuiltinType':
            if tid == 'bool':
                if widget not in ('default', 'input'): raise ValueError(f'widget {widget} cannot edit bool')
            else:
                if widget not in ('default', 'drag', 'slider', 'input'):
                    raise ValueError(f'widget {widget} cannot edit scalar {tid}')
                if tid in ('void', 'long double', 'wchar_t', 'char16_t', 'char32_t'):
                    raise ValueError(f'unsupported scalar {tid}')
                if widget == 'slider' and ('min' not in attrs or 'max' not in attrs):
                    raise ValueError(f'slider requires min/max for {tid}')
                if tid not in ('float', 'double'):
                    bits = t['size'] * 8
                    low, high = (0, 2**bits-1) if 'unsigned' in tid else (-2**(bits-1), 2**(bits-1)-1)
                    for key in ('min', 'max', 'step'):
                        if key in attrs:
                            number = Decimal(str(attrs[key]))
                            if number != int(number) or not low <= number <= high:
                                raise ValueError(f'{key} does not fit {tid}')
            return attrs
        if kind in ('EnumType', 'ScopedEnumType'):
            if widget not in ('default', 'enum', 'input'): raise ValueError(f'widget {widget} cannot edit enum {tid}')
            if not self.decls.get(t.get('decl_id'), {}).get('enumerators'):
                raise ValueError(f'enum {tid} lacks parsed enumerators')
            return attrs
        template, args = t.get('template_name', ''), t.get('template_arguments', [])
        if template in ('std::basic_string', 'std::string') or tid in ('std::string', 'std::basic_string<char>'):
            if widget not in ('default', 'input'): raise ValueError(f'widget {widget} cannot edit string')
            if args and args[0].get('type_id') != 'char': raise ValueError(f'string {tid} requires UTF-8 conversion')
        elif template == 'Eigen::Matrix':
            rows, cols = args[1]['integral_value'], args[2]['integral_value']
            if rows <= 0 or cols <= 0: raise ValueError('dynamic Eigen matrices require an explicit control')
            if widget == 'color':
                if args[0]['type_id'] != 'float' or cols != 1 or rows not in (3, 4):
                    raise ValueError('color requires a fixed float3/float4 vector')
            else: check(args[0]['type_id'], attrs)
        elif template == 'Eigen::Quaternion':
            check(args[0]['type_id'])
        elif kind in ('ConstantArrayType', 'ArrayType'):
            check(t['element_type_id'], attrs)
        elif template in ('std::array', 'std::vector', 'std::deque', 'std::list', 'std::optional'):
            check(args[0]['type_id'])
        elif template in ('std::variant', 'std::pair', 'std::tuple'):
            for child in self.type_arguments(args): check(child)
        elif template in ('std::map', 'std::unordered_map', 'std::set', 'std::unordered_set'):
            check(args[0]['type_id'])
            if template in ('std::map', 'std::unordered_map'): check(args[1]['type_id'])
        elif kind == 'RecordType' and 'luxref::class' in annotations(self.decls.get(t.get('decl_id'), {})):
            for field_id in self.decls[t['decl_id']].get('field_decls', []):
                field = self.decls[field_id]
                properties = annotations(field)
                if field.get('visibility') == 1 and 'luxref::property::skip' not in properties:
                    check(field['type_id'], properties)
        else:
            raise ValueError(f'unsupported type {tid}; provide TInspectorControl specialization')
        return attrs

    def control_plan(self, resolved, properties, value_type, target):
        """One scalar/enum/string classification for fixed members and dynamic rows."""
        tid, kind = resolved['id'], resolved['__kind']
        if tid in self.custom_elements:
            return self.custom_elements[tid], ', interaction, status', []
        custom = tid in self.custom_types or properties.get('widget') in ('custom', 'asset')
        if custom: return None, '', []
        if kind == 'BuiltinType':
            if tid == 'bool': return 'lux::ui::CheckBox', ', std::string{}', []
            mode = {'input':'INPUT', 'slider':'SLIDER'}.get(properties.get('widget'), 'DRAG')
            setup = ['{', '    lux::ui::NumericSpec spec;', f'    spec.mode = lux::ui::EScalarEditMode::{mode};',
                     f'    spec.speed = static_cast<float>({properties.get("speed", "0.1")});']
            for key, member in [('min','minimum'), ('max','maximum'), ('step','step')]:
                if key in properties:
                    setup.append(f'    spec.{member} = NumericStorage<{value_type}>({properties[key]});')
            setup += [f'    if (!{target}.setSpec(std::move(spec)))',
                      '        status = InspectorInteraction::constructionFailure();', '}']
            return 'lux::ui::NumericEdit', '', setup
        if kind in ('EnumType', 'ScopedEnumType'):
            options = ', '.join('{' + f'static_cast<std::int64_t>({tid}::{item["name"]}), {literal(item["name"])}' + '}'
                                for item in self.decls[resolved['decl_id']]['enumerators'])
            return 'lux::ui::Choice', ', std::vector<lux::ui::ChoiceOption>{' + options + '}', []
        if resolved.get('template_name') in ('std::basic_string', 'std::string') or tid in ('std::string', 'std::basic_string<char>'):
            return 'lux::ui::TextEdit', '', []
        return None, '', []

    def element_factory(self, component, tid, properties=None):
        properties = properties or {}
        resolved = self.resolve(tid)
        tid = resolved['id']
        kind, template = resolved.get('__kind'), resolved.get('template_name', '')
        args = resolved.get('template_arguments', [])
        name = 'Factory' + str(self.counter)
        self.counter += 1
        properties = self.validate_field(resolved, properties)
        custom = tid in self.custom_types or properties.get('widget') in ('custom', 'asset')
        control, extra, setup = self.control_plan(resolved, properties, tid, 'result->control()')
        record = self.decls.get(resolved.get('decl_id'), {})
        if control:
            body = [f'auto result = std::make_unique<TFieldElement<InspectorInteraction, {component}, {tid}, Access, {control}>>(',
                    '    parent, std::move(id), editing, target, interaction, status, std::move(label), read_only, std::move(access)' + extra + ');']
            body += setup + ['return result;']
        elif aggregate := self.aggregate_element(component, resolved, 'Access'):
            field_type, extra = aggregate
            body = [f'return std::make_unique<{field_type}>(',
                    '    parent, std::move(id), editing, target, interaction, status, std::move(label), read_only, std::move(access)' + extra + ');']
        elif (children := self.fixed_children(resolved, properties)) is not None:
            body = ['auto group = std::make_unique<FieldGroup>(parent, std::move(id));']
            for child_type, attrs, suffix, child_label, expression in children:
                child = self.element_factory(component, child_type, attrs)
                readonly = attrs.get('readonly') == 'true' or attrs.get('widget') == 'readonly'
                selected = expression.format('*value')
                body += [f'group->add({child}::create(group->layout(), lux::ui::ElementId{{std::string(group->id().name()) + {literal(suffix)}}},',
                         f'    editing, target, interaction, status, label + " / " + {literal(child_label)}, read_only || {str(readonly).lower()},',
                         '    [access](auto& component) noexcept {',
                         f'        auto* value = access(component); using Result = decltype(std::addressof({selected}));',
                         f'        return value ? std::addressof({selected}) : Result{{}};',
                         '    }));']
            body.append('return group;')
        else:
            draw = self.draw_function(tid, properties)
            body = [f'return std::make_unique<TCompositeFieldElement<InspectorInteraction, {component}, {tid}, Access, {draw}>>(',
                    '    parent, std::move(id), editing, target, interaction, status, std::move(label), read_only, std::move(access), 3.F);']
        self.functions.append('struct ' + name + '\n{\n    template<class Access>\n'
            '    static std::unique_ptr<lux::ui::Element> create(lux::ui::Element& parent, lux::ui::ElementId id,\n'
            '        scene::SceneEditing& editing, lux::simulation::ecs::Entity target, InspectorInteraction& interaction, EditorResult<void>& status,\n'
            '        std::string label, bool read_only, Access access)\n    {\n' +
            '\n'.join('        ' + line for line in body) + '\n    }\n};\n')
        return name

    def component(self, name):
        decl = next((d for d in self.data["declarations"] if d.get("fq_name") == name and
                     d.get("__kind") in ("CXXRecordDecl", "RecordDecl")), None)
        if not decl:
            raise ValueError(f"component not found in parsed input: {name}")
        attrs = annotations(decl)
        if attrs.get("component") != "true" or attrs.get("editor") != "true":
            raise ValueError(f"component is not editor-visible: {name}")
        suffix = symbol(name)
        fields, initializers, setup = [], [], []

        def field_binding(tid, properties, identity, label, access_path, immutable):
            resolved = self.resolve(tid)
            kind = resolved.get("__kind", resolved.get("kind"))
            template = resolved.get("template_name", "")
            args = resolved.get("template_arguments", [])
            if (children := self.fixed_children(resolved, properties)) is not None:
                for child_type, attrs, suffix, child_label, expression in children:
                    readonly = immutable or attrs.get('readonly') == 'true' or attrs.get('widget') == 'readonly'
                    field_binding(child_type, attrs, identity + suffix, label + ' / ' + child_label,
                                  expression.format(access_path), readonly)
                return
            index = len(initializers)
            access = f'Access{index}'
            value = f'Value{index}'
            fields.extend([f'    using {access} = decltype([](auto& component) noexcept {{ return std::addressof({access_path}); }});',
                           f'    using {value} = std::remove_cvref_t<decltype(*{access}{{}}(std::declval<{name}&>()))>;'])
            properties = self.validate_field(resolved, properties)
            control, extra, field_setup = self.control_plan(resolved, properties, value, f'field{index}_.control()')
            setup.extend('        ' + line for line in field_setup)
            if aggregate := self.aggregate_element(name, resolved, access):
                field_type, extra = aggregate
            elif control:
                field_type = f'TFieldElement<InspectorInteraction, {name}, {value}, {access}, {control}>'
            else:
                draw = self.draw_function(tid, properties)
                field_type = f'TCompositeFieldElement<InspectorInteraction, {name}, {value}, {access}, {draw}>'
                extra = ', 6.F' if tid in self.custom_types else ', 3.F'
            fields.append(f'    {field_type} field{index}_;')
            initializers.append(f'field{index}_(layout_, lux::ui::ElementId{{{literal(identity)}}}, editing, target, interaction, status, {literal(label)}, {str(immutable).lower()}, {access}{{}}{extra})')

        for field_id in decl.get("field_decls", []):
            field = self.decls[field_id]
            properties = annotations(field)
            if field.get("visibility") != 1 or "luxref::property::skip" in properties:
                continue
            immutable = properties.get("readonly") == "true" or properties.get("widget") == "readonly" or name.endswith("::Parent")
            field_binding(field['type_id'], properties, field['fq_name'], properties.get('display_name', field['name']), 'component.' + field['name'], immutable)
        includes = '\n'.join(f'#include <{h}>' for h in self.config.get('custom_headers', []))
        initializers = ',\n          '.join(initializers)
        if initializers:
            initializers = ',\n          ' + initializers
        text = f'''// Generated persistent component controls; linked only into the Editor target.
#include "{self.config['name']}.inspector.generated.hpp"
#include <InspectorAssociativeElement.hpp>
#include <numbers>
#include <tuple>
#include <exception>
#include <new>
{includes}

namespace lux::editor::ui::generated
{{
namespace
{{
using namespace generated_support;
{chr(10).join(self.functions)}
class Element_{suffix} final : public lux::ui::Element
{{
public:
    Element_{suffix}(lux::ui::Element& parent, lux::ui::ElementId id, scene::SceneEditing& editing,
                    lux::simulation::ecs::Entity target, InspectorInteraction& interaction, EditorResult<void>& status)
        : lux::ui::Element(parent, std::move(id)), layout_(*this, lux::ui::ElementId{{"fields"}}){initializers}
    {{
        setStretch({{1, 0}});
{chr(10).join(setup)}
    }}
private:
    lux::ui::SizeHint sizeHintContent() noexcept override {{ return layout_.sizeHint(); }}
    lux::ui::SizeHint measureContent(float width) noexcept override {{ return layout_.measure(width); }}
    void arrangeContent() noexcept override {{ layout_.arrange({{{{}}, rect().size}}); }}
    void draw() noexcept override {{ drawChild(layout_); }}
    lux::ui::Layout layout_;
{chr(10).join(fields)}
}};
ComponentEditorRegistration::CreateResult create_{suffix}(lux::ui::Element& parent, lux::ui::ElementId id,
    scene::SceneEditing& editing, lux::simulation::ecs::Entity target, InspectorInteraction& interaction) noexcept
{{
    try {{
        EditorResult<void> status;
        auto result = std::unique_ptr<lux::ui::Element>(new Element_{suffix}(parent, std::move(id), editing, target, interaction, status));
        if (!status) return lux::cxx::unexpected(status.error());
        return result;
    }}
    catch (const std::bad_alloc&) {{ std::terminate(); }}
    catch (...) {{ return lux::cxx::unexpected(EditorFailure{{EEditorError::FRONTEND_FAILURE, "inspector.create"}}); }}
}}
}}
ComponentEditorRegistration binding_{suffix}()
{{
    return {{lux::cxx::typeToken<{name}>(), {literal(attrs.get('display_name', decl['name']))}, create_{suffix}}};
}}
}}
'''
        if self.config.get('session_fields'):
            text = text.replace('namespace lux::editor::ui::generated', 'namespace lux::editor::scene::generated')
            text = text.replace('using namespace generated_support;', 'using namespace lux::editor::ui;\nusing namespace lux::editor::ui::generated_support;\nusing InspectorInteraction = InspectorFields;')
            text = text.replace('scene::SceneEditing&', 'InspectorFields&').replace('lux::simulation::ecs::Entity target', 'SceneObjectRef target')
            text = text.replace('EditorResult<void>', 'SceneEditResult<void>').replace('ComponentEditorRegistration', 'InspectorComponent')
            text = text.replace('return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "inspector.create"});', 'return lux::cxx::unexpected(InspectorFields::constructionFailure().error());')
            old = f'create_{suffix}(lux::ui::Element& parent, lux::ui::ElementId id,\n    InspectorFields& editing, SceneObjectRef target, InspectorInteraction& interaction) noexcept'
            new = f'create_{suffix}(lux::ui::Element& parent, lux::ui::ElementId id, InspectorFields& interaction) noexcept'
            text = text.replace(old, new).replace(f'new Element_{suffix}(parent, std::move(id), editing, target, interaction, status)', f'new Element_{suffix}(parent, std::move(id), interaction, interaction.target(), interaction, status)')
            run = text.replace('namespace lux::editor::scene::generated', 'namespace lux::editor::scene::run_generated')
            run = run.replace('InspectorFields', 'RunInspectorFields').replace('SceneObjectRef', 'RunningObjectRef')
            run = run.replace('SceneEditResult<void>', 'RunResult<void>').replace('InspectorComponent', 'RunInspectorComponent')
            # The component-specific copy is generated alongside its controls, with the same code owner.
            old_binding = "return {" + f"lux::cxx::typeToken<{name}>(), {literal(attrs.get('display_name', decl['name']))}, create_{suffix}" + "};"
            new_binding = old_binding[:-2] + f", +[](const void* value) -> std::shared_ptr<void> {{ return std::make_shared<{name}>(*static_cast<const {name}*>(value)); }}" + "};"
            if old_binding not in run:
                raise ValueError('missing generated run component binding')
            run = run.replace(old_binding, new_binding)
            text += '\n' + run
        return suffix, text


def generate(config, data):
    if len(set(config["components"])) != len(config["components"]):
        raise ValueError("duplicate component implementation")
    outputs, declarations, bindings = {}, [], []
    for component in config["components"]:
        suffix, text = Generator(data, config).component(component)
        outputs[suffix + ".inspector.generated.cpp"] = text
        declarations.append(f"ComponentEditorRegistration binding_{suffix}();")
        bindings.append(f"binding_{suffix}()")
    outputs[config["name"] + ".inspector.generated.hpp"] = (
        '#pragma once\n#include <lux/engine/editor/ui/ComponentEditors.hpp>\n#include <array>\n'
        f'#include <{config["logical_path"]}>\nnamespace lux::editor::ui::generated\n{{\n' +
        '\n'.join(declarations) + f'\ninline auto {config["name"]}Bindings()\n{{\n'
        '    return std::array{' + ', '.join(bindings) + '};\n}\n}\n')
    if config.get('session_fields'):
        header = outputs[config['name'] + '.inspector.generated.hpp']
        header = header.replace('lux/engine/editor/ui/ComponentEditors.hpp', 'lux/engine/editor/scene/InspectorView.hpp')
        header = header.replace('namespace lux::editor::ui::generated', 'namespace lux::editor::scene::generated')
        header = header.replace('ComponentEditorRegistration', 'InspectorComponent')
        run_header = header.replace('lux/engine/editor/scene/InspectorView.hpp', 'lux/engine/editor/scene/RunInspectorView.hpp')
        run_header = run_header.replace('namespace lux::editor::scene::generated', 'namespace lux::editor::scene::run_generated')
        run_header = run_header.replace('InspectorComponent', 'RunInspectorComponent')
        outputs[config['name'] + '.inspector.generated.hpp'] = header + '\n' + run_header
    else:
        header = outputs[config['name'] + '.inspector.generated.hpp']
        outputs[config['name'] + '.inspector.generated.hpp'] = header.replace('#include <array>', '#include <array>\n#include <lux/engine/editor/ui/InspectorInteraction.hpp>')
    return outputs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--ir", required=True)
    parser.add_argument("--source-depfile")
    parser.add_argument("--depfile")
    parser.add_argument("--meta-generator")
    parser.add_argument("--meta-config")
    parser.add_argument("--formatter", required=True)
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
        formatted = subprocess.run([args.formatter, "--style=" + config["format_style"]],
                                   input=text.encode("utf-8"), capture_output=True, check=True)
        encoded = formatted.stdout
        if not path.exists() or path.read_bytes() != encoded:
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_bytes(encoded)
            temporary.replace(path)
    if args.depfile and args.source_depfile:
        # Carry the parser's complete transitive input set into this separate generation rule.
        # The IR projection can remain byte-identical when only its JSON sidecar changes.
        dependencies = Path(args.source_depfile).read_text(encoding="utf-8-sig")
        separator = re.search(r"(?<!\\):(?=\s)", dependencies)
        if not separator:
            raise ValueError("parser dependency file has no target separator")
        target = (root / (config["name"] + ".inspector.generated.hpp")).as_posix()
        target = target.replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:")
        Path(args.depfile).write_text(target + dependencies[separator.start():], encoding="utf-8")
    print(f"Editor Inspector: {len(config['components'])} component implementation(s); no runtime target output")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"EDITOR_INSPECTOR_CODEGEN: {error}", file=sys.stderr)
        sys.exit(1)
