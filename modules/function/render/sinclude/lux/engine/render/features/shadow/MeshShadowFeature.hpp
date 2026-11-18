#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>

namespace lux::render
{
    struct MeshShadowFeatureConfig final
    {
        ShadowMapShape shape;
        bool operator==(const MeshShadowFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshShadowFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshShadowFeatureConfig&) noexcept;
    };

} // namespace lux::render
