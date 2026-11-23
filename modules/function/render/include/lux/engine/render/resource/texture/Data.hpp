#pragma once

#include <string_view>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <span>

namespace lux::render
{
    inline constexpr std::string_view kSampledTextureInput = "lux.render.input.sampled_texture_2d";

    class ResourceRegistry;
    struct TextureTag;
    using TextureId = ResourceId<TextureTag>;

    struct TextureGpuRef final
    {
        std::uint32_t image_index = 0;
        std::uint32_t sampler_index = 0;
        std::uint32_t color_space = 0;
        std::uint32_t flags = rdesc::kTextureDefaultWhite;
    };

    static_assert(sizeof(TextureGpuRef) == 16);

    struct TextureRegion final
    {
        std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
        std::uint64_t row_pitch = 0;
    };

    struct TextureData final
    {
        rdesc::TextureInfo info;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: the application must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct TexturePatchData final
    {
        TextureId texture; // Source remains valid through Response; the result is a new identity.
        TextureRegion region;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: the application must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

} // namespace lux::render
