#pragma once

#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/features/ShadowInputs.hpp>

namespace lux::render
{
    struct MeshShadowFeatureConfig final
    {
        ShadowMapShape shape;
        std::vector<RasterMeshGroup> groups;
        std::uint32_t candidate_capacity = 1024;
        bool operator==(const MeshShadowFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshShadowFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshShadowFeatureConfig&) noexcept;
    };

    struct MeshShadowFeatureDescriptor final
    {
        using Config = MeshShadowFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.MeshShadowFeature"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
