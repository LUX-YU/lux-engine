#pragma once

#include <lux/engine/render/inputs/DrawInputs.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <variant>
#include <vector>

namespace lux::render
{
    inline constexpr std::string_view kUiSceneCapability = "lux.render.capability.ui_scene";

    struct UiConfiguration final
    {
        EIndexType index_type = EIndexType::UINT16;
        std::array<float, 4> clear{0.015F, 0.015F, 0.015F, 1}; // Linear BT.709; alpha is coverage.
        bool operator==(const UiConfiguration&) const noexcept = default;
    };

    struct UiSampledImage final
    {
        TextureId texture;
        ImageShape description;
        bool operator==(const UiSampledImage&) const noexcept = default;
    };

    // ImageShape alone declares a sampled Target image, supplied at Attach under uiImageInput(index).
    using VUiSceneImage = std::variant<UiSampledImage, ImageShape>;

    struct UiSceneInputs final
    {
        std::vector<VUiSceneImage> images;
    };

    struct UiDrawParameters final
    {
        std::array<float, 2> scale, translation;
        std::uint32_t source_index = 0;
    };

    [[nodiscard]] inline std::string uiImageInput(std::uint32_t index)
    {
        return "image" + std::to_string(index);
    }

    template <> struct ConfigValueTraits<UiSceneInputs> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const UiSceneInputs&) noexcept;
    };
} // namespace lux::render
