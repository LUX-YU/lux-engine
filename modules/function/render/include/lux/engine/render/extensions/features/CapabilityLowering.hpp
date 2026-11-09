#pragma once

#include <lux/engine/render/extensions/features/CapabilityGraph.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>

namespace lux::render
{
    // Projects a typed frame input into the existing instance parameter bytes. The complete
    // type/size contract is checked before writing; failure leaves those bytes unchanged.
    using FrameParameterProjection =
        RenderResult<void> (*)(cxx::type_info, std::span<const std::byte>, std::span<std::byte>) noexcept;

    struct ParameterProjection final
    {
        cxx::type_info type;
        FrameParameterProjection apply = nullptr;
        std::uint32_t offset = 0, bytes = 0;
    };

    // Synchronous professional lowering input. All borrows end when lower returns.
    // Configuration is qualified by the caller; lowering checks the input types it consumes.
    struct CapabilityBuildInput final
    {
        const EffectOutputDesc& output;
        const ConfigValue& configuration;
        // Input shapes come from the connected producer's actual lowered output.
        // Unconnected optional inputs retain their declaration with required=false.
        std::span<const CapabilityPort> ports;
        std::span<const std::uint8_t> connected;
        std::span<const FeatureId> features;
    };

    // A local authoring fragment, immediately appended to the single EffectDefinition.
    // Each public port maps to actual local Feature ports; inputs may fan out, outputs
    // have exactly one source. This value owns no live Feature/Storage/Graph instance.
    struct CapabilityGraphContribution final
    {
        EffectDefinition definition;
        std::vector<std::vector<FeaturePortRef>> ports;
    };
} // namespace lux::render
