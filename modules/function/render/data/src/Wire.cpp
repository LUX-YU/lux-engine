#include <lux/engine/render/data/Wire.hpp>

#include <algorithm>
#include <bit>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    namespace
    {
        template <typename T>
        bool readUnsigned(cxx::BinaryReader& reader, const DataField& field) noexcept
        {
            T value = 0;
            if (!reader.readUnsigned(value)) return false;
            return field.enum_values.empty() || std::ranges::find(field.enum_values, value) != field.enum_values.end();
        }

        bool readScalar(cxx::BinaryReader& reader, const DataField& field) noexcept
        {
            using enum cxx::EScalarKind;
            switch (field.scalar)
            {
            case BOOL: { bool value = false; return reader.readBool(value); }
            case I8: case U8: return readUnsigned<std::uint8_t>(reader, field);
            case I16: case U16: return readUnsigned<std::uint16_t>(reader, field);
            case I32: case U32: return readUnsigned<std::uint32_t>(reader, field);
            case I64: case U64: return readUnsigned<std::uint64_t>(reader, field);
            case F32: { float value = 0; return reader.readFloat(value); }
            case F64: { double value = 0; return reader.readFloat(value); }
            default: return false;
            }
        }
    }

    RenderResult<OwnedWireBytes>
    resolveBlob(const OwnedWireBytes& owner, BlobRef ref, std::size_t limit, std::uint32_t alignment) noexcept
    {
        const bool is_missing_backing = owner.size() != 0 && owner.data() == nullptr;
        if (is_missing_backing) return cxx::unexpected(RenderError{kInvalidDataReference});
        if (!std::has_single_bit(alignment)) return cxx::unexpected(RenderError{kInvalidDataReference});
        const bool is_invalid_range = ref.offset > owner.size() || ref.offset % alignment != 0;
        if (is_invalid_range) return cxx::unexpected(RenderError{kInvalidDataReference, {ref.offset, ref.size}});
        if (ref.size > owner.size() - ref.offset)
        {
            return cxx::unexpected(RenderError{kInvalidDataReference, {ref.offset, ref.size}});
        }
        if (ref.size > limit) return cxx::unexpected(RenderError{kWireLimit, {ref.size, limit}});
        return owner.subspan(static_cast<std::size_t>(ref.offset), static_cast<std::size_t>(ref.size));
    }

    RenderResult<OwnedWireBytes> resolveExternal(
        std::span<const OwnedWireBytes> owners,
        ExternalDataRef ref,
        std::size_t limit,
        std::uint32_t alignment
    ) noexcept
    {
        if (ref.attachment_index >= owners.size())
        {
            return cxx::unexpected(RenderError{kInvalidDataReference, {ref.attachment_index, owners.size()}});
        }
        return resolveBlob(owners[ref.attachment_index], {ref.offset, ref.size}, limit, alignment);
    }

    RenderResult<void> validateWire(WireView wire, WireLimits limits) noexcept
    {
        if (auto result = validateDataSchema(wire.schema); !result) return result;
        const bool is_missing_backing = wire.bytes.size() != 0 && wire.bytes.data() == nullptr;
        if (is_missing_backing) return cxx::unexpected(RenderError{kInvalidWire});
        if (wire.bytes.size() > limits.max_wire_bytes)
        {
            return cxx::unexpected(RenderError{kWireLimit, {wire.bytes.size(), limits.max_wire_bytes}});
        }
        if (wire.schema.fixed_size > wire.bytes.size()) return cxx::unexpected(RenderError{kInvalidWire});
        const auto bytes = wire.bytes.view();
        cxx::BinaryReader reader(bytes.first(static_cast<std::size_t>(wire.schema.fixed_size)), limits.max_wire_bytes);
        std::uint64_t tail = wire.schema.fixed_size;
        for (const auto& field : wire.schema.fields)
        {
            for (std::uint32_t element = 0; element < field.count; ++element)
            {
                if (field.kind == EDataFieldKind::SCALAR)
                {
                    if (!readScalar(reader, field))
                    {
                        return cxx::unexpected(RenderError{kInvalidWire, {reader.offset()}});
                    }
                    continue;
                }

                ExternalDataRef ref;
                if (field.kind == EDataFieldKind::EXTERNAL)
                {
                    static_cast<void>(reader.readUnsigned(ref.attachment_index));
                }
                const bool has_range = reader.readUnsigned(ref.offset) && reader.readUnsigned(ref.size);
                if (!has_range) return cxx::unexpected(RenderError{kInvalidWire, {reader.offset()}});
                if (field.kind == EDataFieldKind::EXTERNAL)
                {
                    auto result = resolveExternal(
                        wire.attachments, ref, limits.max_reference_bytes, field.reference_alignment
                    );
                    if (!result) return cxx::unexpected(result.error());
                    continue;
                }

                // Inline blobs occupy the tail in field order, with zero alignment
                // padding only. This accounts for every byte and rejects hidden tails.
                auto aligned = cxx::alignUpChecked(tail, static_cast<std::uint64_t>(field.reference_alignment));
                if (!aligned) return cxx::unexpected(RenderError{kInvalidDataReference});
                if (ref.offset != *aligned)
                {
                    return cxx::unexpected(RenderError{kInvalidDataReference, {ref.offset, *aligned}});
                }
                auto result = resolveBlob(
                    wire.bytes, {ref.offset, ref.size}, limits.max_reference_bytes, field.reference_alignment
                );
                if (!result) return cxx::unexpected(result.error());
                for (auto padding = tail; padding < ref.offset; ++padding)
                {
                    if (bytes[static_cast<std::size_t>(padding)] != std::byte{0})
                    {
                        return cxx::unexpected(RenderError{kInvalidWire, {padding}});
                    }
                }
                tail = ref.offset + ref.size; // resolveBlob proved this sum is within the owner.
            }
        }
        const bool is_incomplete = !reader.requireFullyConsumed() || tail != bytes.size();
        if (is_incomplete) return cxx::unexpected(RenderError{kInvalidWire, {tail, bytes.size()}});
        return {};
    }
}
