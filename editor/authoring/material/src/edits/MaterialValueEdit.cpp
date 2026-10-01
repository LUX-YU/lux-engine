#include "MaterialEditPreparation.hpp"

namespace lux::editor::material::detail
{
    namespace
    {
        bool sameTarget(const VMaterialValue& a, const VMaterialValue& b) noexcept
        {
            if (a.index() != b.index())
                return false;
            const auto* constant = std::get_if<MaterialSetConstant>(&a);
            return !constant || constant->node == std::get<MaterialSetConstant>(b).node;
        }
        std::size_t valueBytes(const VMaterialValue& value) noexcept
        {
            return std::visit(
                [](const auto& item) -> std::size_t {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::same_as<T, MaterialRename>)
                        return item.value.capacity() + 1;
                    else if constexpr (std::same_as<T, MaterialSetTextureSlots> ||
                                       std::same_as<T, MaterialSetParameterSlots>)
                    {
                        std::size_t bytes = item.value.capacity() * sizeof(typename decltype(item.value)::value_type);
                        for (const auto& slot : item.value)
                            bytes += slot.name.capacity() + 1;
                        return bytes;
                    }
                    else
                        return 0;
                },
                value
            );
        }
        MaterialEditResult<void> validate(const VMaterialValue& value)
        {
            const bool valid = std::visit(
                [](const auto& item) -> bool {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::same_as<T, MaterialRename>)
                        return bool(lux::material::validateMaterialName(item.value));
                    else if constexpr (std::same_as<T, MaterialSetConstant>)
                        return std::ranges::all_of(item.value, [](float v) { return std::isfinite(v); });
                    else if constexpr (std::same_as<T, MaterialSetShading>)
                        return item.value <= lux::rdesc::ELightingTechnique::GRAPH;
                    else if constexpr (std::same_as<T, MaterialSetRenderState>)
                        return item.value.alpha_mode <= lux::rdesc::EAlphaMode::BLEND &&
                               std::isfinite(item.value.alpha_cutoff) && item.value.alpha_cutoff >= 0 &&
                               item.value.alpha_cutoff <= 1;
                    else if constexpr (std::same_as<T, MaterialSetTextureSlots>)
                        return bool(lux::material::validateMaterialTextureSlots(item.value));
                    else
                        return bool(lux::material::validateMaterialParameterSlots(item.value));
                },
                value
            );
            if (!valid)
                return rejected(EMaterialEditError::INVALID_VALUE);
            return {};
        }
    }
    MaterialEditResult<VMaterialValue> readValue(const lux::material::MaterialSource& source, const VMaterialValue& key)
    {
        return std::visit(
            [&](const auto& item) -> MaterialEditResult<VMaterialValue> {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::same_as<T, MaterialRename>)
                    return VMaterialValue{MaterialRename{source.name}};
                else if constexpr (std::same_as<T, MaterialSetConstant>)
                {
                    const auto* node = source.graph.node(item.node);
                    const auto* constant = node ? node->as<lux::material::ConstantNode>() : nullptr;
                    if (!constant)
                        return rejected(EMaterialEditError::INVALID_NODE, item.node);
                    MaterialSetConstant result{item.node};
                    std::ranges::copy(constant->value, result.value.begin());
                    return VMaterialValue{result};
                }
                else if constexpr (std::same_as<T, MaterialSetShading>)
                    return VMaterialValue{MaterialSetShading{source.graph.shading_model}};
                else if constexpr (std::same_as<T, MaterialSetRenderState>)
                    return VMaterialValue{MaterialSetRenderState{source.graph.render_state}};
                else if constexpr (std::same_as<T, MaterialSetTextureSlots>)
                    return VMaterialValue{MaterialSetTextureSlots{source.graph.texture_slots}};
                else
                    return VMaterialValue{MaterialSetParameterSlots{source.graph.param_slots}};
            },
            key
        );
    }
    bool equalValue(const VMaterialValue& a, const VMaterialValue& b) noexcept
    {
        if (!sameTarget(a, b))
            return false;
        return std::visit(
            [&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                const auto& other = std::get<T>(b);
                if constexpr (std::same_as<T, MaterialSetRenderState>)
                    return item.value.alpha_mode == other.value.alpha_mode &&
                           item.value.alpha_cutoff == other.value.alpha_cutoff &&
                           item.value.double_sided == other.value.double_sided;
                else
                    return item.value == other.value;
            },
            a
        );
    }
    void swapValue(lux::material::MaterialSource& source, VMaterialValue& value) noexcept
    {
        std::visit(
            [&](auto& item) {
                using T = std::decay_t<decltype(item)>;
                using std::swap;
                if constexpr (std::same_as<T, MaterialRename>)
                    swap(source.name, item.value);
                else if constexpr (std::same_as<T, MaterialSetConstant>)
                {
                    auto& constant = *source.graph.node(item.node)->as<lux::material::ConstantNode>();
                    for (std::size_t i{}; i < item.value.size(); ++i)
                        swap(constant.value[i], item.value[i]);
                }
                else if constexpr (std::same_as<T, MaterialSetShading>)
                    swap(source.graph.shading_model, item.value);
                else if constexpr (std::same_as<T, MaterialSetRenderState>)
                    swap(source.graph.render_state, item.value);
                else if constexpr (std::same_as<T, MaterialSetTextureSlots>)
                    swap(source.graph.texture_slots, item.value);
                else
                    swap(source.graph.param_slots, item.value);
            },
            value
        );
    }
    MaterialEditResult<void> MaterialValueEdit::set(const lux::material::MaterialSource& source, VMaterialValue value)
    {
        if (auto checked = validate(value); !checked)
            return checked;
        auto current = readValue(source, value);
        if (!current)
            return lux::cxx::unexpected(current.error());
        for (std::size_t i{}; i < after.size(); ++i)
            if (sameTarget(after[i], value))
            {
                after[i] = std::move(value);
                return {};
            }
        before.push_back(std::move(*current));
        after.push_back(std::move(value));
        return {};
    }
    void MaterialValueEdit::normalize()
    {
        for (std::size_t i = after.size(); i-- > 0;)
            if (equalValue(before[i], after[i]))
            {
                before.erase(before.begin() + i);
                after.erase(after.begin() + i);
            }
    }
    std::size_t MaterialValueEdit::bytes() const noexcept
    {
        std::size_t bytes = (before.capacity() + after.capacity()) * sizeof(VMaterialValue);
        for (const auto& item : before)
            bytes += valueBytes(item);
        for (const auto& item : after)
            bytes += valueBytes(item);
        return bytes;
    }
    std::size_t nodeBytes(const lux::material::Node& node) noexcept
    {
        const auto object = std::max(
            {sizeof(lux::material::ConstantNode),
             sizeof(lux::material::InputNode),
             sizeof(lux::material::SampleTextureNode),
             sizeof(lux::material::ParamNode),
             sizeof(lux::material::MathNode),
             sizeof(lux::material::SwizzleNode),
             sizeof(lux::material::ConstructNode),
             sizeof(lux::material::DecodeNormalNode),
             sizeof(lux::material::TbnTransformNode),
             sizeof(lux::material::OutputSurfaceNode)}
        );
        std::size_t bytes = object + node.name().capacity() + 1 +
                            (node.inputs().capacity() + node.outputs().capacity()) * sizeof(lux::material::DataPin);
        for (const auto& pin : node.inputs())
            bytes += pin.name.capacity() + 1;
        for (const auto& pin : node.outputs())
            bytes += pin.name.capacity() + 1;
        return bytes;
    }
    std::size_t sourceBytes(const lux::material::MaterialSource& source) noexcept
    {
        const auto& graph = source.graph;
        std::size_t bytes = sizeof(source) + source.name.capacity() + 1 +
                            graph.topology().nodes().size() * sizeof(lux::graph::NodeRecord) +
                            graph.topology().pins().size() * sizeof(lux::graph::PinRecord) +
                            graph.topology().links().size() * sizeof(lux::graph::LinkRecord) +
                            graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry) +
                            graph.texture_slots.capacity() * sizeof(lux::material::TextureSlotDecl) +
                            graph.param_slots.capacity() * sizeof(lux::material::ParamSlotDecl);
        for (const auto& [id, node] : graph.nodes())
            bytes += nodeBytes(*node);
        for (const auto& slot : graph.texture_slots)
            bytes += slot.name.capacity() + 1;
        for (const auto& slot : graph.param_slots)
            bytes += slot.name.capacity() + 1;
        return bytes;
    }
    MaterialEditResult<void> validateReferences(
        const lux::material::MaterialSource& before,
        const lux::material::MaterialGraph& graph,
        std::span<const lux::material::TextureSlotDecl> textures,
        std::span<const lux::material::ParamSlotDecl> parameters
    )
    {
        for (const auto& [id, node] : graph.nodes())
        {
            if (const auto* texture = node->as<lux::material::SampleTextureNode>())
            {
                const bool removed = texture->texture_slot < before.graph.texture_slots.size() &&
                                     texture->texture_slot >= textures.size();
                if (removed)
                    return rejected(EMaterialEditError::REFERENCE_IN_USE, id);
            }
            if (const auto* parameter = node->as<lux::material::ParamNode>())
            {
                const bool removed = parameter->param_slot < before.graph.param_slots.size() &&
                                     parameter->param_slot >= parameters.size();
                const bool mismatch = parameter->param_slot < parameters.size() &&
                                      parameter->type != parameters[parameter->param_slot].type;
                if (removed || mismatch)
                    return rejected(EMaterialEditError::REFERENCE_IN_USE, id);
            }
        }
        return {};
    }
}
