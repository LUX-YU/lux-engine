#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/capabilities/UiScene.hpp>

#include <lux/engine/description/UiImageToken.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <lux/engine/render/extensions/resource/SampledTexture2D.hpp>

namespace lux::render
{
    struct UiRasterFeatureConfig final
    {
        EffectOutputDesc output;
        EIndexType index_type = EIndexType::UINT16;
        // Linear-light BT.709 backdrop. Vertex colors are encoded sRGB; alpha is coverage.
        std::array<float, 4> clear{0.015f, 0.015f, 0.015f, 1};
        bool operator==(const UiRasterFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<UiRasterFeatureConfig> final
    {
        [[nodiscard]] static LUX_RENDER_PUBLIC RenderResult<std::size_t>
        retainedBytes(const UiRasterFeatureConfig&) noexcept;
    };

} // namespace lux::render
