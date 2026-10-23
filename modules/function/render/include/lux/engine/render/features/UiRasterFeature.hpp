#pragma once

#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/graph/Draw.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>

namespace lux::render
{
    struct UiTextureImage final
    {
        TextureId texture;
        GraphImageDesc description;
        std::string storage_key = "assets";
        bool operator==(const UiTextureImage&) const noexcept = default;
    };

    // The caller connects this image port to an imported Target image at Attach.
    struct UiTargetImage final
    {
        GraphImageDesc description;
        bool operator==(const UiTargetImage&) const noexcept = default;
    };

    using VUiImage = std::variant<UiTextureImage, UiTargetImage>;

    struct UiRasterFeatureConfig final
    {
        TargetDesc target;
        EIndexType index_type = EIndexType::UINT16;
        std::vector<VUiImage> images;
        // Linear-light BT.709 backdrop. Vertex colors are encoded sRGB; alpha is coverage.
        std::array<float, 4> clear{0.015f, 0.015f, 0.015f, 1};
        bool operator==(const UiRasterFeatureConfig&) const noexcept = default;
    };

    // Exact push-constant layout of the built-in UI shader. Capture supplies scale/translation;
    // the recorder writes source_index from IndexedDrawRange::selection, never a raw Descriptor index.
    struct UiRasterParameters final
    {
        std::array<float, 2> scale, translation;
        std::uint32_t source_index = 0;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC std::string uiImagePort(std::uint32_t index);

    template <> struct ConfigValueTraits<UiRasterFeatureConfig> final
    {
        [[nodiscard]] static LUX_RENDER_PUBLIC RenderResult<std::size_t>
        retainedBytes(const UiRasterFeatureConfig&) noexcept;
    };

    struct UiRasterFeatureDescriptor final
    {
        using Config = UiRasterFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.UiRasterFeature"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
