#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/detail/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/detail/MaterialMath.hpp>
#include <lux/engine/material/detail/MaterialToml.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <sstream>
#include <toml++/toml.hpp>
#include <unordered_set>

namespace lux::material
{
    namespace detail
    {
        struct MaterialSourceAccess final
        {
            static cxx::expected<void, graph::GraphTopologyFailure> restoreLinks(
                MaterialGraph& graph,
                std::span<const graph::LinkRecord> links
            ) noexcept
            {
                graph::GraphEdit candidate(graph.topology_, graph.layout_);
                for (const auto& link : links)
                {
                    auto accepted = candidate.connect(link.from, link.to);
                    if (!accepted)
                    {
                        return accepted;
                    }
                }
                candidate.commit();
                return {};
            }
        };
    } // namespace detail

    namespace
    {
        using detail::fields;
        using detail::floats;
        using detail::integer;
        using detail::readFloats;

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

        std::optional<std::uint64_t> identity(const toml::node_view<const toml::node>& value) noexcept
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

        std::optional<std::string> text(
            const toml::node_view<const toml::node>& value,
            MaterialSourceLimits limits
        ) noexcept
        {
            auto result = value.value<std::string>();
            if (!result || !textValid(*result, limits))
            {
                return {};
            }
            return result;
        }

        auto payloadFailure(MaterialCompileFailure cause, NodeId id) noexcept
        {
            cause.node_id = id;
            MaterialSourceFailure error{EMaterialSourceError::INVALID_VALUE, "node.payload", id};
            error.cause = std::move(cause);
            return cxx::unexpected(std::move(error));
        }

        std::string legacyType(std::string_view kind)
        {
            constexpr std::string_view Kinds[]{
                "constant",
                "input",
                "sample_texture",
                "parameter",
                "math",
                "swizzle",
                "construct",
                "decode_normal",
                "tbn_transform",
                "output_surface"
            };
            if (std::ranges::find(Kinds, kind) == std::end(Kinds))
            {
                return {};
            }
            return "lux.material." + std::string(kind) + ".v1";
        }

        MaterialSourceResult<std::vector<MaterialPinEntry>> decodePins(
            const toml::table& node,
            NodeId id,
            std::span<const MaterialPinDeclaration> schema,
            bool legacy,
            std::unordered_set<PinId>& identities,
            MaterialSourceLimits limits
        ) noexcept
        {
            std::vector<MaterialPinEntry> result;
            if (schema.size() > limits.max_pins - identities.size())
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "pins", id);
            }
            result.reserve(schema.size());
            if (legacy)
            {
                for (const auto direction : {graph::EPinDirection::INPUT, graph::EPinDirection::OUTPUT})
                {
                    const auto* array =
                        node[direction == graph::EPinDirection::INPUT ? "inputs" : "outputs"].as_array();
                    const auto count = std::ranges::count(schema, direction, &MaterialPinDeclaration::direction);
                    if (!array || array->size() != static_cast<std::size_t>(count))
                    {
                        return fail(EMaterialSourceError::INVALID_VALUE, "pins", id);
                    }
                }
            }
            const auto* all = node["pins"].as_array();
            if (!legacy && (!all || all->size() != schema.size()))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "pins", id);
            }
            std::size_t input_index{}, output_index{};
            for (const auto& declaration : schema)
            {
                const toml::table* record{};
                if (legacy)
                {
                    const bool input = declaration.direction == graph::EPinDirection::INPUT;
                    const auto* array = node[input ? "inputs" : "outputs"].as_array();
                    record = (*array)[input ? input_index++ : output_index++].as_table();
                }
                else
                {
                    for (const auto& item : *all)
                    {
                        const auto* candidate = item.as_table();
                        if (candidate && identity((*candidate)["semantic"]) == declaration.semantic.value)
                        {
                            if (record)
                            {
                                return fail(EMaterialSourceError::INVALID_IDENTITY, "pin.semantic", id);
                            }
                            record = candidate;
                        }
                    }
                }
                if (!record)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "pins", id);
                }
                const bool known = legacy ? fields(*record, {"id", "name", "type", "default"})
                                          : fields(*record, {"id", "semantic", "name", "type", "default"});
                if (!known)
                {
                    return fail(EMaterialSourceError::UNKNOWN_FIELD, "pin", id);
                }
                const auto pin_id = identity((*record)["id"]);
                const auto name = text((*record)["name"], limits);
                const auto type = integer((*record)["type"], 3);
                if (!pin_id || !identities.emplace(PinId{*pin_id}).second)
                {
                    return fail(EMaterialSourceError::INVALID_IDENTITY, "pin", id);
                }
                MaterialPinPayload value;
                if (!name || !type || !readFloats((*record)["default"], value.constant))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "pin", id, PinId{*pin_id});
                }
                value.name = *name;
                value.type = static_cast<EValueType>(*type);
                const auto fan = declaration.direction == graph::EPinDirection::INPUT ? 1 : graph::kUnlimitedFan;
                result.push_back(
                    {{PinId{*pin_id}, id, declaration.direction, static_cast<std::uint8_t>(fan), declaration.semantic},
                     std::move(value)}
                );
            }
            return result;
        }
    } // namespace

    MaterialSourceResult<void> validateMaterialName(std::string_view name, MaterialSourceLimits limits) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (name.empty() || !textValid(name, limits))
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "name");
        }
        return {};
    }

    MaterialSourceResult<void> validateMaterialTextureSlots(
        std::span<const TextureSlotDecl> slots,
        MaterialSourceLimits limits
    ) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (slots.size() > limits.max_slots)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED, "textures");
        }
        std::size_t text_bytes{};
        for (const auto& slot : slots)
        {
            if (!textValid(slot.name, limits))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "texture.name");
            }
            if (slot.name.size() > limits.max_bytes - text_bytes)
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "textures");
            }
            text_bytes += slot.name.size();
        }
        return {};
    }

    MaterialSourceResult<void> validateMaterialParameterSlots(
        std::span<const ParamSlotDecl> slots,
        MaterialSourceLimits limits
    ) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (slots.size() > limits.max_slots)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED, "parameters");
        }
        std::size_t text_bytes{};
        for (const auto& slot : slots)
        {
            if (!textValid(slot.name, limits) || !typeValid(slot.type) || !finite(slot.dflt))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
            if (slot.name.size() > limits.max_bytes - text_bytes)
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "parameters");
            }
            text_bytes += slot.name.size();
        }
        return {};
    }

    MaterialSourceResult<void> validateMaterialNode(
        const MaterialNode& node,
        NodeId id,
        MaterialSourceLimits limits
    ) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (!textValid(node.name, limits))
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "node.name", id);
        }
        if (!node.definition)
        {
            return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "node.type", id);
        }
        auto schema = node.definition->describePins(node.payload);
        if (!schema)
        {
            return payloadFailure(std::move(schema.error()), id);
        }
        if (schema->size() > limits.max_pins)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED, "pins", id);
        }
        return {};
    }

    MaterialSourceResult<void> validateMaterialSource(
        const MaterialSource& source,
        MaterialSourceLimits limits
    ) noexcept
    {
        if (!validLimits(limits))
        {
            return fail(EMaterialSourceError::INVALID_ARGUMENT);
        }
        if (source.id.isNull())
        {
            return fail(EMaterialSourceError::INVALID_IDENTITY, "id");
        }
        const auto& graph = source.graph;
        if (const auto checked = validateMaterialName(source.name, limits); !checked)
        {
            return checked;
        }
        if (const auto checked = validateMaterialTextureSlots(graph.texture_slots, limits); !checked)
        {
            return checked;
        }
        if (const auto checked = validateMaterialParameterSlots(graph.param_slots, limits); !checked)
        {
            return checked;
        }
        const bool invalid_source = graph.shading_model > lux::rdesc::ELightingTechnique::GRAPH ||
                                    graph.render_state.alpha_mode > lux::rdesc::EAlphaMode::BLEND ||
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
            if (value.size() > limits.max_bytes - text_bytes)
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
        for (const auto& slot : graph.texture_slots)
        {
            if (!count_text(slot.name))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "texture.name");
            }
        }
        for (const auto& slot : graph.param_slots)
        {
            if (!count_text(slot.name))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
        }
        std::unordered_map<NodeId, std::unordered_map<graph::PinSemanticId, const graph::PinRecord*>> pins;
        for (const auto& record : graph.topology().pins())
        {
            pins[record.owner].emplace(record.semantic, &record);
        }
        for (const auto& record : graph.topology().nodes())
        {
            const auto id = record.id;
            const auto* node = graph.node(id);
            const bool has_node = node && node->definition;
            const bool has_identity = has_node && node->definition->identity().id == record.type;
            if (!has_identity)
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "node.type", id);
            }
            if (const auto checked = validateMaterialNode(*node, id, limits); !checked)
            {
                return checked;
            }
            if (!count_text(node->name) || !count_text(node->definition->identity().canonical_name))
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "node.name", id);
            }
            auto schema = node->definition->describePins(node->payload);
            if (!schema)
            {
                return payloadFailure(std::move(schema.error()), id);
            }
            const auto& stored = pins[id];
            if (stored.size() != schema->size())
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pins", id);
            }
            for (const auto& declaration : *schema)
            {
                const auto found = stored.find(declaration.semantic);
                const bool has_pin = found != stored.end();
                const bool has_direction = has_pin && found->second->direction == declaration.direction;
                if (!has_direction)
                {
                    return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pin", id);
                }
                const auto pin_id = found->second->id;
                const auto* value = graph.pin(pin_id);
                const bool valid_value = value && typeValid(value->type) && textValid(value->name, limits) &&
                                         finite(value->constant) && count_text(value->name);
                if (!valid_value)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "pin", id, pin_id);
                }
                ++pin_count;
            }
        }
        if (pin_count != graph.topology().pins().size())
        {
            return fail(EMaterialSourceError::INVALID_TOPOLOGY, "pins");
        }
        for (const auto& entry : graph.layout().all())
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

    MaterialSourceResult<std::string> encodeMaterialSource(
        const MaterialSource& source,
        MaterialSourceLimits limits
    ) noexcept
    {
        if (const auto checked = validateMaterialSource(source, limits); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        const auto& graph = source.graph;
        std::vector<MaterialNodeEntry> ordered;
        ordered.reserve(graph.nodes().size());
        for (const auto& [id, node] : graph.nodes())
        {
            ordered.push_back({id, node});
        }
        std::ranges::sort(ordered, {}, &MaterialNodeEntry::id);
        toml::table document{
            {"format", "lux.material.source"},
            {"version", 2},
            {"id", uuids::to_string(source.id.uuid())},
            {"name", source.name},
            {"shading", static_cast<std::int64_t>(graph.shading_model)},
            {"alpha_mode", static_cast<std::int64_t>(graph.render_state.alpha_mode)},
            {"alpha_cutoff", static_cast<double>(graph.render_state.alpha_cutoff)},
            {"double_sided", graph.render_state.double_sided}
        };
        toml::array textures, parameters, nodes, links;
        for (const auto& slot : graph.texture_slots)
        {
            textures.push_back(toml::table{{"name", slot.name}, {"asset", uuids::to_string(slot.texture.uuid())}});
        }
        for (const auto& slot : graph.param_slots)
        {
            if (!typeValid(slot.type) || !finite(slot.dflt))
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "parameter");
            }
            parameters.push_back(toml::table{
                {"name", slot.name},
                {"type", static_cast<std::int64_t>(slot.type)},
                {"default", floats(slot.dflt)}
            });
        }
        std::unordered_map<NodeId, std::vector<const graph::PinRecord*>> pins;
        for (const auto& pin : graph.topology().pins())
        {
            pins[pin.owner].push_back(&pin);
        }
        for (const auto& entry : ordered)
        {
            const auto id = entry.id;
            const auto* node = entry.value;
            auto data = node->definition->encode(node->payload);
            if (!data)
            {
                return payloadFailure(std::move(data.error()), id);
            }
            if (data->size() > limits.max_bytes)
            {
                return fail(EMaterialSourceError::LIMIT_EXCEEDED, "node.payload", id);
            }
            toml::array encoded_pins;
            for (const auto* pin : pins[id])
            {
                const auto& value = *graph.pin(pin->id);
                encoded_pins.push_back(toml::table{
                    {"id", std::to_string(pin->id.value)},
                    {"semantic", std::to_string(pin->semantic.value)},
                    {"name", value.name},
                    {"type", static_cast<std::int64_t>(value.type)},
                    {"default", floats(value.constant)}
                });
            }
            const auto& type = node->definition->identity();
            toml::table record{
                {"id", std::to_string(id.value)},
                {"name", node->name},
                {"type", type.canonical_name},
                {"type_version", type.version},
                {"payload", std::move(*data)},
                {"pins", std::move(encoded_pins)}
            };
            if (const auto* layout = graph.layout().find(id))
            {
                if (!std::isfinite(layout->x) || !std::isfinite(layout->y))
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "layout", id);
                }
                record.insert(
                    "layout",
                    toml::table{
                        {"x", static_cast<double>(layout->x)},
                        {"y", static_cast<double>(layout->y)},
                        {"placed", layout->placed}
                    }
                );
            }
            nodes.push_back(std::move(record));
        }
        for (const auto& link : graph.topology().links())
        {
            links.push_back(
                toml::table{{"from", std::to_string(link.from.value)}, {"to", std::to_string(link.to.value)}}
            );
        }
        for (const auto& layout : graph.layout().all())
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

    MaterialSourceResult<MaterialSource> decodeMaterialSource(
        std::string_view bytes,
        const MaterialNodeCatalog& catalog,
        MaterialSourceLimits limits
    ) noexcept
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
                MaterialSourceFailure{EMaterialSourceError::PARSE_FAILURE, {}, {}, {}, point.line, point.column}
            );
        }
        const auto& table = parsed.table();
        if (!fields(
                table,
                {"format",
                 "version",
                 "id",
                 "name",
                 "shading",
                 "alpha_mode",
                 "alpha_cutoff",
                 "double_sided",
                 "textures",
                 "parameters",
                 "nodes",
                 "links"}
            ))
        {
            return fail(EMaterialSourceError::UNKNOWN_FIELD, "source");
        }
        const auto version = table["version"].value<int>();
        const bool has_format = table["format"].value<std::string_view>() == "lux.material.source";
        const bool has_version = version == 1 || version == 2;
        if (!has_format || !has_version)
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
        const auto shading = integer(table["shading"], static_cast<unsigned>(lux::rdesc::ELightingTechnique::GRAPH));
        const auto alpha = integer(table["alpha_mode"], static_cast<unsigned>(lux::rdesc::EAlphaMode::BLEND));
        const auto cutoff = table["alpha_cutoff"].value<double>();
        const auto double_sided = table["double_sided"].value<bool>();
        if (!name || name->empty() || !shading || !alpha || !cutoff || !std::isfinite(static_cast<float>(*cutoff)) ||
            !double_sided)
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "source");
        }
        MaterialSource result;
        result.id = lux::asset::AssetId{*uuid};
        result.name = *name;
        result.graph.shading_model = static_cast<lux::rdesc::ELightingTechnique>(*shading);
        result.graph
            .render_state = {static_cast<lux::rdesc::EAlphaMode>(*alpha), static_cast<float>(*cutoff), *double_sided};
        const auto* textures = table["textures"].as_array();
        const auto* parameters = table["parameters"].as_array();
        const auto* nodes = table["nodes"].as_array();
        const auto* links = table["links"].as_array();
        if (!textures || !parameters || !nodes || !links)
        {
            return fail(EMaterialSourceError::INVALID_VALUE, "arrays");
        }
        if (textures->size() > limits.max_slots || parameters->size() > limits.max_slots ||
            nodes->size() > limits.max_nodes || links->size() > limits.max_pins)
        {
            return fail(EMaterialSourceError::LIMIT_EXCEEDED);
        }
        for (const auto& entry : *textures)
        {
            const auto* record = entry.as_table();
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
        for (const auto& entry : *parameters)
        {
            const auto* record = entry.as_table();
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
        for (const auto& entry : *nodes)
        {
            const auto* record = entry.as_table();
            if (!record)
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "node");
            }
            const bool legacy = *version == 1;
            const bool known_fields =
                legacy ? fields(*record, {"id", "name", "kind", "payload", "inputs", "outputs", "layout"})
                       : fields(*record, {"id", "name", "type", "type_version", "payload", "pins", "layout"});
            if (!known_fields)
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "node");
            }
            const auto id = identity((*record)["id"]);
            if (!id || result.graph.node(NodeId{*id}))
            {
                return fail(EMaterialSourceError::INVALID_IDENTITY, "node");
            }
            const auto node_name = text((*record)["name"], limits);
            const auto type_name = legacy ? legacyType((*record)["kind"].value_or(std::string{}))
                                          : (*record)["type"].value_or(std::string{});
            const auto type_version =
                legacy ? std::optional<std::uint32_t>{1} : integer((*record)["type_version"], UINT32_MAX);
            auto definition = catalog.find(graph::nodeTypeId(type_name));
            const bool has_definition = definition && definition->identity().canonical_name == type_name;
            if (!has_definition)
            {
                return fail(EMaterialSourceError::UNKNOWN_NODE_KIND, "node.type", NodeId{*id});
            }
            if (type_version != definition->identity().version)
            {
                return fail(EMaterialSourceError::UNSUPPORTED_FORMAT, "node.type_version", NodeId{*id});
            }
            std::string payload_bytes;
            if (legacy)
            {
                const auto* data = (*record)["payload"].as_table();
                if (!data)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "node.payload", NodeId{*id});
                }
                std::ostringstream stream;
                stream.imbue(std::locale::classic());
                stream << toml::toml_formatter{*data};
                payload_bytes = std::move(stream).str();
            }
            else
            {
                auto data = (*record)["payload"].value<std::string>();
                if (!data)
                {
                    return fail(EMaterialSourceError::INVALID_VALUE, "node.payload", NodeId{*id});
                }
                payload_bytes = std::move(*data);
            }
            if (!node_name)
            {
                return fail(EMaterialSourceError::INVALID_VALUE, "node.name", NodeId{*id});
            }
            auto payload = definition->decode(payload_bytes);
            if (!payload)
            {
                return payloadFailure(std::move(payload.error()), NodeId{*id});
            }
            auto schema = definition->describePins(*payload);
            if (!schema)
            {
                return payloadFailure(std::move(schema.error()), NodeId{*id});
            }
            auto restored = decodePins(*record, NodeId{*id}, *schema, legacy, pin_ids, limits);
            if (!restored)
            {
                return cxx::unexpected(std::move(restored.error()));
            }
            // v1 Math's output type was an operand hint. Normalize that exact legacy hint only;
            // deliberately mismatched draft pins remain invalid at compilation.
            if (legacy)
            {
                if (const auto* math = payload->get<MaterialMath>())
                {
                    for (auto& pin : *restored)
                    {
                        const bool is_output = pin.record.direction == graph::EPinDirection::OUTPUT;
                        if (is_output && pin.value.type == math->operand_type)
                        {
                            pin.value.type = detail::mathOutputType(*math);
                        }
                    }
                }
            }
            MaterialNode node{std::move(definition), *node_name, std::move(*payload)};
            auto inserted = result.graph.addNodeWithId(NodeId{*id}, std::move(node), *restored);
            if (!inserted)
            {
                return fail(EMaterialSourceError::INVALID_TOPOLOGY, "node", NodeId{*id});
            }
            if (record->contains("layout"))
            {
                const auto* layout = (*record)["layout"].as_table();
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
                const graph::GraphLayoutEntry position{
                    NodeId{*id},
                    {static_cast<float>(*x), static_cast<float>(*y), *placed}
                };
                MaterialGraphChange change;
                change.place = {&position, 1};
                auto installed = MaterialGraphEdit::prepare(result.graph, change);
                if (!installed)
                {
                    return fail(EMaterialSourceError::INVALID_TOPOLOGY, "layout", NodeId{*id});
                }
                installed->commit();
            }
        }
        std::vector<graph::LinkRecord> restored_links;
        restored_links.reserve(links->size());
        for (const auto& entry : *links)
        {
            const auto* record = entry.as_table();
            if (!record || !fields(*record, {"from", "to"}))
            {
                return fail(EMaterialSourceError::UNKNOWN_FIELD, "link");
            }
            const auto from = identity((*record)["from"]), to = identity((*record)["to"]);
            if (!from || !to)
            {
                return fail(EMaterialSourceError::INVALID_IDENTITY, "link");
            }
            restored_links.push_back({PinId{*from}, PinId{*to}});
        }
        auto linked = detail::MaterialSourceAccess::restoreLinks(result.graph, restored_links);
        if (!linked)
        {
            return fail(EMaterialSourceError::INVALID_TOPOLOGY, "link", linked.error().node, linked.error().pin);
        }
        const auto validated = validateMaterialSource(result, limits);
        if (!validated)
        {
            return lux::cxx::unexpected(validated.error());
        }
        return result;
    }

    MaterialSourceResult<MaterialSource> decodeMaterialSource(
        std::string_view bytes,
        MaterialSourceLimits limits
    ) noexcept
    {
        MaterialNodeCatalog catalog;
        const auto registrations = materialBuiltinRegistrations();
        if (!catalog.add(registrations))
        {
            std::terminate();
        }
        return decodeMaterialSource(bytes, catalog, limits);
    }
} // namespace lux::material
