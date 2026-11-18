#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightInputs.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>

namespace lux::render
{
    // The consumer needs only the encoded buffer extents, not the producer's
    // cluster partition algorithm or its spatial grid configuration.
    struct ClusterLightListShape final
    {
        std::uint32_t cluster_count = 0, index_capacity = 0;
        bool operator==(const ClusterLightListShape&) const noexcept = default;
    };

    struct DeferredLightingFeatureConfig final
    {
        GraphImageDesc extent{1, 1, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        std::string lights = "lux.render.resource.lights";
        std::optional<ShadowMapShape> shadows;
        std::optional<ClusterLightListShape> clusters;
        bool external_lights = false;
        std::array<float, 4> environment{};
        bool operator==(const DeferredLightingFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<DeferredLightingFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const DeferredLightingFeatureConfig&) noexcept;
    };

} // namespace lux::render
