#include <lux/engine/render/features/Texture.hpp>

namespace lux::render
{
    namespace
    {
        void writeIdentity(cxx::BinarySpanWriter& out, TextureId id, std::uint64_t revision) noexcept
        {
            out.writeUnsigned(id.index); out.writeUnsigned(id.gen); out.writeUnsigned(revision);
        }

        void readIdentity(cxx::BinaryReader& in, TextureId& id, std::uint64_t& revision) noexcept
        {
            in.readUnsigned(id.index); in.readUnsigned(id.gen); in.readUnsigned(revision);
        }

        void writeSource(cxx::BinarySpanWriter& out, ExternalDataRef source) noexcept
        {
            out.writeUnsigned(source.attachment_index); out.writeUnsigned(source.offset); out.writeUnsigned(source.size);
        }

        void readSource(cxx::BinaryReader& in, ExternalDataRef& source) noexcept
        {
            in.readUnsigned(source.attachment_index); in.readUnsigned(source.offset); in.readUnsigned(source.size);
        }
    }

    const DataSchemaDescriptor& DataSchema<TexturePublished>::describe() noexcept
    {
        static constexpr DataField fields[]{{"texture", cxx::EScalarKind::U32, 2}, {"revision", cxx::EScalarKind::U64}};
        static const DataSchemaDescriptor schema{DataTypeIdView("lux.render.TexturePublished"), {1, 0}, {1, 0}, fields, 16, 4};
        return schema;
    }

    RenderResult<void> DataSchema<TexturePublished>::encode(const TexturePublished& value, cxx::BinarySpanWriter& out) noexcept
    {
        writeIdentity(out, value.texture, value.revision);
        return {};
    }

    RenderResult<TexturePublished> DataSchema<TexturePublished>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        TexturePublished value;
        readIdentity(in, value.texture, value.revision);
        return value;
    }

    RenderResult<TexturePublished> DataSchema<TexturePublished>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<UploadTexture>::describe() noexcept
    {
        static constexpr std::uint64_t colors[]{0, 1, 2, 3};
        static constexpr DataField fields[]{
            {"texture", cxx::EScalarKind::U32, 2}, {"expected_revision", cxx::EScalarKind::U64},
            {"extent_channels", cxx::EScalarKind::I32, 3}, {"layers_mips_format", cxx::EScalarKind::U32, 3},
            {"color_space", cxx::EScalarKind::U32, 1, EDataFieldKind::SCALAR, 1, colors},
            {"flags", cxx::EScalarKind::U32}, {"mip_offsets", cxx::EScalarKind::U64, 16},
            {"mip_sizes", cxx::EScalarKind::U64, 16}, {"mip_widths", cxx::EScalarKind::U32, 16},
            {"mip_heights", cxx::EScalarKind::U32, 16},
            {"source", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}
        };
        static const DataSchemaDescriptor schema{DataTypeIdView("lux.render.UploadTexture"), {1, 0}, {1, 0}, fields, 452, 4};
        return schema;
    }

    RenderResult<void> DataSchema<UploadTexture>::encode(const UploadTexture& value, cxx::BinarySpanWriter& out) noexcept
    {
        writeIdentity(out, value.texture, value.expected_revision);
        const auto& info = value.info;
        out.writeSigned(info.width); out.writeSigned(info.height); out.writeSigned(info.channel);
        out.writeUnsigned(info.layers); out.writeUnsigned(info.mip_count);
        out.writeUnsigned(std::uint32_t(info.pixel_format)); out.writeUnsigned(std::uint32_t(info.color_space));
        out.writeUnsigned(info.flags);
        for (const auto& mip : info.mip_ranges) out.writeUnsigned(mip.offset);
        for (const auto& mip : info.mip_ranges) out.writeUnsigned(mip.size);
        for (const auto& mip : info.mip_ranges) out.writeUnsigned(mip.width);
        for (const auto& mip : info.mip_ranges) out.writeUnsigned(mip.height);
        writeSource(out, value.source);
        return {};
    }

    RenderResult<UploadTexture> DataSchema<UploadTexture>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        UploadTexture value;
        readIdentity(in, value.texture, value.expected_revision);
        auto& info = value.info;
        in.readSigned(info.width); in.readSigned(info.height); in.readSigned(info.channel);
        in.readUnsigned(info.layers); in.readUnsigned(info.mip_count);
        std::uint32_t format = 0, color = 0;
        in.readUnsigned(format); in.readUnsigned(color); in.readUnsigned(info.flags);
        info.pixel_format = static_cast<rdesc::ETexturePixelFormat>(format);
        info.color_space = static_cast<rdesc::ETextureColorSpace>(color);
        for (auto& mip : info.mip_ranges) in.readUnsigned(mip.offset);
        for (auto& mip : info.mip_ranges) in.readUnsigned(mip.size);
        for (auto& mip : info.mip_ranges) in.readUnsigned(mip.width);
        for (auto& mip : info.mip_ranges) in.readUnsigned(mip.height);
        readSource(in, value.source);
        return value;
    }

    RenderResult<UploadTexture> DataSchema<UploadTexture>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<UploadTextureRegion>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"texture", cxx::EScalarKind::U32, 2}, {"expected_revision", cxx::EScalarKind::U64},
            {"region", cxx::EScalarKind::U32, 5}, {"row_pitch", cxx::EScalarKind::U64},
            {"source", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}
        };
        static const DataSchemaDescriptor schema{DataTypeIdView("lux.render.UploadTextureRegion"), {1, 0}, {1, 0}, fields, 64, 4};
        return schema;
    }

    RenderResult<void> DataSchema<UploadTextureRegion>::encode(const UploadTextureRegion& value, cxx::BinarySpanWriter& out) noexcept
    {
        writeIdentity(out, value.texture, value.expected_revision);
        const auto& r = value.region;
        out.writeUnsigned(r.mip); out.writeUnsigned(r.x); out.writeUnsigned(r.y);
        out.writeUnsigned(r.width); out.writeUnsigned(r.height); out.writeUnsigned(r.row_pitch);
        writeSource(out, value.source);
        return {};
    }

    RenderResult<UploadTextureRegion> DataSchema<UploadTextureRegion>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        UploadTextureRegion value;
        readIdentity(in, value.texture, value.expected_revision);
        auto& r = value.region;
        in.readUnsigned(r.mip); in.readUnsigned(r.x); in.readUnsigned(r.y);
        in.readUnsigned(r.width); in.readUnsigned(r.height); in.readUnsigned(r.row_pitch);
        readSource(in, value.source);
        return value;
    }

    RenderResult<UploadTextureRegion> DataSchema<UploadTextureRegion>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const OperationDescriptor& uploadTextureOperation() noexcept
    {
        static const auto value = operationDescriptor<UploadTexture, TexturePublished>(
            "lux.render.UploadTexture", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);
        return value;
    }

    const OperationDescriptor& uploadTextureRegionOperation() noexcept
    {
        static const auto value = operationDescriptor<UploadTextureRegion, TexturePublished>(
            "lux.render.UploadTextureRegion", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);
        return value;
    }

    const OperationDescriptor& releaseTextureOperation() noexcept
    {
        static const auto value = operationDescriptor<TexturePublished>("lux.render.ReleaseTexture");
        return value;
    }

    const FeatureDefinition& textureFeatureDefinition() noexcept
    {
        static const std::array operations{uploadTextureOperation(), uploadTextureRegionOperation(), releaseTextureOperation()};
        static const FeatureDefinition definition{FeatureTypeIdView("lux.render.Texture"), {1, 0}, nullptr,
            [](const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> { return {}; },
            nullptr, operations};
        return definition;
    }
}
