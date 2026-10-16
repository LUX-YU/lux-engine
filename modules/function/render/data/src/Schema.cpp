#include <lux/engine/render/data/Schema.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    namespace
    {
        std::uint64_t scalarSize(cxx::EScalarKind kind) noexcept
        {
            using enum cxx::EScalarKind;
            switch (kind)
            {
            case BOOL: case I8: case U8: return 1;
            case I16: case U16: return 2;
            case I32: case U32: case F32: return 4;
            case I64: case U64: case F64: return 8;
            default: return 0;
            }
        }
    }

    RenderResult<void> validateDataSchema(const DataSchemaDescriptor& schema) noexcept
    {
        const bool is_invalid_identity = !schema.type.isValid() || schema.schema_version.major == 0 ||
            schema.encoding_version.major == 0;
        const bool is_invalid_layout = schema.fields.empty() || schema.fixed_size == 0 ||
            !std::has_single_bit(schema.alignment);
        if (is_invalid_identity || is_invalid_layout) return cxx::unexpected(RenderError{kInvalidDataSchema});
        if (schema.fixed_size % schema.alignment != 0) return cxx::unexpected(RenderError{kInvalidDataSchema});

        std::uint64_t size = 0;
        for (std::size_t index = 0; index < schema.fields.size(); ++index)
        {
            const auto& field = schema.fields[index];
            const bool is_invalid_field = field.name.empty() || field.count == 0 ||
                !std::has_single_bit(field.reference_alignment);
            if (is_invalid_field) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (schema.fields[previous].name == field.name)
                {
                    return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
                }
            }

            std::uint64_t width = 0;
            if (field.kind == EDataFieldKind::SCALAR)
            {
                width = scalarSize(field.scalar);
                if (field.reference_alignment != 1) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
            }
            else
            {
                const bool is_invalid_reference = field.scalar != cxx::EScalarKind::U8 || !field.enum_values.empty();
                if (is_invalid_reference) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
                if (field.kind == EDataFieldKind::BLOB) width = 16;
                if (field.kind == EDataFieldKind::EXTERNAL) width = 20;
            }
            if (width == 0) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});

            if (!field.enum_values.empty())
            {
                const bool is_unsigned_scalar = field.scalar == cxx::EScalarKind::U8 ||
                    field.scalar == cxx::EScalarKind::U16 || field.scalar == cxx::EScalarKind::U32 ||
                    field.scalar == cxx::EScalarKind::U64;
                if (!is_unsigned_scalar) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
                const auto maximum = width == 8 ? UINT64_MAX : (std::uint64_t{1} << (width * 8)) - 1;
                for (std::size_t n = 0; n < field.enum_values.size(); ++n)
                {
                    if (field.enum_values[n] > maximum)
                    {
                        return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
                    }
                    for (std::size_t p = 0; p < n; ++p)
                    {
                        if (field.enum_values[n] == field.enum_values[p])
                        {
                            return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
                        }
                    }
                }
            }
            auto field_size = cxx::checkedMul<std::uint64_t>(width, field.count);
            if (!field_size) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
            auto next = cxx::checkedAdd(size, *field_size);
            if (!next) return cxx::unexpected(RenderError{kInvalidDataSchema, {index}});
            size = *next;
        }
        if (size != schema.fixed_size)
        {
            return cxx::unexpected(RenderError{kInvalidDataSchema, {size, schema.fixed_size}});
        }
        return {};
    }

    RenderResult<void>
    compatibleDataSchemas(const DataSchemaDescriptor& expected, const DataSchemaDescriptor& actual) noexcept
    {
        if (auto result = validateDataSchema(expected); !result) return result;
        if (auto result = validateDataSchema(actual); !result) return result;
        const bool is_identity_mismatch = expected.type != actual.type ||
            expected.schema_version != actual.schema_version || expected.encoding_version != actual.encoding_version;
        const bool is_layout_mismatch = expected.fixed_size != actual.fixed_size ||
            expected.alignment != actual.alignment || expected.fields.size() != actual.fields.size();
        if (is_identity_mismatch || is_layout_mismatch) return cxx::unexpected(RenderError{kDataSchemaMismatch});
        for (std::size_t index = 0; index < expected.fields.size(); ++index)
        {
            const auto& left = expected.fields[index];
            const auto& right = actual.fields[index];
            const bool is_field_mismatch = left.name != right.name || left.scalar != right.scalar ||
                left.count != right.count || left.kind != right.kind ||
                left.reference_alignment != right.reference_alignment ||
                !std::ranges::equal(left.enum_values, right.enum_values);
            if (is_field_mismatch) return cxx::unexpected(RenderError{kDataSchemaMismatch, {index}});
        }
        return {};
    }

    std::span<const error::ErrorDescriptor> renderDataErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.data.invalid_schema", "Invalid static wire schema", error::ERecovery::PERMANENT},
            {"lux.render.data.schema_mismatch", "Wire schema does not exactly match", error::ERecovery::PERMANENT},
            {"lux.render.data.invalid_wire", "Invalid wire record", error::ERecovery::PERMANENT},
            {"lux.render.data.wire_limit", "Wire byte limit exceeded", error::ERecovery::PERMANENT},
            {"lux.render.data.invalid_reference", "Invalid owned byte range", error::ERecovery::PERMANENT}
        };
        return descriptors;
    }
}
