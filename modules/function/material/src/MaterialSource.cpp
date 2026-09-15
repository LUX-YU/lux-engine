#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <sstream>
#include <toml++/toml.hpp>
#include <unordered_set>

namespace lux::material
{
    bool equalMaterialNodes(const Node& first, const Node& second) noexcept
    {
        if (first.id() != second.id() || first.kind() != second.kind() || first.name() != second.name())
        {
            return false;
        }
        const auto equal_pin = [](const DataPin& a, const DataPin& b)
        {
            return a.id == b.id && a.type == b.type && a.direction == b.direction && a.name == b.name &&
                std::ranges::equal(a.constant, b.constant);
        };
        if (!std::ranges::equal(first.inputs(), second.inputs(), equal_pin) ||
            !std::ranges::equal(first.outputs(), second.outputs(), equal_pin))
        {
            return false;
        }
        switch (first.kind())
        {
        case EMatNodeKind::CONSTANT:
            return first.as<ConstantNode>()->value_type == second.as<ConstantNode>()->value_type &&
                std::ranges::equal(first.as<ConstantNode>()->value, second.as<ConstantNode>()->value);
        case EMatNodeKind::INPUT:
            return first.as<InputNode>()->input == second.as<InputNode>()->input;
        case EMatNodeKind::SAMPLE_TEXTURE:
            return first.as<SampleTextureNode>()->texture_slot == second.as<SampleTextureNode>()->texture_slot;
        case EMatNodeKind::PARAM:
            return first.as<ParamNode>()->param_slot == second.as<ParamNode>()->param_slot &&
                first.as<ParamNode>()->type == second.as<ParamNode>()->type;
        case EMatNodeKind::MATH:
            return first.as<MathNode>()->op == second.as<MathNode>()->op &&
                first.as<MathNode>()->operand_type == second.as<MathNode>()->operand_type;
        case EMatNodeKind::SWIZZLE:
            return first.as<SwizzleNode>()->source_type == second.as<SwizzleNode>()->source_type &&
                first.as<SwizzleNode>()->out_type == second.as<SwizzleNode>()->out_type &&
                std::ranges::equal(first.as<SwizzleNode>()->components, second.as<SwizzleNode>()->components);
        case EMatNodeKind::CONSTRUCT:
            return first.as<ConstructNode>()->out_type == second.as<ConstructNode>()->out_type;
        case EMatNodeKind::DECODE_NORMAL:
        case EMatNodeKind::TBN_TRANSFORM:
        case EMatNodeKind::OUTPUT_SURFACE:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        constexpr std::array kinds{"invalid",       "constant",  "input",         "sample_texture",
                                   "math",          "swizzle",   "construct",     "decode_normal",
                                   "tbn_transform", "parameter", "output_surface"};
        static_assert(kinds.size() == static_cast<std::size_t>(EMatNodeKind::COUNT));
        auto fail(EMaterialSourceError code, std::string field = {}, NodeId node = {}, PinId pin = {}) noexcept
        {
            return lux::cxx::unexpected(MaterialSourceFailure{code, std::move(field), node, pin});
        }
        bool validLimits(MaterialSourceLimits limits) noexcept
        {
            return limits.max_bytes && limits.max_nodes && limits.max_pins && limits.max_slots &&
                   limits.max_string_bytes;
        }
        bool finite(std::span<const float> values) noexcept
        {
            return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
        }
        bool typeValid(EValueType type) noexcept
        {
            return type <= EValueType::VEC4;
        }
        toml::array floats(std::span<const float> values)
        {
            toml::array result;
            for (const auto value : values)
            {
                result.push_back(static_cast<double>(value));
            }
            return result;
        }
        bool readFloats(const toml::node_view<const toml::node> &value, std::span<float> destination) noexcept
        {
            const auto *array = value.as_array();
            if (!array || array->size() != destination.size())
            {
                return false;
            }
            for (std::size_t index{}; index < destination.size(); ++index)
            {
                const auto number = (*array)[index].value<double>();
                if (!number)
                {
                    return false;
                }
                destination[index] = static_cast<float>(*number);
                if (!std::isfinite(destination[index]))
                {
                    return false;
                }
            }
            return true;
        }
        bool fields(const toml::table &table, std::initializer_list<std::string_view> allowed) noexcept
        {
            return std::all_of(
                table.begin(), table.end(), [&](const auto &item)
                { return std::find(allowed.begin(), allowed.end(), item.first.str()) != allowed.end(); });
        }
        std::optional<std::uint64_t> identity(const toml::node_view<const toml::node> &value) noexcept
        {
            const auto text = value.value<std::string_view>();
            if (!text || text->empty())
            {
                return {};
            }
            std::uint64_t number{};
            const auto parsed = std::from_chars(text->data(), text->data() + text->size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || number == 0)
            {
                return {};
            }
            return number;
        }
        std::optional<std::uint32_t> integer(const toml::node_view<const toml::node> &value,
                                             std::uint32_t maximum) noexcept
        {
            const auto number = value.value<std::int64_t>();
            if (!number || *number < 0 || static_cast<std::uint64_t>(*number) > maximum)
            {
                return {};
            }
            return static_cast<std::uint32_t>(*number);
        }
        bool textValid(std::string_view text, MaterialSourceLimits limits) noexcept
        {
            if (text.size() > limits.max_string_bytes)
            {
                return false;
            }
            for (std::size_t index{}; index < text.size();)
            {
                const auto first = static_cast<unsigned char>(text[index++]);
                if (!first)
                {
                    return false;
                }
                if (first < 0x80)
                {
                    continue;
                }
                const unsigned tails = first >= 0xc2 && first <= 0xdf   ? 1
                                       : first >= 0xe0 && first <= 0xef ? 2
                                       : first >= 0xf0 && first <= 0xf4 ? 3
                                                                        : 0;
                if (!tails || tails > text.size() - index)
                {
                    return false;
                }
                std::uint32_t scalar = first & ((1U << (6 - tails)) - 1);
                for (unsigned tail{}; tail < tails; ++tail)
                {
                    const auto next = static_cast<unsigned char>(text[index++]);
                    if ((next & 0xc0U) != 0x80U)
                    {
                        return false;
                    }
                    scalar = (scalar << 6U) | (next & 0x3fU);
                }
                const bool overlong = scalar < (tails == 1 ? 0x80U : tails == 2 ? 0x800U : 0x10000U);
                if (overlong || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU))
                {
                    return false;
                }
            }
            return true;
        }
        std::optional<std::string> text(const toml::node_view<const toml::node> &value,
                                        MaterialSourceLimits limits) noexcept
        {
            auto result = value.value<std::string>();
            if (!result || !textValid(*result, limits))
            {
                return {};
            }
            return result;
        }
        MaterialSourceResult<toml::table> payload(const Node &node) noexcept
        {
            toml::table result;
            switch (node.kind())
            {
            case EMatNodeKind::CONSTANT:
            {
                const auto &value = *node.as<ConstantNode>();
                if (!typeValid(value.value_type) || !finite(value.value))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "constant", node.id());
                }
                result.insert("type", static_cast<std::int64_t>(value.value_type));
                result.insert("value", floats(value.value));
                break;
            }
            case EMatNodeKind::INPUT:
            {
                const auto input = node.as<InputNode>()->input;
                if (input >= EMaterialInput::COUNT)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "input", node.id());
                }
                result.insert("input", static_cast<std::int64_t>(input));
                break;
            }
            case EMatNodeKind::SAMPLE_TEXTURE:
                result.insert("slot", node.as<SampleTextureNode>()->texture_slot);
                break;
            case EMatNodeKind::PARAM:
            {
                const auto &value = *node.as<ParamNode>();
                if (!typeValid(value.type))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "type", node.id());
                }
                result.insert("type", static_cast<std::int64_t>(value.type));
                result.insert("slot", value.param_slot);
                break;
            }
            case EMatNodeKind::MATH:
            {
                const auto &value = *node.as<MathNode>();
                if (!typeValid(value.operand_type) || value.op > EMathOp::LENGTH)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "math", node.id());
                }
                result.insert("type", static_cast<std::int64_t>(value.operand_type));
                result.insert("operation", static_cast<std::int64_t>(value.op));
                break;
            }
            case EMatNodeKind::SWIZZLE:
            {
                const auto &value = *node.as<SwizzleNode>();
                if (!typeValid(value.source_type) || !typeValid(value.out_type))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "swizzle", node.id());
                }
                result.insert("source_type", static_cast<std::int64_t>(value.source_type));
                result.insert("type", static_cast<std::int64_t>(value.out_type));
                toml::array components;
                for (const auto component : value.components)
                {
                    if (component > 3)
                    {
                        return fail(EMaterialSourceError::INVALID_VALUE, "component", node.id());
                    }
                    components.push_back(component);
                }
                result.insert("components", std::move(components));
                break;
            }
            case EMatNodeKind::CONSTRUCT:
            {
                const auto type = node.as<ConstructNode>()->out_type;
                if (!typeValid(type))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "type", node.id());
                }
                result.insert("type", static_cast<std::int64_t>(type));
                break;
            }
            case EMatNodeKind::DECODE_NORMAL:
            case EMatNodeKind::TBN_TRANSFORM:
            case EMatNodeKind::OUTPUT_SURFACE:
                break;
            default:
                return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "kind", node.id());
            }
            return result;
        }
        MaterialSourceResult<std::unique_ptr<Node>> makeNode(EMatNodeKind kind, const toml::table &data) noexcept
        {
            const auto type = integer(data["type"], 3);
            const auto slot = integer(data["slot"], UINT32_MAX);
            switch (kind)
            {
            case EMatNodeKind::CONSTANT:
            {
                if (!fields(data, {"type", "value"}) || !type)
                {
                    break;
                }
                auto node = std::make_unique<ConstantNode>();
                if (!readFloats(data["value"], node->value))
                {
                    break;
                }
                node->setType(static_cast<EValueType>(*type));
                return node;
            }
            case EMatNodeKind::INPUT:
            {
                const auto input = integer(data["input"], static_cast<unsigned>(EMaterialInput::COUNT) - 1);
                if (!fields(data, {"input"}) || !input)
                {
                    break;
                }
                auto node = std::make_unique<InputNode>();
                node->input = static_cast<EMaterialInput>(*input);
                node->outputs()[0].type = materialInputDescription(node->input)->type;
                return node;
            }
            case EMatNodeKind::SAMPLE_TEXTURE:
            {
                if (!fields(data, {"slot"}) || !slot)
                {
                    break;
                }
                auto node = std::make_unique<SampleTextureNode>();
                node->texture_slot = *slot;
                return node;
            }
            case EMatNodeKind::PARAM:
            {
                if (!fields(data, {"type", "slot"}) || !type || !slot)
                {
                    break;
                }
                auto node = std::make_unique<ParamNode>(static_cast<EValueType>(*type));
                node->param_slot = *slot;
                return node;
            }
            case EMatNodeKind::MATH:
            {
                const auto op = integer(data["operation"], static_cast<unsigned>(EMathOp::LENGTH));
                if (!fields(data, {"type", "operation"}) || !type || !op)
                {
                    break;
                }
                auto node = std::make_unique<MathNode>(static_cast<EMathOp>(*op));
                node->setOperandType(static_cast<EValueType>(*type));
                return node;
            }
            case EMatNodeKind::SWIZZLE:
            {
                const auto source_type = integer(data["source_type"], 3);
                const auto *components = data["components"].as_array();
                const bool valid = fields(data, {"type", "source_type", "components"}) && type && source_type &&
                                   components && components->size() == 4;
                if (!valid)
                {
                    break;
                }
                auto node = std::make_unique<SwizzleNode>(static_cast<EValueType>(*source_type),
                                                          static_cast<EValueType>(*type));
                for (std::size_t index{}; index < 4; ++index)
                {
                    const auto component = integer(toml::node_view<const toml::node>{&(*components)[index]}, 3);
                    if (!component)
                    {
                        return fail(EMaterialSourceError::INVALID_VALUE, "components");
                    }
                    node->components[index] = static_cast<std::uint8_t>(*component);
                }
                return node;
            }
            case EMatNodeKind::CONSTRUCT:
                if (fields(data, {"type"}) && type)
                {
                    return std::make_unique<ConstructNode>(static_cast<EValueType>(*type));
                }
                break;
            case EMatNodeKind::DECODE_NORMAL:
                if (data.empty())
                {
                    return std::make_unique<DecodeNormalNode>();
                }
                break;
            case EMatNodeKind::TBN_TRANSFORM:
                if (data.empty())
                {
                    return std::make_unique<TbnTransformNode>();
                }
                break;
            case EMatNodeKind::OUTPUT_SURFACE:
                if (data.empty())
                {
                    return std::make_unique<OutputSurfaceNode>();
                }
                break;
            default:
                return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "kind");
            }
            return fail(EMaterialSourceError::INVALID_VALUE, "payload");
        }
        MaterialSourceResult<toml::array> encodePins(const MaterialGraph &graph, const Node &node,
                                                     std::span<const DataPin> pins, EPinDirection direction,
                                                     MaterialSourceLimits limits) noexcept
        {
            toml::array result;
            for (const auto &pin : pins)
            {
                const auto *topology = graph.topology().findPin(pin.id);
                const bool valid = topology && topology->owner == node.id() &&
                                   static_cast<unsigned>(topology->direction) == static_cast<unsigned>(direction) &&
                                   pin.direction == direction && typeValid(pin.type) && textValid(pin.name, limits) &&
                                   finite(pin.constant);
                if (!valid)
                {
                    return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pin", node.id(), pin.id);
                }
                result.push_back(toml::table{{"id", std::to_string(pin.id.value)},
                                             {"name", pin.name},
                                             {"type", static_cast<std::int64_t>(pin.type)},
                                             {"default", floats(pin.constant)}});
            }
            return result;
        }
        MaterialSourceResult<void> decodePins(const toml::node_view<const toml::node> &value,
                                              std::vector<DataPin> &pins, std::unordered_set<PinId> &identities,
                                              MaterialSourceLimits limits) noexcept
        {
            const auto *array = value.as_array();
            if (!array || array->size() != pins.size())
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "pins");
            }
            if (pins.size() > limits.max_pins - identities.size())
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "pins");
            }
            for (std::size_t index{}; index < pins.size(); ++index)
            {
                const auto *table = (*array)[index].as_table();
                if (!table || !fields(*table, {"id", "name", "type", "default"}))
                {
                    return fail(EMaterialSourceError::UNKNOWN_FIELD, "pin");
                }
                const auto id = identity((*table)["id"]);
                const auto name = text((*table)["name"], limits);
                const auto type = integer((*table)["type"], 3);
                if (!id || !identities.emplace(PinId{*id}).second)
                {
                    return fail(EMaterialSourceError::INVALID_IDENTITY, "pin");
                }
                if (!name || !type || !readFloats((*table)["default"], pins[index].constant))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "pin");
                }
                pins[index].id = PinId{*id};
                pins[index].name = *name;
                pins[index].type = static_cast<EValueType>(*type);
            }
            return {};
        }
    } // namespace
    MaterialSourceResult<void> validateMaterialSource(const MaterialSourceDocument &source,
                                                      MaterialSourceLimits limits) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (source.id.isNull())
        {
            return fail(EMaterialSourceError::INVALID_IDENTITY, "id");
        }
        const auto &graph = source.graph;
        const bool invalid_source = source.name.empty() || !textValid(source.name, limits) ||
                                    graph.shading_model > lux::rdesc::ELightingTechnique::Graph ||
                                    graph.render_state.alpha_mode > lux::rdesc::EAlphaMode::Blend ||
                                    !std::isfinite(graph.render_state.alpha_cutoff);
        if (invalid_source)
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "source");
        }
        const bool exceeded =
            graph.nodes().size() > limits.max_nodes || graph.topology().pins().size() > limits.max_pins ||
            graph.texture_slots.size() > limits.max_slots || graph.param_slots.size() > limits.max_slots;
        if (exceeded)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED);
        }
        if (graph.nodes().size() != graph.topology().nodes().size())
        {
            return fail(EMaterialSourceError::INVALID_TOPOLOGY, "nodes");
        }
        std::size_t text_bytes{}, pin_count{};
        const auto count_text = [&](std::string_view value)
        {
            if (!textValid(value, limits) || value.size() > limits.max_bytes - text_bytes)
            {
                return false;
            }
            text_bytes += value.size();
            return true;
        };
        if (!count_text(source.name))
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED, "name");
        }
        for (const auto &slot : graph.texture_slots)
        {
            if (!count_text(slot.name))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "texture.name");
            }
        }
        for (const auto &slot : graph.param_slots)
        {
            if (!count_text(slot.name) || !typeValid(slot.type) || !finite(slot.dflt))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
        }
        for (const auto &[id, owned] : graph.nodes())
        {
            const auto *record = graph.topology().findNode(id);
            if (!owned || !id.valid() || owned->id() != id || !record)
            {
                return fail(EMaterialSourceError::INVALID_IDENTITY, "node", id);
            }
            const auto &node = *owned;
            if (record->type.value != static_cast<std::uint64_t>(node.kind()) + 1)
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "node.type", id);
            }
            if (!count_text(node.name()))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "node.name", id);
            }
            std::size_t inputs{}, outputs{1};
            bool valid{true};
            switch (node.kind())
            {
            case EMatNodeKind::CONSTANT:
                valid = typeValid(node.as<ConstantNode>()->value_type) && finite(node.as<ConstantNode>()->value);
                break;
            case EMatNodeKind::INPUT:
                valid = node.as<InputNode>()->input < EMaterialInput::COUNT;
                break;
            case EMatNodeKind::PARAM:
                valid = typeValid(node.as<ParamNode>()->type);
                break;
            case EMatNodeKind::SAMPLE_TEXTURE:
            case EMatNodeKind::DECODE_NORMAL:
            case EMatNodeKind::TBN_TRANSFORM:
                inputs = 1;
                break;
            case EMatNodeKind::MATH:
                inputs = 2;
                valid = typeValid(node.as<MathNode>()->operand_type) && node.as<MathNode>()->op <= EMathOp::LENGTH;
                break;
            case EMatNodeKind::SWIZZLE:
            {
                inputs = 1;
                const auto &value = *node.as<SwizzleNode>();
                valid = typeValid(value.source_type) && typeValid(value.out_type) &&
                        std::all_of(std::begin(value.components), std::end(value.components),
                                    [](auto component) { return component < 4; });
                break;
            }
            case EMatNodeKind::CONSTRUCT:
                valid = typeValid(node.as<ConstructNode>()->out_type);
                inputs = static_cast<std::size_t>(node.as<ConstructNode>()->out_type) + 1;
                break;
            case EMatNodeKind::OUTPUT_SURFACE:
                inputs = std::size(kMaterialAttributes);
                outputs = 0;
                break;
            default:
                return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "kind", id);
            }
            if (!valid || node.inputs().size() != inputs || node.outputs().size() != outputs)
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "node.payload", id);
            }
            for (unsigned direction{}; direction < 2; ++direction)
            {
                const auto &pins = direction ? node.outputs() : node.inputs();
                if (pins.size() > limits.max_pins - pin_count)
                {
                    return fail(EMaterialSourceError::LIMIT_EXCEEDED, "pins", id);
                }
                pin_count += pins.size();
                for (std::size_t index{}; index < pins.size(); ++index)
                {
                    const auto &pin = pins[index];
                    const auto *topology = graph.topology().findPin(pin.id);
                    const auto semantic = (direction ? std::uint64_t{1} << 63 : 0) | (index + 1);
                    const bool valid_pin =
                        topology && topology->owner == id && static_cast<unsigned>(topology->direction) == direction &&
                        static_cast<unsigned>(pin.direction) == direction && topology->semantic.value == semantic &&
                        topology->fan_cap == (direction ? lux::graph::kUnlimitedFan : 1) && typeValid(pin.type) &&
                        count_text(pin.name) && finite(pin.constant);
                    if (!valid_pin)
                    {
                        return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pin", id, pin.id);
                    }
                }
            }
        }
        if (pin_count != graph.topology().pins().size())
        {
            return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pins");
        }
        for (const auto &entry : graph.layout().all())
        {
            if (!graph.node(entry.node))
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "layout", entry.node);
            }
            if (!std::isfinite(entry.layout.x) || !std::isfinite(entry.layout.y))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "layout", entry.node);
            }
        }
        return {};
    }
    MaterialSourceResult<std::string> encodeMaterialSource(const MaterialSourceDocument &source,
                                                           MaterialSourceLimits limits) noexcept
    {
        if (const auto checked = validateMaterialSource(source, limits); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        const auto &graph = source.graph;
        std::vector<const Node *> ordered;
        ordered.reserve(graph.nodes().size());
        for (const auto &[id, node] : graph.nodes())
        {
            ordered.push_back(node);
        }
        std::ranges::sort(ordered, {}, &Node::id);
        toml::table document{{"format", "lux.material.source"},
                             {"version", 1},
                             {"id", uuids::to_string(source.id.uuid())},
                             {"name", source.name},
                             {"shading", static_cast<std::int64_t>(graph.shading_model)},
                             {"alpha_mode", static_cast<std::int64_t>(graph.render_state.alpha_mode)},
                             {"alpha_cutoff", static_cast<double>(graph.render_state.alpha_cutoff)},
                             {"double_sided", graph.render_state.double_sided}};
        toml::array textures, parameters, nodes, links;
        for (const auto &slot : graph.texture_slots)
        {
            textures.push_back(toml::table{{"name", slot.name}, {"asset", uuids::to_string(slot.texture.uuid())}});
        }
        for (const auto &slot : graph.param_slots)
        {
            if (!typeValid(slot.type) || !finite(slot.dflt))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
            parameters.push_back(toml::table{
                {"name", slot.name}, {"type", static_cast<std::int64_t>(slot.type)}, {"default", floats(slot.dflt)}});
        }
        for (const auto *node : ordered)
        {
            auto data = payload(*node);
            if (!data)
            {
                return lux::cxx::unexpected(data.error());
            }
            auto inputs = encodePins(graph, *node, node->inputs(), EPinDirection::INPUT, limits);
            auto outputs = encodePins(graph, *node, node->outputs(), EPinDirection::OUTPUT, limits);
            if (!inputs)
            {
                return lux::cxx::unexpected(inputs.error());
            }
            if (!outputs)
            {
                return lux::cxx::unexpected(outputs.error());
            }
            toml::table record{{"id", std::to_string(node->id().value)},
                               {"name", node->name()},
                               {"kind", kinds[static_cast<std::size_t>(node->kind())]},
                               {"payload", std::move(*data)},
                               {"inputs", std::move(*inputs)},
                               {"outputs", std::move(*outputs)}};
            if (const auto *layout = graph.layout().find(node->id()))
            {
                if (!std::isfinite(layout->x) || !std::isfinite(layout->y))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "layout", node->id());
                }
                record.insert("layout", toml::table{{"x", static_cast<double>(layout->x)},
                                                    {"y", static_cast<double>(layout->y)},
                                                    {"placed", layout->placed}});
            }
            nodes.push_back(std::move(record));
        }
        for (const auto &link : graph.topology().links())
        {
            links.push_back(
                toml::table{{"from", std::to_string(link.from.value)}, {"to", std::to_string(link.to.value)}});
        }
        for (const auto &layout : graph.layout().all())
        {
            if (!graph.node(layout.node))
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "layout", layout.node);
            }
        }
        document.insert("textures", std::move(textures));
        document.insert("parameters", std::move(parameters));
        document.insert("nodes", std::move(nodes));
        document.insert("links", std::move(links));
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << toml::toml_formatter{document};
        auto encoded = std::move(stream).str();
        if (encoded.size() > limits.max_bytes)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED);
        }
        if (!toml::parse(encoded))
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "UTF-8");
        }
        return encoded;
    }
    MaterialSourceResult<MaterialSourceDocument> decodeMaterialSource(std::string_view bytes,
                                                                      MaterialSourceLimits limits) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (bytes.size() > limits.max_bytes)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED);
        }
        auto parsed = toml::parse(bytes);
        if (!parsed)
        {
            const auto point = parsed.error().source().begin;
            return lux::cxx::unexpected(
                MaterialSourceFailure{EMaterialSourceError::PARSE_FAILURE, {}, {}, {}, point.line, point.column});
        }
        const auto &table = parsed.table();
        if (!fields(table, {"format", "version", "id", "name", "shading", "alpha_mode", "alpha_cutoff", "double_sided",
                            "textures", "parameters", "nodes", "links"}))
        {
            return fail(EMaterialSourceError::UNKNOWN_FIELD, "source");
        }
        if (table["format"].value<std::string_view>() != "lux.material.source" || table["version"].value<int>() != 1)
        {
            return fail(EMaterialSourceError::UNSUPPORTED_FORMAT);
        }
        const auto uuid_text = table["id"].value<std::string_view>();
        const auto uuid = uuid_text ? uuids::uuid::from_string(*uuid_text) : std::optional<uuids::uuid>{};
        if (!uuid || uuid->is_nil())
        {
            return fail(EMaterialSourceError::INVALID_IDENTITY, "id");
        }
        const auto name = text(table["name"], limits);
        const auto shading = integer(table["shading"], static_cast<unsigned>(lux::rdesc::ELightingTechnique::Graph));
        const auto alpha = integer(table["alpha_mode"], static_cast<unsigned>(lux::rdesc::EAlphaMode::Blend));
        const auto cutoff = table["alpha_cutoff"].value<double>();
        const auto double_sided = table["double_sided"].value<bool>();
        if (!name || name->empty() || !shading || !alpha || !cutoff || !std::isfinite(static_cast<float>(*cutoff)) ||
            !double_sided)
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "source");
        }
        MaterialSourceDocument result;
        result.id = lux::asset::AssetId{*uuid};
        result.name = *name;
        result.graph.shading_model = static_cast<lux::rdesc::ELightingTechnique>(*shading);
        result.graph.render_state = {static_cast<lux::rdesc::EAlphaMode>(*alpha), static_cast<float>(*cutoff),
                                     *double_sided};
        const auto *textures = table["textures"].as_array();
        const auto *parameters = table["parameters"].as_array();
        const auto *nodes = table["nodes"].as_array();
        const auto *links = table["links"].as_array();
        if (!textures || !parameters || !nodes || !links)
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "arrays");
        }
        if (textures->size() > limits.max_slots || parameters->size() > limits.max_slots ||
            nodes->size() > limits.max_nodes || links->size() > limits.max_pins)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED);
        }
        for (const auto &entry : *textures)
        {
            const auto *record = entry.as_table();
            if (!record || !fields(*record, {"name", "asset"}))
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "texture");
            }
            const auto slot_name = text((*record)["name"], limits);
            const auto asset = (*record)["asset"].value<std::string_view>();
            const auto id = asset ? uuids::uuid::from_string(*asset) : std::optional<uuids::uuid>{};
            if (!id || !slot_name)
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "texture");
            }
            result.graph.texture_slots.push_back({*slot_name, lux::asset::AssetId{*id}});
        }
        for (const auto &entry : *parameters)
        {
            const auto *record = entry.as_table();
            if (!record || !fields(*record, {"name", "type", "default"}))
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "parameter");
            }
            const auto slot_name = text((*record)["name"], limits);
            const auto type = integer((*record)["type"], 3);
            ParamSlotDecl slot;
            if (!slot_name || !type || !readFloats((*record)["default"], slot.dflt))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
            slot.name = *slot_name;
            slot.type = static_cast<EValueType>(*type);
            result.graph.param_slots.push_back(std::move(slot));
        }
        std::unordered_set<PinId> pin_ids;
        for (const auto &entry : *nodes)
        {
            const auto *record = entry.as_table();
            if (!record || !fields(*record, {"id", "name", "kind", "payload", "inputs", "outputs", "layout"}))
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "node");
            }
            const auto id = identity((*record)["id"]);
            if (!id || result.graph.node(NodeId{*id}))
            {
                return fail(EMaterialSourceError::INVALID_IDENTITY, "node");
            }
            const auto node_name = text((*record)["name"], limits);
            const auto kind_text = (*record)["kind"].value<std::string_view>();
            const auto kind = kind_text ? std::find(kinds.begin() + 1, kinds.end(), *kind_text) : kinds.end();
            if (kind == kinds.end())
            {
                return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "kind", NodeId{*id});
            }
            const auto *data = (*record)["payload"].as_table();
            if (!node_name || !data)
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "node", NodeId{*id});
            }
            auto made = makeNode(static_cast<EMatNodeKind>(kind - kinds.begin()), *data);
            if (!made)
            {
                auto failure = made.error();
                failure.node = NodeId{*id};
                return lux::cxx::unexpected(std::move(failure));
            }
            auto node = std::move(*made);
            node->setName(*node_name);
            const auto inputs = decodePins((*record)["inputs"], node->inputs(), pin_ids, limits);
            const auto outputs = decodePins((*record)["outputs"], node->outputs(), pin_ids, limits);
            if (!inputs)
            {
                return lux::cxx::unexpected(inputs.error());
            }
            if (!outputs)
            {
                return lux::cxx::unexpected(outputs.error());
            }
            if (!result.graph.addNodeWithId(NodeId{*id}, std::move(node)).valid())
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "node", NodeId{*id});
            }
            if (record->contains("layout"))
            {
                const auto *layout = (*record)["layout"].as_table();
                if (!layout || !fields(*layout, {"x", "y", "placed"}))
                {
                    return fail(EMaterialSourceError::UNKNOWN_FIELD, "layout", NodeId{*id});
                }
                const auto x = (*layout)["x"].value<double>(), y = (*layout)["y"].value<double>();
                const auto placed = (*layout)["placed"].value<bool>();
                const bool invalid_position = !x || !y || !placed || !std::isfinite(static_cast<float>(*x)) ||
                                              !std::isfinite(static_cast<float>(*y));
                if (invalid_position)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "layout", NodeId{*id});
                }
                const auto installed =
                    result.graph.layout().set(NodeId{*id}, {static_cast<float>(*x), static_cast<float>(*y), *placed});
                if (!installed)
                {
                    return fail(EMaterialSourceError::INVALID_TOPOLOGY, "layout", NodeId{*id});
                }
            }
        }
        for (const auto &entry : *links)
        {
            const auto *record = entry.as_table();
            if (!record || !fields(*record, {"from", "to"}))
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "link");
            }
            const auto from = identity((*record)["from"]), to = identity((*record)["to"]);
            if (!from || !to)
            {
                return fail(EMaterialSourceError::INVALID_IDENTITY, "link");
            }
            const auto linked = result.graph.topology().connect(PinId{*from}, PinId{*to});
            if (!linked)
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "link", {}, PinId{*to});
            }
        }
        const auto validated = validateMaterialSource(result, limits);
        if (!validated)
        {
            return lux::cxx::unexpected(validated.error());
        }
        return result;
    }
} // namespace lux::material
