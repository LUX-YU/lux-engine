#include <lux/engine/flowforge/graph/FlowNode.hpp>
#include <lux/engine/meta/MetaCompat.hpp>

#include <utility>

namespace lux::flowforge
{
    FlowForgeResult<FlowNode> createFlowNode(
        std::shared_ptr<const FlowNodeType> definition,
        FlowNodePayload payload
    ) noexcept
    {
        if (!definition)
        {
            return cxx::unexpected(FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "missing node definition"});
        }
        auto pins = definition->describePins(payload);
        if (!pins)
        {
            return cxx::unexpected(std::move(pins.error()));
        }
        auto name = definition->identity().canonical_name;
        return FlowNode{std::move(definition), std::move(name), {}, std::move(payload)};
    }

    FlowForgeResult<FlowNode> FlowNode::clone() const noexcept
    {
        if (!definition)
        {
            return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "node has no definition"});
        }
        auto copy = payload.clone();
        if (!copy)
        {
            return cxx::unexpected(std::move(copy.error()));
        }
        return FlowNode{definition, name, creator, std::move(*copy)};
    }

    cxx::expected<FlowPinPayload, meta::ERuntimeObjectError> FlowPinPayload::clone() const noexcept
    {
        auto copy = default_value.clone();
        if (!copy)
        {
            return cxx::unexpected(copy.error());
        }
        return FlowPinPayload{name, role, type, allow_default, necessary, std::move(*copy)};
    }

    cxx::expected<void, meta::ERuntimeObjectError> FlowPinPayload::resetDefault() noexcept
    {
        const bool is_data = role == EFlowPinRole::DATA && type != nullptr;
        if (!is_data)
        {
            return cxx::unexpected(meta::ERuntimeObjectError::INVALID_TYPE);
        }
        auto candidate = meta::RuntimeObject::defaultOf(*type);
        if (!candidate)
        {
            return cxx::unexpected(candidate.error());
        }
        default_value = std::move(*candidate);
        return {};
    }

    bool FlowPinPayload::setDefault(meta::RuntimeObject candidate) noexcept
    {
        const bool is_data = role == EFlowPinRole::DATA && type != nullptr;
        const bool is_assignable = is_data && meta::canAssign(candidate.type(), type);
        if (!is_assignable)
        {
            return false;
        }
        default_value = std::move(candidate);
        return true;
    }

    FlowNodeSnapshot& FlowNodeSnapshot::operator=(FlowNodeSnapshot&& other) noexcept
    {
        if (this != &other)
        {
            // Pin values can use metadata owned by the old node definition.
            pins.clear();
            value = std::move(other.value);
            id = std::exchange(other.id, {});
            pins = std::move(other.pins);
            links = std::move(other.links);
            layout = std::move(other.layout);
        }
        return *this;
    }
} // namespace lux::flowforge
