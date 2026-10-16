#pragma once

#include <array>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/data/Encoding.hpp>

namespace fixture
{
    struct Record final
    {
        std::uint32_t sequence = 0;
        std::array<std::int16_t, 3> positions{};
        float weight = 0;
        bool enabled = false;
        std::uint8_t mode = 1;

        bool operator==(const Record&) const noexcept = default;
    };

    struct Payload final
    {
        lux::cxx::SharedBytes<> blob;
        lux::render::ExternalDataRef external;
        lux::cxx::SharedBytes<> external_bytes;
    };

    inline unsigned record_decode_calls = 0;
    enum class ECodecFault { NONE, SIZE, SHORT_WRITE, LONG_WRITE, ENCODE_ERROR, SHORT_READ, DECODE_ERROR };
    inline ECodecFault codec_fault = ECodecFault::NONE;
}

namespace lux::render
{
    template <> struct DataSchema<fixture::Record> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr std::uint64_t modes[]{1, 3};
            static constexpr DataField fields[]{
                {"sequence", cxx::EScalarKind::U32},
                {"positions", cxx::EScalarKind::I16, 3},
                {"weight", cxx::EScalarKind::F32},
                {"enabled", cxx::EScalarKind::BOOL},
                {"mode", cxx::EScalarKind::U8, 1, EDataFieldKind::SCALAR, 1, modes}
            };
            static constexpr DataSchemaDescriptor schema{
                DataTypeIdView("test.g2.record"), {1, 0}, {1, 0}, fields, 16, 4
            };
            return schema;
        }

        static RenderResult<std::size_t> encodedSize(const fixture::Record&) noexcept
        {
            if (fixture::codec_fault == fixture::ECodecFault::SIZE) return 15;
            return 16;
        }

        static RenderResult<void> encode(const fixture::Record& value, cxx::BinarySpanWriter& writer) noexcept
        {
            if (fixture::codec_fault == fixture::ECodecFault::ENCODE_ERROR)
            {
                return cxx::unexpected(RenderError{kInvalidEncoding, {123}});
            }
            if (fixture::codec_fault == fixture::ECodecFault::SHORT_WRITE) return {};
            writer.writeUnsigned(value.sequence);
            for (auto position : value.positions) writer.writeSigned(position);
            writer.writeFloat(value.weight);
            writer.writeBool(value.enabled);
            writer.writeUnsigned(value.mode);
            if (fixture::codec_fault == fixture::ECodecFault::LONG_WRITE) writer.writeUnsigned<std::uint8_t>(0);
            return {};
        }

        static RenderResult<fixture::Record> decode(cxx::BinaryReader& reader, RetainableEncodedDataView) noexcept
        {
            ++fixture::record_decode_calls;
            fixture::Record value;
            if (fixture::codec_fault == fixture::ECodecFault::DECODE_ERROR)
            {
                return cxx::unexpected(RenderError{kInvalidEncoding, {456}});
            }
            if (fixture::codec_fault == fixture::ECodecFault::SHORT_READ) return value;
            reader.readUnsigned(value.sequence);
            for (auto& position : value.positions) reader.readSigned(position);
            reader.readFloat(value.weight);
            reader.readBool(value.enabled);
            reader.readUnsigned(value.mode);
            return value;
        }
    };

    template <> struct DataSchema<fixture::Payload> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{
                {"blob", cxx::EScalarKind::U8, 1, EDataFieldKind::BLOB, 4},
                {"external", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL, 4}
            };
            static constexpr DataSchemaDescriptor schema{
                DataTypeIdView("test.g2.payload"), {1, 0}, {1, 0}, fields, 36, 4
            };
            return schema;
        }

        static RenderResult<std::size_t> encodedSize(const fixture::Payload& value) noexcept
        {
            auto size = cxx::checkedAdd<std::size_t>(36, value.blob.size());
            if (!size) return cxx::unexpected(RenderError{kEncodingLimit});
            return *size;
        }

        static RenderResult<void> encode(const fixture::Payload& value, cxx::BinarySpanWriter& writer) noexcept
        {
            writer.writeUnsigned<std::uint64_t>(36);
            writer.writeUnsigned<std::uint64_t>(value.blob.size());
            writer.writeUnsigned(value.external.attachment_index);
            writer.writeUnsigned(value.external.offset);
            writer.writeUnsigned(value.external.size);
            writer.writeBytes(value.blob.view());
            return {};
        }

        static RenderResult<fixture::Payload> decode(cxx::BinaryReader& reader, RetainableEncodedDataView wire) noexcept
        {
            BlobRef blob;
            fixture::Payload value;
            reader.readUnsigned(blob.offset);
            reader.readUnsigned(blob.size);
            reader.readUnsigned(value.external.attachment_index);
            reader.readUnsigned(value.external.offset);
            reader.readUnsigned(value.external.size);
            auto owned_blob = resolveBlob(wire.bytes, blob, wire.bytes.size(), 4);
            if (!owned_blob) return cxx::unexpected(owned_blob.error());
            auto external = resolveExternal(wire.attachments, value.external, SIZE_MAX, 4);
            if (!external) return cxx::unexpected(external.error());
            value.blob = std::move(*owned_blob);
            value.external_bytes = std::move(*external);
            return value;
        }
    };
}
