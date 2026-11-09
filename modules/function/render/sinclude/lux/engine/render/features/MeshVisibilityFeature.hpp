#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <optional>

namespace lux::render
{
    struct MeshVisibilityFeatureConfig final
    {
        std::string instance_storage = "instances";
        std::uint32_t view_count = 1;
        bool frustum_culling = true;
        bool shadow_pass = false;
        // When present, build one view from the Attachment's view/projection fields on the GPU.
        // Otherwise the composition must connect the geometric views input (e.g. shadow slices).
        std::optional<rdesc::ShaderBlock> view_parameters;
        bool operator==(const MeshVisibilityFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshVisibilityFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshVisibilityFeatureConfig&) noexcept;
    };

} // namespace lux::render
