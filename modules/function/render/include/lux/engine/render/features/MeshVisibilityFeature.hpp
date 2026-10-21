#pragma once

#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{
    struct MeshVisibilityFeatureConfig final
    {
        std::vector<MeshInstanceDrawGroup> groups;
        std::string instance_storage = "instances", mesh_storage = "meshes";
        std::uint32_t view_count = 1, candidate_capacity = 1024;
        bool frustum_culling = true;
        bool operator==(const MeshVisibilityFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshVisibilityFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshVisibilityFeatureConfig&) noexcept;
    };

    struct MeshVisibilityFeatureDescriptor final
    {
        using Config = MeshVisibilityFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.MeshVisibilityFeature"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
