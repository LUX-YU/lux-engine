#pragma once


#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

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
        RenderPayload source;
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
        RenderPayload source;
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





    struct TextureStorageConfig final
    {
        // Optional application residency budgets. Zero imposes no business limit; real device limits remain.
        std::uint32_t resident_versions_budget = 0;
        std::uint64_t resident_byte_budget = 0;
        bool operator==(const TextureStorageConfig&) const noexcept = default;
    };

    class TextureStorage final
    {
    public:
        using Config = TextureStorageConfig;
        static inline constexpr StorageDescriptor description{
            kTextureStorageType,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC), ConfigValue::typeOf<TextureBinding>()
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
                if (!writable_) return cxx::unexpected(RenderError{kStorageIdentity});
                return ops_->release(native_, id, revision);
            }

        private:
            friend class ResourceRegistry;
            Access(void* native, const Ops& ops, bool writable) noexcept
                : native_(native), ops_(&ops), writable_(writable) {}
            void* native_;
            const Ops* ops_;
            bool writable_;
        };
    };

}
