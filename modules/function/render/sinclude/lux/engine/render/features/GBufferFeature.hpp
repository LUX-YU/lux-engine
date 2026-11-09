#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{
    struct GBufferFeatureConfig final
    {
        EffectOutputDesc output;
        // Composition can provide one larger typed input block; only view/projection are consumed here.
        rdesc::ShaderBlock input_layout = meshViewParameterBlock();
        bool front_counter_clockwise = true;
        bool operator==(const GBufferFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<GBufferFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const GBufferFeatureConfig&) noexcept;
    };

} // namespace lux::render
