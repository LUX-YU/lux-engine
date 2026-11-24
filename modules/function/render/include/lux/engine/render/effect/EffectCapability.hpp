#pragma once

#include <lux/engine/function/graph/GraphNodeTypeIdentity.hpp>
#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>

namespace lux::render
{
    // A public rendering meaning, not an image format, descriptor binding or shader field.
    // New semantic contracts are ordinary versioned data; no central enum must be extended.
    struct EffectValueType final
    {
        std::string_view canonical_name;
        std::uint32_t version = 1;
        [[nodiscard]] bool operator==(const EffectValueType&) const noexcept = default;
    };

    [[nodiscard]] constexpr graph::PinSemanticId effectPortSemantic(std::string_view name) noexcept
    {
        return graph::PinSemanticId{cxx::algorithm::fnv1a(name)};
    }

    struct EffectPortDefinition final
    {
        graph::PinSemanticId semantic;
        std::string_view name;
        EffectValueType type;
        graph::EPinDirection direction = graph::EPinDirection::INPUT;
        std::uint8_t fan_cap = 1; // GraphTopology links only; named exports are root references, not links.
        bool required = false; // INPUT only. Output demand comes exclusively from graph connections/exports.
    };

    [[nodiscard]] constexpr EffectPortDefinition
    inputPort(std::string_view name, EffectValueType type, bool required = false, std::uint8_t fan_cap = 1) noexcept
    {
        return {effectPortSemantic(name), name, type, graph::EPinDirection::INPUT, fan_cap, required};
    }

    [[nodiscard]] constexpr EffectPortDefinition
    outputPort(std::string_view name, EffectValueType type, std::uint8_t fan_cap = 1) noexcept
    {
        return {effectPortSemantic(name), name, type, graph::EPinDirection::OUTPUT, fan_cap};
    }

    struct ImplementationSelection final
    {
        std::string canonical_name;
        std::uint32_t version = 1;
        [[nodiscard]] bool operator==(const ImplementationSelection&) const noexcept = default;
    };

    // Discovery data for a registered implementation. It carries no executable operations.
    struct ImplementationOption final
    {
        ImplementationSelection identity;
        std::string display_name;
    };

    // Borrows its arrays and all nested names until synchronous registration returns. A Capability is the graph NodeType.
    // Implementations cannot add/reorder pins or change the node contract.
    struct EffectCapability final
    {
        graph::NodeTypeId id;
        std::string_view canonical_name;
        std::uint32_t version = 1;
        std::string_view display_name;
        std::span<const EffectPortDefinition> ports;
        std::uint32_t max_instances = 0; // 0 = unbounded; otherwise checked across all nodes in this graph.
        std::span<const ResourceRequirement> resources;
    };
}
