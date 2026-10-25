#pragma once

#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/features/ClusteredLightInputs.hpp>

namespace lux::render
{
    struct ClusteredLightingFeatureConfig final
    {
        ClusterGrid grid;
        rdesc::ShaderBlock input_layout = clusteredLightingParameterBlock();
        std::string lights = "direct_lights";
        // Explicit port source, e.g. the same light snapshot feeding shadow projection and shading.
        bool external_lights = false;
        bool operator==(const ClusteredLightingFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<ClusteredLightingFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const ClusteredLightingFeatureConfig&) noexcept;
    };

} // namespace lux::render
