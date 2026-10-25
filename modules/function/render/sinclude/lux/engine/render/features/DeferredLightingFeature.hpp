#pragma once

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/features/ClusteredLightInputs.hpp>
#include <lux/engine/render/features/ShadowInputs.hpp>

namespace lux::render
{
    struct DeferredLightingParameters final
    {
        std::array<float, 16> inverse_view_projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::array<float, 4> camera_position{0, 0, 1, 0};
        std::array<float, 4> viewport{0, 0, 1, 1}; // Normalized rectangle, matching the RasterViewport.
        std::array<float, 4> background{0, 0, 0, 1};
        std::array<float, 4> environment{}; // Explicit diffuse environment only; zero means no ambient/IBL.
    };

    static_assert(sizeof(DeferredLightingParameters) == 128);

    [[nodiscard]] inline rdesc::ShaderBlock deferredLightingParameterBlock() noexcept
    {
        rdesc::ShaderBlock block{sizeof(DeferredLightingParameters), 16};
        block.fields = {
            {"inverse_view_projection", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"camera_position", rdesc::EDataScalar::FLOAT, 64, 4, 1, 0, 0},
            {"viewport", rdesc::EDataScalar::FLOAT, 80, 4, 1, 0, 0},
            {"background", rdesc::EDataScalar::FLOAT, 96, 4, 1, 0, 0},
            {"environment", rdesc::EDataScalar::FLOAT, 112, 4, 1, 0, 0}
        };
        return block;
    }

    struct DeferredLightingFeatureConfig final
    {
        GraphImageDesc extent{1, 1, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        std::string lights = "direct_lights";
        rdesc::ShaderBlock input_layout = deferredLightingParameterBlock();
        std::optional<ShadowMapShape> shadows;
        std::optional<ClusterGrid> clusters;
        bool operator==(const DeferredLightingFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<DeferredLightingFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const DeferredLightingFeatureConfig&) noexcept;
    };

} // namespace lux::render
