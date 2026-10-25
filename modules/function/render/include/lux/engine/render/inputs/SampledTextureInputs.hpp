#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/ResourceId.hpp>
#include <span>

namespace lux::render
{
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

    struct TextureWrite final
    {
        CapabilityInput destination;
        rdesc::TextureInfo info;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: the application must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreateTexture final
    {
        static constexpr std::string_view name = "lux.render.CreateTexture";
        static constexpr bool frame_scoped = false;
        // Success certifies Copy, required Acquire and publication; the generic protocol has no GPU category.
        using Request = TextureWrite;

        struct Response final
        {
            RenderError error{};
            TextureId texture{};
        };
    };

    struct TextureRegionWrite final
    {
        TextureId texture; // Source remains valid through Response; the result is a new identity.
        TextureRegion region;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: the application must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreatePatchedTexture final
    {
        static constexpr std::string_view name = "lux.render.CreatePatchedTexture";
        static constexpr bool frame_scoped = false;
        // Success certifies Copy, required Acquire and publication; the generic protocol has no GPU category.
        using Request = TextureRegionWrite;

        struct Response final
        {
            RenderError error{};
            TextureId texture{};
        };
    };

    struct ReleaseTexture final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "lux.render.ReleaseTexture";
        static constexpr bool frame_scoped = true;
        // Client ends all Material/Attachment references first. Release does not update other Storage.
        // Success revokes the identity and hands accepted GPU use to Session cleanup, without a GPU wait.
        using Request = TextureId;
        using Response = AckResponse;
    };

} // namespace lux::render
