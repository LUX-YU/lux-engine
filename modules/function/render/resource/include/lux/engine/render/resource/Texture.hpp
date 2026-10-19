#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resource/Storage.hpp>
#include <span>

namespace lux::render
{
    class ResourceRegistry;
    struct TextureTag;
    using TextureId = StorageResourceId<TextureTag>;
    inline constexpr StorageNameView kTextureStorageType("lux.render.TextureStorage");

    struct TexturePublished final
    {
        TextureId texture;
        std::uint64_t revision = 0;
    };

    struct TextureBinding final
    {
        TextureId texture;
        std::uint32_t lod = 0;
        bool operator==(const TextureBinding&) const noexcept = default;
    };

    struct TextureRegion final
    {
        std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
        std::uint64_t row_pitch = 0;
    };

    struct TextureWrite final
    {
        TextureId texture{}; // valid StorageId + null local key creates; otherwise replace that qualified resource
        std::uint64_t expected_revision = 0;
        rdesc::TextureInfo info;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: Host must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct SetTexture final
    {
        static constexpr std::string_view name = "lux.render.SetTexture";
        // Success certifies Copy, required Acquire and publication; the generic protocol has no GPU category.
        using Request = TextureWrite;
        using Response = RenderResult<TexturePublished>;
    };

    struct TextureRegionWrite final
    {
        TextureId texture;
        std::uint64_t expected_revision = 0;
        TextureRegion region;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: Host must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct SetTextureRegion final
    {
        static constexpr std::string_view name = "lux.render.SetTextureRegion";
        // Success certifies Copy, required Acquire and publication; the generic protocol has no GPU category.
        using Request = TextureRegionWrite;
        using Response = RenderResult<TexturePublished>;
    };

    struct ReleaseTexture final
    {
        static constexpr std::string_view name = "lux.render.ReleaseTexture";
        using Request = TexturePublished;
        using Response = RenderResult<Ack>;
    };

    struct QueryTexture final
    {
        static constexpr std::string_view name = "lux.render.QueryTexture";
        using Request = TextureId;
        using Response = RenderResult<TexturePublished>;
    };

    class TextureStorage final
    {
    public:
        using Config = void;
        static inline constexpr StorageDescriptor description{
            kTextureStorageType,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC),
            cppType<TextureBinding>()
        };

        struct Ops final
        {
            RenderResult<TexturePublished> (*query)(const void*, TextureId) noexcept;
            RenderResult<void> (*release)(void*, TextureId, std::uint64_t) noexcept;
        };

        class Access final
        {
        public:
            [[nodiscard]] RenderResult<TexturePublished> query(TextureId id) const noexcept
            {
                return ops_->query(native_, id);
            }

            [[nodiscard]] RenderResult<void> release(TextureId id, std::uint64_t revision) const noexcept
            {
                return ops_->release(native_, id, revision);
            }

        private:
            friend class ResourceRegistry;

            Access(void* native, const Ops& ops) noexcept : native_(native), ops_(&ops) {}

            void* native_;
            const Ops* ops_;
        };
    };

} // namespace lux::render
