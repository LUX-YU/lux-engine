#include "InspectorModel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <lux/cxx/algorithm/sha256.hpp>
#include <map>
#include <set>
#include <string_view>

namespace lux::editor::inspector_codegen
{
    namespace
    {
        using Properties = std::map<std::string, std::string, std::less<>>;

        std::string trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(" \r\n\t");
            return first == text.npos ? std::string{}
                                      : std::string(text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1));
        }

        Result<Properties> annotations(const Json& declaration)
        {
            Properties result;
            for (const auto& attribute : declaration.value("attributes", Json::array()))
            {
                const auto text = attribute.get<std::string>();
                std::size_t start = 0;
                char quote = 0;
                bool escaped = false;
                for (std::size_t index = 0; index <= text.size(); ++index)
                {
                    const auto ch = index == text.size() ? ',' : text[index];
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (quote && ch == '\\')
                    {
                        escaped = true;
                    }
                    else if (quote && ch == quote)
                    {
                        quote = 0;
                    }
                    else if (!quote && (ch == '"' || ch == '\''))
                    {
                        quote = ch;
                    }
                    else if (!quote && ch == ',')
                    {
                        const auto part = trim(std::string_view(text).substr(start, index - start));
                        start = index + 1;
                        if (part.empty())
                        {
                            continue;
                        }
                        const auto equal = part.find('=');
                        const auto key = trim(part.substr(0, equal));
                        auto value = equal == part.npos ? std::string{"true"} : trim(part.substr(equal + 1));
                        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                        {
                            const auto parsed = Json::parse(value, nullptr, false);
                            if (parsed.is_discarded() || !parsed.is_string())
                            {
                                return lux::cxx::unexpected("invalid quoted annotation: " + key);
                            }
                            value = parsed.get<std::string>();
                        }
                        else if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
                        {
                            value = value.substr(1, value.size() - 2);
                        }
                        if (key.empty() || !result.emplace(key, std::move(value)).second)
                        {
                            return lux::cxx::unexpected("duplicate or empty annotation: " + key);
                        }
                    }
                }
                if (quote || escaped)
                {
                    return lux::cxx::unexpected(std::string{"unterminated annotation"});
                }
            }
            return result;
        }

        std::string property(const Properties& values, std::string_view key, std::string fallback = {})
        {
            const auto found = values.find(key);
            return found == values.end() ? std::move(fallback) : found->second;
        }

        bool readOnly(const Properties& properties)
        {
            return property(properties, "readonly") == "true" || property(properties, "widget") == "readonly";
        }

        std::string symbol(std::string_view name)
        {
            std::string result;
            for (const unsigned char ch : name)
            {
                const bool is_identifier =
                    (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
                result += is_identifier ? static_cast<char>(ch) : '_';
            }
            std::array<char, 64> hex;
            lux::cxx::algorithm::Sha256::hash(name).formatHex(hex);
            return result + '_' + std::string(hex.data(), 8);
        }

        // Decimal normalization retains all 64 bits, including exponent and fractional spellings of exact integers.
        Result<std::string> integer(std::string text)
        {
            const bool negative = !text.empty() && text.front() == '-';
            if (!text.empty() && (text.front() == '-' || text.front() == '+'))
            {
                text.erase(0, 1);
            }
            std::int64_t exponent = 0;
            const auto e = text.find_first_of("eE");
            if (e != text.npos)
            {
                auto power = std::string_view(text).substr(e + 1);
                if (!power.empty() && power.front() == '+')
                {
                    power.remove_prefix(1);
                }
                const auto parsed = std::from_chars(power.data(), power.data() + power.size(), exponent);
                if (parsed.ec != std::errc{} || parsed.ptr != power.data() + power.size())
                {
                    return lux::cxx::unexpected(std::string{"invalid integer exponent"});
                }
                // Reject extreme powers before subtracting the fractional digit count.
                // No annotation path may overflow while diagnosing an out-of-range integer.
                if (exponent < -1024 || exponent > 1024)
                {
                    return lux::cxx::unexpected(std::string{"integer exponent out of range"});
                }
                text.resize(e);
            }
            const auto dot = text.find('.');
            if (dot != text.npos)
            {
                if (text.size() > 1024)
                {
                    return lux::cxx::unexpected(std::string{"numeric annotation too long"});
                }
                exponent -= static_cast<std::int64_t>(text.size() - dot - 1);
                text.erase(dot, 1);
            }
            if (text.empty() || text.find_first_not_of("0123456789") != text.npos)
            {
                return lux::cxx::unexpected(std::string{"invalid integer"});
            }
            const auto nonzero = text.find_first_not_of('0');
            if (nonzero == text.npos)
            {
                return std::string{"0"};
            }
            text.erase(0, nonzero);
            if (exponent < -1024 || exponent > 20)
            {
                return lux::cxx::unexpected(std::string{"integer out of range"});
            }
            while (exponent < 0 && !text.empty() && text.back() == '0')
            {
                text.pop_back();
                ++exponent;
            }
            if (exponent < 0)
            {
                return lux::cxx::unexpected(std::string{"fractional integer annotation"});
            }
            text.append(static_cast<std::size_t>(exponent), '0');
            return negative ? '-' + text : text;
        }

        Result<double> real(std::string_view text)
        {
            if (!text.empty() && text.front() == '+')
            {
                text.remove_prefix(1);
            }
            double value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            const bool invalid =
                parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value);
            if (invalid)
            {
                return lux::cxx::unexpected(std::string{"invalid or non-finite numeric annotation"});
            }
            return value;
        }

        class Model final
        {
        public:
            Model(const Json& unit, const Json& config) : unit_(unit), config_(config)
            {
                for (const auto& type : unit.at("types"))
                {
                    types_.emplace(type.at("id").get<std::string>(), &type);
                }
                for (const auto& decl : unit.at("declarations"))
                {
                    declarations_.emplace(decl.at("id").get<std::string>(), &decl);
                }
                for (const auto& value : config.value("custom_types", Json::array()))
                {
                    custom_.insert(value.get<std::string>());
                }
                for (const auto& value : config.value("custom_elements", Json::array()))
                {
                    const auto text = value.get<std::string>();
                    const auto equal = text.find('=');
                    custom_elements_.emplace(text.substr(0, equal), equal == text.npos ? "" : text.substr(equal + 1));
                    custom_.insert(text.substr(0, equal));
                }
            }

            Result<Json> prepare()
            {
                Json result{
                    {"name", config_.at("name")},
                    {"logical_path", config_.value("logical_path", "")},
                    {"custom_headers", config_.value("custom_headers", Json::array())},
                    {"components", Json::array()}
                };
                std::set<std::string> unique;
                for (const auto& requested : config_.at("components"))
                {
                    const auto name = requested.get<std::string>();
                    if (!unique.insert(name).second)
                    {
                        return lux::cxx::unexpected("duplicate component implementation: " + name);
                    }
                    const Json* declaration = nullptr;
                    for (const auto& decl : unit_.at("declarations"))
                    {
                        const auto kind = decl.value("__kind", "");
                        if (decl.value("fq_name", "") == name && (kind == "CXXRecordDecl" || kind == "RecordDecl"))
                        {
                            declaration = &decl;
                        }
                    }
                    if (!declaration)
                    {
                        return lux::cxx::unexpected("component not found: " + name);
                    }
                    auto attrs = annotations(*declaration);
                    if (!attrs)
                    {
                        return lux::cxx::unexpected(attrs.error());
                    }
                    if (property(*attrs, "component") != "true" || property(*attrs, "editor") != "true")
                    {
                        return lux::cxx::unexpected("component is not editor-visible: " + name);
                    }
                    nodes_ = Json::array();
                    Json fields = Json::array();
                    for (const auto& field_id : declaration->value("field_decls", Json::array()))
                    {
                        const auto& field = *declarations_.at(field_id.get<std::string>());
                        auto properties = annotations(field);
                        if (!properties)
                        {
                            return lux::cxx::unexpected(properties.error());
                        }
                        if (field.value("visibility", 0) != 1 || properties->contains("luxref::property::skip"))
                        {
                            continue;
                        }
                        auto node = prepareNode(field.at("type_id"), *properties, {});
                        if (!node)
                        {
                            return lux::cxx::unexpected(
                                name + "." + field.at("name").get<std::string>() + ": " + node.error()
                            );
                        }
                        flatten(
                            fields,
                            *node,
                            field.at("fq_name"),
                            property(*properties, "display_name", field.at("name")),
                            "component." + field.at("name").get<std::string>(),
                            readOnly(*properties) || name.ends_with("::Parent")
                        );
                    }
                    result["components"].push_back(Json{
                        {"type", name},
                        {"symbol", symbol(name)},
                        {"label", property(*attrs, "display_name", declaration->at("name"))},
                        {"fields", std::move(fields)},
                        {"nodes", std::move(nodes_)}
                    });
                }
                result["projections"] = Json::array(
                    {Json{
                         {"namespace", "generated"},
                         {"fields", "InspectorFields"},
                         {"target", "SceneObjectRef"},
                         {"result", "SceneEditResult<void>"},
                         {"binding", "InspectorComponent"},
                         {"run", false}
                     },
                     Json{
                         {"namespace", "run_generated"},
                         {"fields", "RunInspectorFields"},
                         {"target", "RunningObjectRef"},
                         {"result", "RunResult<void>"},
                         {"binding", "RunInspectorComponent"},
                         {"run", true}
                     }}
                );
                return result;
            }

        private:
            Result<const Json*> resolve(const std::string& id) const
            {
                const auto found = types_.find(id);
                if (found != types_.end())
                {
                    return found->second;
                }
                const auto& aliases = unit_.value("type_alias_map", Json::object());
                const auto alias = aliases.find(id);
                if (alias != aliases.end() && alias->is_string())
                {
                    const auto canonical = types_.find(alias->get<std::string>());
                    if (canonical != types_.end())
                    {
                        return canonical->second;
                    }
                }
                return lux::cxx::unexpected("missing parsed type: " + id);
            }

            Result<Json> numeric(const Json& type, const Properties& props) const
            {
                Json result{
                    {"mode",
                     property(props, "widget") == "input"    ? "INPUT"
                     : property(props, "widget") == "slider" ? "SLIDER"
                                                             : "DRAG"},
                    {"speed", property(props, "speed", "0.1")},
                    {"minimum", ""},
                    {"maximum", ""},
                    {"step", ""}
                };
                const auto name = type.at("id").get<std::string>();
                const bool floating = name == "float" || name == "double";
                const bool is_unsigned = name.find("unsigned") != name.npos;
                const auto bits = type.at("size").get<std::size_t>() * 8;
                if (!floating && (bits == 0 || bits > 64))
                {
                    return lux::cxx::unexpected("unsupported scalar width: " + name);
                }
                if (property(props, "widget") == "slider" && (!props.contains("min") || !props.contains("max")))
                {
                    return lux::cxx::unexpected("slider requires min/max: " + name);
                }
                std::array<std::int64_t, 2> signed_bounds{};
                std::array<std::uint64_t, 2> unsigned_bounds{};
                std::array<double, 2> floating_bounds{};
                std::size_t index = 0;
                for (const auto& [key, member] : std::array<std::pair<std::string_view, std::string_view>, 3>{
                         {{"min", "minimum"}, {"max", "maximum"}, {"step", "step"}}
                     })
                {
                    const auto raw = props.find(key);
                    if (raw != props.end())
                    {
                        if (floating)
                        {
                            auto number = real(raw->second);
                            if (!number)
                            {
                                return lux::cxx::unexpected(number.error());
                            }
                            if (key == "step" && *number <= 0)
                            {
                                return lux::cxx::unexpected(std::string{"non-positive step"});
                            }
                            if (name == "float" && std::abs(*number) > std::numeric_limits<float>::max())
                            {
                                return lux::cxx::unexpected("annotation does not fit " + name);
                            }
                            result[member] = raw->second;
                            if (index < 2)
                            {
                                floating_bounds[index] = *number;
                            }
                        }
                        else
                        {
                            auto exact = integer(raw->second);
                            if (!exact)
                            {
                                return lux::cxx::unexpected(exact.error());
                            }
                            if (key == "step" && (*exact == "0" || exact->front() == '-'))
                            {
                                return lux::cxx::unexpected(std::string{"non-positive step"});
                            }
                            const auto* end = exact->data() + exact->size();
                            if (is_unsigned)
                            {
                                std::uint64_t value = 0;
                                const auto parsed = std::from_chars(exact->data(), end, value);
                                const auto limit = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
                                if (parsed.ec != std::errc{} || parsed.ptr != end || value > limit)
                                {
                                    return lux::cxx::unexpected("annotation does not fit " + name);
                                }
                                result[member] = *exact + "ULL";
                                if (index < 2)
                                {
                                    unsigned_bounds[index] = value;
                                }
                            }
                            else
                            {
                                std::int64_t value = 0;
                                const auto parsed = std::from_chars(exact->data(), end, value);
                                const auto limit = bits == 64 ? INT64_MAX : (std::int64_t{1} << (bits - 1)) - 1;
                                if (parsed.ec != std::errc{} || parsed.ptr != end || value < -limit - 1 ||
                                    value > limit)
                                {
                                    return lux::cxx::unexpected("annotation does not fit " + name);
                                }
                                result[member] = value == INT64_MIN ? "(-9223372036854775807LL - 1)" : *exact + "LL";
                                if (index < 2)
                                {
                                    signed_bounds[index] = value;
                                }
                            }
                        }
                    }
                    ++index;
                }
                if (props.contains("min") && props.contains("max"))
                {
                    const bool reversed = floating      ? floating_bounds[0] > floating_bounds[1]
                                          : is_unsigned ? unsigned_bounds[0] > unsigned_bounds[1]
                                                        : signed_bounds[0] > signed_bounds[1];
                    if (reversed)
                    {
                        return lux::cxx::unexpected(std::string{"reversed range"});
                    }
                }
                return result;
            }

            static Json typeArguments(const Json& input)
            {
                Json result = Json::array();
                for (const auto& argument : input)
                {
                    if (argument.value("kind", "") == "type")
                    {
                        result.push_back(argument);
                    }
                    else if (argument.value("kind", "") == "pack")
                    {
                        for (const auto& child :
                             typeArguments(argument.value("elements", argument.value("pack", Json::array()))))
                        {
                            result.push_back(child);
                        }
                    }
                }
                return result;
            }

            Result<std::size_t> prepareNode(const std::string& id, Properties props, std::set<std::string> stack)
            {
                constexpr std::array<std::string_view, 14> known_properties{
                    "display_name",
                    "readonly",
                    "widget",
                    "widget_tag",
                    "color",
                    "axis_labels",
                    "min",
                    "max",
                    "speed",
                    "step",
                    "semantic_editor",
                    "luxref::property::member",
                    "luxref::property::skip",
                    "tooltip"
                };
                for (const auto& [key, value] : props)
                {
                    if (std::ranges::find(known_properties, key) == known_properties.end())
                    {
                        return lux::cxx::unexpected("unknown field annotation: " + key);
                    }
                    const bool boolean_property = key == "readonly" || key == "color";
                    const bool invalid_boolean = boolean_property && value != "true" && value != "false";
                    if (invalid_boolean)
                    {
                        return lux::cxx::unexpected("invalid boolean field annotation: " + key);
                    }
                }
                auto resolved = resolve(id);
                if (!resolved)
                {
                    return lux::cxx::unexpected(resolved.error());
                }
                const auto& type = **resolved;
                const auto tid = type.at("id").get<std::string>();
                const auto kind = type.at("__kind").get<std::string>();
                const auto template_name = type.value("template_name", "");
                auto args = type.value("template_arguments", Json::array());
                if (template_name == "std::tuple" || template_name == "std::pair" || template_name == "std::variant")
                {
                    args = typeArguments(args);
                }
                auto widget = property(props, "widget", property(props, "color") == "true" ? "color" : "default");
                const std::set<std::string_view>
                    widgets{"default", "drag", "slider", "input", "color", "asset", "enum", "readonly", "custom"};
                if (!widgets.contains(widget))
                {
                    return lux::cxx::unexpected("unknown widget: " + widget);
                }
                const auto read_only = readOnly(props);
                if (read_only)
                {
                    widget = "default";
                }
                props["widget"] = widget;
                // Integer bounds are checked by numeric() without conversion through double.
                const bool integer_type = kind == "BuiltinType" && tid != "float" && tid != "double" && tid != "bool";
                const bool uses_custom_control = custom_.contains(tid) || widget == "custom" || widget == "asset";
                const bool integer_scalar = integer_type && !uses_custom_control;
                for (const auto key : {"min", "max", "speed", "step"})
                {
                    const bool check_real = !integer_scalar || std::string_view(key) == "speed";
                    if (check_real && props.contains(key))
                    {
                        auto value = real(props.at(key));
                        if (!value)
                        {
                            return lux::cxx::unexpected(value.error());
                        }
                        if (std::string_view(key) == "step" && *value <= 0)
                        {
                            return lux::cxx::unexpected(std::string{"non-positive step"});
                        }
                    }
                }
                const bool check_range = !integer_scalar && props.contains("min") && props.contains("max");
                if (check_range && *real(props.at("min")) > *real(props.at("max")))
                {
                    return lux::cxx::unexpected(std::string{"reversed range"});
                }
                Json node{
                    {"type", tid},
                    {"kind", ""},
                    {"read_only", read_only},
                    {"options", Json::array()},
                    {"children", Json::array()},
                    {"custom_control", false},
                    {"numeric", Json::object()}
                };
                const auto custom_element = custom_elements_.find(tid);
                if (custom_element != custom_elements_.end())
                {
                    if (custom_element->second.empty())
                    {
                        return lux::cxx::unexpected("missing custom Element: " + tid);
                    }
                    node["kind"] = "control";
                    node["control"] = custom_element->second;
                    node["custom_control"] = true;
                }
                else if (custom_.contains(tid) || widget == "custom" || widget == "asset")
                {
                    node["kind"] = "composite";
                    node["composite"] = "custom";
                    node["tag"] = property(props, "widget_tag", "DefaultControlTag");
                    node["speed"] = property(props, "speed", "0.1");
                }
                else
                {
                    if (!stack.insert(tid).second)
                    {
                        return lux::cxx::unexpected("recursive ownership requires a custom Element: " + tid);
                    }
                    auto child = [&](const std::string& type_id, const Properties& attributes = {}
                                 ) -> Result<std::size_t> { return prepareNode(type_id, attributes, stack); };
                    auto fixed = [&](const std::string& child_id,
                                     const Properties& attrs,
                                     const std::string& suffix,
                                     const std::string& label,
                                     const std::string& expression) -> Result<void>
                    {
                        auto prepared = child(child_id, attrs);
                        if (!prepared)
                        {
                            return lux::cxx::unexpected(prepared.error());
                        }
                        node["children"].push_back(Json{
                            {"node", *prepared},
                            {"suffix", suffix},
                            {"label", label},
                            {"expression", expression},
                            {"read_only", readOnly(attrs)}
                        });
                        return {};
                    };
                    if (kind == "BuiltinType")
                    {
                        node["kind"] = "control";
                        if (tid == "bool")
                        {
                            if (widget != "default" && widget != "input")
                            {
                                return lux::cxx::unexpected("widget cannot edit bool: " + widget);
                            }
                            node["control"] = "lux::ui::CheckBox";
                        }
                        else
                        {
                            const bool bad_widget =
                                widget != "default" && widget != "drag" && widget != "input" && widget != "slider";
                            const bool bad_type = tid == "void" || tid == "long double" || tid == "wchar_t" ||
                                                  tid == "char16_t" || tid == "char32_t" || tid == "char8_t";
                            if (bad_widget || bad_type)
                            {
                                return lux::cxx::unexpected("unsupported scalar/widget: " + tid + "/" + widget);
                            }
                            auto spec = numeric(type, props);
                            if (!spec)
                            {
                                return lux::cxx::unexpected(spec.error());
                            }
                            node["control"] = "lux::ui::NumericEdit";
                            node["numeric"] = std::move(*spec);
                        }
                    }
                    else if (kind == "EnumType" || kind == "ScopedEnumType" || kind == "UnscopedEnumType")
                    {
                        if (widget != "default" && widget != "enum" && widget != "input")
                        {
                            return lux::cxx::unexpected("widget cannot edit enum: " + widget);
                        }
                        const auto& decl = *declarations_.at(type.at("decl_id").get<std::string>());
                        if (decl.at("enumerators").empty())
                        {
                            return lux::cxx::unexpected("enum has no enumerators: " + tid);
                        }
                        node["kind"] = "control";
                        node["control"] = "lux::ui::Choice";
                        for (const auto& item : decl.at("enumerators"))
                        {
                            node["options"].push_back(Json{
                                {"value",
                                 "static_cast<std::int64_t>(" + tid + "::" + item.at("name").get<std::string>() + ")"},
                                {"label", item.at("name")}
                            });
                        }
                    }
                    else if (template_name == "std::basic_string" || template_name == "std::string" ||
                             tid == "std::string")
                    {
                        const bool bad_widget = widget != "default" && widget != "input";
                        const bool bad_encoding = !args.empty() && args.at(0).at("type_id") != "char";
                        if (bad_widget || bad_encoding)
                        {
                            return lux::cxx::unexpected("string requires UTF-8 control: " + tid);
                        }
                        node["kind"] = "control";
                        node["control"] = "lux::ui::TextEdit";
                    }
                    else if (template_name == "Eigen::Quaternion" ||
                             (template_name == "Eigen::Matrix" && widget == "color"))
                    {
                        node["kind"] = "composite";
                        const auto scalar = args.at(0).at("type_id").get<std::string>();
                        if (template_name == "Eigen::Quaternion")
                        {
                            if (widget != "default" && widget != "drag" && widget != "input")
                            {
                                return lux::cxx::unexpected(std::string{"quaternion widget must edit rotation"});
                            }
                            auto scalar_type = resolve(scalar);
                            if (!scalar_type)
                            {
                                return lux::cxx::unexpected(scalar_type.error());
                            }
                            props.try_emplace("speed", "0.25");
                            auto spec = numeric(**scalar_type, props);
                            if (!spec)
                            {
                                return lux::cxx::unexpected(spec.error());
                            }
                            node["numeric"] = std::move(*spec);
                            node["composite"] = "quaternion";
                            node["scalar"] = scalar;
                        }
                        else
                        {
                            const auto rows = args.at(1).at("integral_value").get<int>();
                            const bool bad_color =
                                scalar != "float" || args.at(2).at("integral_value") != 1 || (rows != 3 && rows != 4);
                            if (bad_color)
                            {
                                return lux::cxx::unexpected(std::string{"color requires fixed float3/float4"});
                            }
                            node["composite"] = "color";
                            node["rows"] = rows;
                        }
                    }
                    else if (tid == "std::monostate")
                    {
                        node["kind"] = "composite";
                        node["composite"] = "empty";
                    }
                    else if (kind == "ArrayType" || template_name == "std::array" || template_name == "Eigen::Matrix")
                    {
                        node["kind"] = "group";
                        const bool matrix = template_name == "Eigen::Matrix";
                        const auto element = kind == "ArrayType" ? type.at("element_type_id").get<std::string>()
                                                                 : args.at(0).at("type_id").get<std::string>();
                        const auto count = kind == "ArrayType" ? type.at("array_size").get<std::int64_t>()
                                                               : args.at(1).at("integral_value").get<std::int64_t>();
                        const auto columns = matrix ? args.at(2).at("integral_value").get<std::int64_t>() : 1;
                        const bool invalid_extent =
                            count <= 0 || columns <= 0 ||
                            (columns > 0 && count > std::numeric_limits<std::int64_t>::max() / columns);
                        if (invalid_extent)
                        {
                            return lux::cxx::unexpected("fixed control extent requires a custom Element: " + tid);
                        }
                        for (std::int64_t row = 0; row < count; ++row)
                        {
                            for (std::int64_t col = 0; col < columns; ++col)
                            {
                                const auto indices = std::to_string(row) + (matrix ? "," + std::to_string(col) : "");
                                const auto suffix = "[" + indices + "]";
                                auto label = suffix;
                                if (matrix && (count == 1 || columns == 1))
                                {
                                    auto axes = property(props, "axis_labels", "X|Y|Z|W");
                                    const auto wanted = row * columns + col;
                                    for (std::int64_t axis = 0; axis <= wanted && !axes.empty(); ++axis)
                                    {
                                        const auto end = axes.find('|');
                                        if (axis == wanted)
                                        {
                                            label = axes.substr(0, end);
                                        }
                                        axes = end == axes.npos ? "" : axes.substr(end + 1);
                                    }
                                }
                                auto added = fixed(
                                    element,
                                    props,
                                    suffix,
                                    label,
                                    matrix ? "({})(" + indices + ")" : "({})[" + indices + "]"
                                );
                                if (!added)
                                {
                                    return lux::cxx::unexpected(added.error());
                                }
                            }
                        }
                    }
                    else if (template_name == "std::pair" || template_name == "std::tuple")
                    {
                        node["kind"] = "group";
                        std::size_t index = 0;
                        for (const auto& arg : args)
                        {
                            auto added = fixed(
                                arg.at("type_id"),
                                {},
                                "/" + std::to_string(index),
                                "[" + std::to_string(index) + "]",
                                "std::get<" + std::to_string(index) + ">({})"
                            );
                            if (!added)
                            {
                                return lux::cxx::unexpected(added.error());
                            }
                            ++index;
                        }
                    }
                    else if (template_name == "std::vector" || template_name == "std::deque" ||
                             template_name == "std::list")
                    {
                        auto item = child(args.at(0).at("type_id"));
                        if (!item)
                        {
                            return lux::cxx::unexpected(item.error());
                        }
                        node["kind"] = "sequence";
                        node["item"] = *item;
                    }
                    else if (template_name == "std::optional" || template_name == "std::variant")
                    {
                        node["kind"] = "alternative";
                        node["optional"] = template_name == "std::optional";
                        node["items"] = Json::array();
                        std::size_t index = 0;
                        for (const auto& arg : args)
                        {
                            auto item = child(arg.at("type_id"));
                            if (!item)
                            {
                                return lux::cxx::unexpected(item.error());
                            }
                            node["items"].push_back(*item);
                            node["options"].push_back(Json{
                                {"value", std::to_string(index)},
                                {"label", std::to_string(index) + ": " + arg.at("type_id").get<std::string>()}
                            });
                            ++index;
                        }
                        if (node["optional"] == true)
                        {
                            node["options"] = Json::array(
                                {Json{{"value", "0"}, {"label", "Absent"}}, Json{{"value", "1"}, {"label", "Present"}}}
                            );
                        }
                    }
                    else if (template_name == "std::map" || template_name == "std::unordered_map" ||
                             template_name == "std::set" || template_name == "std::unordered_set")
                    {
                        auto key = child(args.at(0).at("type_id"));
                        if (!key)
                        {
                            return lux::cxx::unexpected(key.error());
                        }
                        if (nodes_.at(*key).at("kind") != "control" || nodes_.at(*key).at("custom_control") == true)
                        {
                            return lux::cxx::unexpected(
                                "associative keys require scalar/string or custom Element: " + tid
                            );
                        }
                        node["kind"] = "associative";
                        node["mapping"] = template_name == "std::map" || template_name == "std::unordered_map";
                        if (node["mapping"] == true)
                        {
                            auto item = child(args.at(1).at("type_id"));
                            if (!item)
                            {
                                return lux::cxx::unexpected(item.error());
                            }
                            node["item"] = *item;
                        }
                    }
                    else if (kind == "RecordType")
                    {
                        const auto decl = declarations_.find(type.value("decl_id", ""));
                        if (decl == declarations_.end())
                        {
                            return lux::cxx::unexpected("unsupported record: " + tid);
                        }
                        auto record_attrs = annotations(*decl->second);
                        if (!record_attrs || !record_attrs->contains("luxref::class"))
                        {
                            return lux::cxx::unexpected("unreflected record requires a custom Element: " + tid);
                        }
                        node["kind"] = "group";
                        for (const auto& field_id : decl->second->value("field_decls", Json::array()))
                        {
                            const auto& field = *declarations_.at(field_id.get<std::string>());
                            auto attrs = annotations(field);
                            if (!attrs)
                            {
                                return lux::cxx::unexpected(attrs.error());
                            }
                            if (field.value("visibility", 0) != 1 || attrs->contains("luxref::property::skip"))
                            {
                                continue;
                            }
                            const auto name = field.at("name").get<std::string>();
                            auto added = fixed(
                                field.at("type_id"),
                                *attrs,
                                "." + name,
                                property(*attrs, "display_name", name),
                                "({})." + name
                            );
                            if (!added)
                            {
                                return lux::cxx::unexpected(added.error());
                            }
                        }
                    }
                    else
                    {
                        return lux::cxx::unexpected("unsupported parsed type: " + tid);
                    }
                    const bool aggregate = node["kind"] == "group" || node["kind"] == "sequence" ||
                                           node["kind"] == "alternative" || node["kind"] == "associative";
                    if (aggregate && template_name != "Eigen::Matrix" && kind != "ArrayType" && widget != "default")
                    {
                        return lux::cxx::unexpected("aggregate requires a custom widget: " + tid);
                    }
                }
                const auto index = nodes_.size();
                node["index"] = index;
                nodes_.push_back(std::move(node));
                return index;
            }

            void flatten(
                Json& fields,
                std::size_t index,
                const std::string& identity,
                const std::string& label,
                const std::string& access,
                bool read_only
            ) const
            {
                const auto& node = nodes_.at(index);
                if (node.at("kind") == "group")
                {
                    for (const auto& child : node.at("children"))
                    {
                        auto expression = child.at("expression").get<std::string>();
                        expression.replace(expression.find("{}"), 2, access);
                        flatten(
                            fields,
                            child.at("node"),
                            identity + child.at("suffix").get<std::string>(),
                            label + " / " + child.at("label").get<std::string>(),
                            expression,
                            read_only || child.at("read_only").get<bool>()
                        );
                    }
                    return;
                }
                fields.push_back(Json{
                    {"index", fields.size()},
                    {"node", index},
                    {"identity", identity},
                    {"label", label},
                    {"access", access},
                    {"read_only", read_only}
                });
            }

            const Json& unit_;
            const Json& config_;
            std::map<std::string, const Json*, std::less<>> types_;
            std::map<std::string, const Json*, std::less<>> declarations_;
            std::set<std::string, std::less<>> custom_;
            Properties custom_elements_;
            Json nodes_ = Json::array();
        };
    } // namespace

    Result<Json> prepare(const Json& unit, const Json& configuration)
    {
        return Model(unit, configuration).prepare();
    }
} // namespace lux::editor::inspector_codegen
