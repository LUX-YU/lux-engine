#pragma once

#include <lux/engine/render/data/Encoding.hpp>

#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <tuple>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    class ResourceRegistry;
    struct TextureTag;
    using TextureId = cxx::SlotKey<TextureTag>; // Server session-local; not an asset hash or descriptor index.
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
    };

    template <> struct DataSchema<TextureBinding>
    {
        static inline constexpr DataField fields[]{
            {"index", cxx::EScalarKind::U32}, {"generation", cxx::EScalarKind::U32}, {"lod", cxx::EScalarKind::U32}
        };
        static inline constexpr DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.TextureBinding"), {1, 0}, {1, 0}, fields, 12, 4
        };
        static const DataSchemaDescriptor& describe() noexcept { return schema; }
        static RenderResult<std::size_t> encodedSize(const TextureBinding&) noexcept { return 12; }
        static RenderResult<void> encode(const TextureBinding& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.texture.index);
            out.writeUnsigned(value.texture.gen);
            out.writeUnsigned(value.lod);
            return {};
        }
        static RenderResult<TextureBinding> decode(cxx::BinaryReader& in, EncodedDataView) noexcept
        {
            TextureBinding value;
            in.readUnsigned(value.texture.index);
            in.readUnsigned(value.texture.gen);
            in.readUnsigned(value.lod);
            return value;
        }
        static RenderResult<TextureBinding> decode(cxx::BinaryReader& in, RetainableEncodedDataView wire) noexcept
        {
            return decode(in, EncodedDataView{wire.schema, wire.bytes.view(), {}});
        }
    };

    struct TextureRegion final
    {
        std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
        std::uint64_t row_pitch = 0;
    };

    struct TextureWrite final
    {
        TextureId texture{}; // null creates; otherwise replace the named current revision
        std::uint64_t expected_revision = 0;
        rdesc::TextureInfo info;
        RenderPayload source;
    };

    struct SetTexture final
    {
        static constexpr std::string_view name = "lux.render.SetTexture";
        // Success certifies Copy, required Acquire and publication; the generic protocol has no GPU category.
        using Request = TextureWrite;
        using Response = TexturePublished;
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
        using Response = TexturePublished;
    };

    struct ReleaseTexture final
    {
        static constexpr std::string_view name = "lux.render.ReleaseTexture";
        using Request = TexturePublished;
        using Response = Ack;
    };

    struct QueryTexture final
    {
        static constexpr std::string_view name = "lux.render.QueryTexture";
        using Request = TextureId;
        using Response = TexturePublished;
    };

    template <> struct CommandValueTraits<SetTexture::Request> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const SetTexture::Request&) noexcept;
    };

    template <> struct CommandValueTraits<SetTextureRegion::Request> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const SetTextureRegion::Request&) noexcept;
    };

    struct TextureStorageConfig final
    {
        std::uint32_t capacity = 128;
        std::uint64_t byte_limit = 256 * 1024 * 1024;
        bool operator==(const TextureStorageConfig&) const noexcept = default;
    };

    class TextureStorage final
    {
    public:
        using Config = TextureStorageConfig;
        static inline constexpr DataField config_fields[]{
            {"capacity", cxx::EScalarKind::U32}, {"byte_limit", cxx::EScalarKind::U64}
        };
        static inline constexpr DataSchemaDescriptor config_schema{
            DataTypeIdView("lux.render.TextureStorage.Config"), {1, 0}, {1, 0}, config_fields, 12, 4
        };
        static inline constexpr ShaderBinding bindings[]{{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}};
        static inline constexpr StorageDescriptor description{
            kTextureStorageType, {1, 0}, scopeMask(EResourceScope::GLOBAL), &config_schema,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC), bindings, &DataSchema<TextureBinding>::schema
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
                if (!writable_) return cxx::unexpected(RenderError{kStorageDomain});
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
