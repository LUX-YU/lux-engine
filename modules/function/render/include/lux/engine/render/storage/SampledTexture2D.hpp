#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>
#include <span>

namespace lux::render
{
    class ResourceRegistry;
    struct TextureTag;
    using TextureId = StorageResourceId<TextureTag>;
    inline constexpr StorageNameView kTextureStorageType("lux.render.storage.SampledTexture2D");

    struct TextureGpuRef final
    {
        std::uint32_t descriptor_index = 0;
        std::uint32_t color_space = 0;
    };

    static_assert(sizeof(TextureGpuRef) == 8);

    struct TextureBinding final
    {
        TextureId texture;
        bool operator==(const TextureBinding&) const noexcept = default;
    };

    struct TextureRegion final
    {
        std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
        std::uint64_t row_pitch = 0;
    };

    struct TextureWrite final
    {
        StorageId storage;
        rdesc::TextureInfo info;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: Host must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreateTexture final
    {
        static constexpr std::string_view name = "lux.render.CreateTexture";
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
        // Abandon/connection loss is not completion: Host must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreatePatchedTexture final
    {
        static constexpr std::string_view name = "lux.render.CreatePatchedTexture";
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
        static constexpr std::string_view name = "lux.render.ReleaseTexture";
        // Client ends all Material/Attachment references first. Release does not update other Storage.
        // Success revokes the identity and hands accepted GPU use to Session cleanup, without a GPU wait.
        using Request = TextureId;
        using Response = AckResponse;
    };

    class SampledTexture2DStorageDescriptor final
    {
    public:
        using Config = void;
        static inline constexpr StorageDescriptor descriptor{
            kTextureStorageType,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC),
            cppType<TextureBinding>(),
            "Single-layer sampled 2D images with mip, BC1, region uploads and a white default."
        };

        using Ops = void;
    };
} // namespace lux::render
