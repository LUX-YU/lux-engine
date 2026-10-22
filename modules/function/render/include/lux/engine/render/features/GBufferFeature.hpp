#pragma once

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{
    struct GBufferFeatureConfig final
    {
        TargetDesc target;
        std::vector<RasterMeshGroup> groups;
        // Composition can provide one larger typed input block; only view/projection are consumed here.
        rdesc::ShaderBlock input_layout = meshViewParameterBlock();
        // Nonzero: consume groupN.members/commands/counts from an explicit visibility producer,
        // one geometric view per Attachment. Zero: render all candidates as a reference path.
        std::uint32_t visible_capacity = 0;
        bool operator==(const GBufferFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<GBufferFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const GBufferFeatureConfig&) noexcept;
    };

    struct GBufferFeatureDescriptor final
    {
        using Config = GBufferFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.GBufferFeature"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
