#pragma once

#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/data/Wire.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/features/Feature.hpp>

namespace lux::render
{
    struct TextureTag;
    using TextureId = cxx::SlotKey<TextureTag>; // Server session-local; not an asset hash or descriptor index.
    inline constexpr StorageTypeIdView kTextureStorageType("lux.render.TextureStorage");

    struct TexturePublished final
    {
        TextureId texture;
        std::uint64_t revision = 0;
    };

    struct TextureRegion final
    {
        std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
        std::uint64_t row_pitch = 0;
    };

    struct UploadTexture final
    {
        TextureId texture{}; // null creates; non-null replaces only the named current revision
        std::uint64_t expected_revision = 0;
        rdesc::TextureInfo info;
        ExternalDataRef source;
    };

    struct UploadTextureRegion final
    {
        TextureId texture;
        std::uint64_t expected_revision = 0;
        TextureRegion region;
        ExternalDataRef source;
    };

    template <> struct DataSchema<TexturePublished>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const TexturePublished&) noexcept { return 16; }
        [[nodiscard]] static RenderResult<void> encode(const TexturePublished&, cxx::BinarySpanWriter&) noexcept;
        [[nodiscard]] static RenderResult<TexturePublished> decode(cxx::BinaryReader&, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<TexturePublished> decode(cxx::BinaryReader&, WireView) noexcept;
    };

    template <> struct DataSchema<UploadTexture>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const UploadTexture&) noexcept { return 452; }
        [[nodiscard]] static RenderResult<void> encode(const UploadTexture&, cxx::BinarySpanWriter&) noexcept;
        [[nodiscard]] static RenderResult<UploadTexture> decode(cxx::BinaryReader&, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<UploadTexture> decode(cxx::BinaryReader&, WireView) noexcept;
    };

    template <> struct DataSchema<UploadTextureRegion>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const UploadTextureRegion&) noexcept { return 64; }
        [[nodiscard]] static RenderResult<void> encode(const UploadTextureRegion&, cxx::BinarySpanWriter&) noexcept;
        [[nodiscard]] static RenderResult<UploadTextureRegion> decode(cxx::BinaryReader&, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<UploadTextureRegion> decode(cxx::BinaryReader&, WireView) noexcept;
    };

    [[nodiscard]] const FeatureDefinition& textureFeatureDefinition() noexcept;
    [[nodiscard]] const OperationDescriptor& uploadTextureOperation() noexcept;
    [[nodiscard]] const OperationDescriptor& uploadTextureRegionOperation() noexcept;
    [[nodiscard]] const OperationDescriptor& releaseTextureOperation() noexcept;
}
