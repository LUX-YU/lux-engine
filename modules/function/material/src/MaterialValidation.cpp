#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/detail/MaterialValidation.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::material::detail
{
    namespace
    {
        auto invalidGraph(std::string message, NodeId node = {}, std::uint32_t pin = ~std::uint32_t{0}) noexcept
        {
            return cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, std::move(message), node, pin}
            );
        }

        bool validType(EValueType type) noexcept
        {
            return type >= EValueType::FLOAT && type <= EValueType::VEC4;
        }
    } // namespace

    MaterialNodeResult<MaterialCompileGraph> validateMaterialGraph(
        const MaterialGraph& graph,
        shadergen::ShaderIR& resources
    ) noexcept
    {
        const bool has_nodes = !graph.nodes().empty();
        const bool exceeds_parameters = graph.param_slots.size() > rdesc::MaterialDescription::kMaxParams;
        const bool exceeds_textures = graph.texture_slots.size() > rdesc::MaterialDescription::kMaxTextures;
        const bool is_invalid_capacity = !has_nodes || exceeds_parameters || exceeds_textures;
        if (is_invalid_capacity)
        {
            return invalidGraph("invalid material graph capacity");
        }
        const auto alpha_mode = static_cast<std::uint8_t>(graph.render_state.alpha_mode);
        const auto shading_model = static_cast<std::uint8_t>(graph.shading_model);
        const bool is_invalid_alpha = alpha_mode > static_cast<std::uint8_t>(rdesc::EAlphaMode::BLEND);
        const bool is_invalid_shading = shading_model > static_cast<std::uint8_t>(rdesc::ELightingTechnique::GRAPH);
        const bool is_invalid_cutoff = !std::isfinite(graph.render_state.alpha_cutoff);
        const bool is_invalid_state = is_invalid_alpha || is_invalid_shading || is_invalid_cutoff;
        if (is_invalid_state)
        {
            return invalidGraph("invalid material render state");
        }
        for (const auto& texture : graph.texture_slots)
        {
            if (texture.texture.isNull())
            {
                return invalidGraph("material texture slot has a null AssetId");
            }
            resources.textures.push_back({texture.name});
        }
        for (const auto& parameter : graph.param_slots)
        {
            if (!validType(parameter.type))
            {
                return invalidGraph("material parameter has an invalid value type");
            }
            for (const float value : parameter.dflt)
            {
                if (!std::isfinite(value))
                {
                    return invalidGraph("material parameter default is not finite");
                }
            }
            shadergen::ParamSlot slot;
            slot.name = parameter.name;
            slot.type = static_cast<shadergen::EValueType>(parameter.type);
            std::copy_n(parameter.dflt, 4, slot.dflt);
            resources.params.push_back(std::move(slot));
        }

        MaterialCompileGraph result;
        result.nodes.reserve(graph.topology().nodes().size());
        result.pins.reserve(graph.topology().pins().size());
        result.incoming.reserve(graph.topology().links().size());
        using Semantics = std::unordered_map<lux::graph::PinSemanticId, const lux::graph::PinRecord*>;
        std::unordered_map<NodeId, Semantics> by_node;
        for (const auto& record : graph.topology().pins())
        {
            result.pins.emplace(record.id, &record);
            by_node[record.owner].emplace(record.semantic, &record);
        }
        for (const auto& record : graph.topology().nodes())
        {
            const auto* node = graph.node(record.id);
            const bool has_node = node && node->definition;
            const bool has_matching_type = has_node && node->definition->identity().id == record.type;
            if (!has_matching_type)
            {
                return invalidGraph("material graph contains an invalid node identity", record.id);
            }
            auto admitted = node->definition->validateBindings(node->payload, resources);
            if (!admitted)
            {
                admitted.error().node_id = record.id;
                return cxx::unexpected(std::move(admitted.error()));
            }
            auto declarations = node->definition->describePins(node->payload);
            if (!declarations)
            {
                declarations.error().node_id = record.id;
                return cxx::unexpected(std::move(declarations.error()));
            }
            auto& stored = by_node[record.id];
            if (stored.size() != declarations->size())
            {
                return invalidGraph("invalid material node pin arity", record.id);
            }
            MaterialCompileNode compiled;
            std::uint32_t input_index{};
            std::uint32_t output_index{};
            for (auto& declaration : *declarations)
            {
                const bool is_input = declaration.direction == lux::graph::EPinDirection::INPUT;
                const auto index = is_input ? input_index++ : output_index++;
                const auto found = stored.find(declaration.semantic);
                const bool has_record = found != stored.end();
                const bool has_direction = has_record && found->second->direction == declaration.direction;
                if (!has_direction)
                {
                    return invalidGraph("invalid material pin contract", record.id, index);
                }
                const auto pin_id = found->second->id;
                const auto* value = graph.pin(pin_id);
                const bool has_value = value && validType(value->type);
                const bool is_finite =
                    has_value &&
                    std::ranges::all_of(value->constant, [](float x) noexcept { return std::isfinite(x); });
                if (!is_finite)
                {
                    return invalidGraph("invalid material pin contract", record.id, index);
                }
                if (value->type != declaration.type)
                {
                    return invalidGraph("material node pin type does not match its payload", record.id, index);
                }
                if (is_input)
                {
                    compiled.inputs.push_back({pin_id, std::move(declaration)});
                }
                else
                {
                    compiled.outputs.push_back(pin_id);
                }
            }
            result.nodes.emplace(record.id, std::move(compiled));
        }
        for (const auto& link : graph.topology().links())
        {
            result.incoming.emplace(link.to, link.from);
        }
        return result;
    }
} // namespace lux::material::detail
