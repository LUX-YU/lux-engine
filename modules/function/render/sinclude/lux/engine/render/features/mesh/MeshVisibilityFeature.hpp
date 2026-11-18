#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>

namespace lux::render
{
    struct MeshVisibilityFeatureConfig final
    {
        std::string instance_storage = "lux.render.resource.instances";
        std::uint32_t view_count = 1;
        bool frustum_culling = true;
        bool shadow_pass = false;
        // The main View supplies the camera. Shadow projections connect their own views port.
        bool camera_view = false;
        bool operator==(const MeshVisibilityFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshVisibilityFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshVisibilityFeatureConfig&) noexcept;
    };

} // namespace lux::render
