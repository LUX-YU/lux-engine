#pragma once
#include <lux/engine/function/render/graph/RGEnums.hpp>

namespace lux::render
{

    /// Scene-level pipeline configuration — controls HDR/LDR format decisions
    /// that affect multiple features uniformly.
    ///
    /// Owned by RenderScene; features query via `renderScene().pipelineConfig()`.
    struct PipelineConfig
    {
        /// Texture format for the intermediate lit-colour target.
        /// RGBA16_SFLOAT → HDR pipeline; RGBA8_UNORM → LDR pipeline.
        lux::common::ETextureFormat lit_color_format{lux::common::ETextureFormat::RGBA16_SFLOAT};

        /// True when lit_color_format is an 8-bit (LDR) format.
        /// LDR pipelines skip the intermediate "LitColor" buffer and write
        /// directly to "SceneColor", eliminating the need for a tonemap pass.
        [[nodiscard]] constexpr bool isLdr() const noexcept
        {
            switch (lit_color_format)
            {
            case lux::common::ETextureFormat::RGBA8_UNORM:
            case lux::common::ETextureFormat::RGBA8_SRGB:
            case lux::common::ETextureFormat::BGRA8_UNORM:
            case lux::common::ETextureFormat::BGRA8_SRGB:
                return true;
            default:
                return false;
            }
        }
    };

} // namespace lux::render
