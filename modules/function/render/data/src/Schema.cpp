#include <lux/engine/render/data/Schema.hpp>

#include <algorithm>
#include <bit>

namespace lux::render
{
    RenderResult<void> validateDataSchema(const DataSchemaDescriptor& schema) noexcept
    {
        const bool is_invalid_identity = !schema.id.isValid() || schema.id != renderDataTypeId(schema.canonical_name);
        if (is_invalid_identity)
        {
            return cxx::unexpected(RenderError{kInvalidIdentity});
        }
        if (schema.schema_version == 0)
        {
            return cxx::unexpected(RenderError{kInvalidVersion});
        }
        const bool is_invalid_scope = schema.scope != EDataScope::RUNTIME && schema.scope != EDataScope::SCENE &&
                                      schema.scope != EDataScope::VIEW;
        const bool is_invalid_authority = schema.authority != EDataAuthority::CPU &&
                                          schema.authority != EDataAuthority::GPU &&
                                          schema.authority != EDataAuthority::EXTERNAL;
        const bool has_no_representation = (!schema.cpu && !schema.wire && !schema.gpu) || schema.fields.empty();
        const bool is_invalid_gpu_authority = schema.authority == EDataAuthority::GPU && (!schema.gpu || schema.cpu);
        if (is_invalid_scope || is_invalid_authority || has_no_representation || is_invalid_gpu_authority)
        {
            return cxx::unexpected(RenderError{kDefinitionMismatch});
        }
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
        {
            const auto& field = schema.fields[i];
            if (field.name.empty() || field.cpp_type.empty())
            {
                return cxx::unexpected(RenderError{kDefinitionMismatch});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (schema.fields[j].name == field.name)
                {
                    return cxx::unexpected(RenderError{kDefinitionMismatch});
                }
            }
        }
        if (schema.cpu)
        {
            const auto& cpu = *schema.cpu;
            const bool is_invalid_cpu =
                cpu.size == 0 || !std::has_single_bit(cpu.alignment) || cpu.size % cpu.alignment != 0;
            if (is_invalid_cpu)
            {
                return cxx::unexpected(RenderError{kInvalidLayout});
            }
        }
        if (schema.wire && (schema.wire->codec.empty() || schema.wire->version == 0))
        {
            return cxx::unexpected(RenderError{kInvalidVersion});
        }
        if (schema.gpu)
        {
            const auto& gpu = *schema.gpu;
            const bool is_invalid_storage =
                gpu.storage != rdesc::EDataLayout::STD140 && gpu.storage != rdesc::EDataLayout::STD430;
            const bool is_invalid_layout = is_invalid_storage || gpu.size == 0 || !std::has_single_bit(gpu.alignment) ||
                                           gpu.size % gpu.alignment != 0 || gpu.fields.empty() ||
                                           gpu.structural_identity.empty() || gpu.declarations.empty() ||
                                           (gpu.storage == rdesc::EDataLayout::STD140 && gpu.alignment < 16) ||
                                           gpu.fingerprint != cxx::Fnv1a64::hash(gpu.structural_identity);
            if (is_invalid_layout)
            {
                return cxx::unexpected(RenderError{kInvalidLayout});
            }
            for (std::size_t index = 0; index < gpu.fields.size(); ++index)
            {
                const auto& field = gpu.fields[index];
                const auto alignment = field.rows == 1 ? 4u : field.rows == 2 ? 8u : 16u;
                const bool is_invalid_scalar = field.scalar != rdesc::EDataScalar::FLOAT &&
                                               field.scalar != rdesc::EDataScalar::SINT &&
                                               field.scalar != rdesc::EDataScalar::UINT;
                const bool is_invalid_matrix =
                    field.columns == 1
                        ? field.matrix_stride != 0
                        : field.scalar != rdesc::EDataScalar::FLOAT || field.matrix_stride < field.rows * 4 ||
                              field.matrix_stride % alignment != 0 ||
                              (gpu.storage == rdesc::EDataLayout::STD140 && field.matrix_stride % 16 != 0);
                const auto field_bytes =
                    std::uint64_t{field.matrix_stride} * (field.columns ? field.columns - 1 : 0) + field.rows * 4;
                const bool is_invalid_array =
                    field.array_stride != 0 &&
                    (field.array_stride < field_bytes || field.array_stride % alignment != 0 ||
                     (gpu.storage == rdesc::EDataLayout::STD140 && field.array_stride % 16 != 0));
                const bool is_invalid_shape = field.rows == 0 || field.rows > 4 || field.columns == 0 ||
                                              field.columns > 4 || field.offset % 4 != 0;
                const auto end = std::uint64_t{field.offset} + field_bytes;
                const bool is_invalid_field = is_invalid_shape || is_invalid_scalar || is_invalid_matrix ||
                                              is_invalid_array || field.path.empty() || field.offset % alignment != 0 ||
                                              alignment > gpu.alignment || end > gpu.size;
                if (is_invalid_field)
                {
                    return cxx::unexpected(RenderError{kInvalidLayout});
                }
                for (std::size_t previous = 0; previous < index; ++previous)
                {
                    const auto& other = gpu.fields[previous];
                    const auto other_end = std::uint64_t{other.offset} +
                                           std::uint64_t{other.matrix_stride} * (other.columns - 1) + other.rows * 4;
                    const bool is_overlap = field.offset < other_end && other.offset < end;
                    if (field.path == other.path || is_overlap)
                    {
                        return cxx::unexpected(RenderError{kInvalidLayout});
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> compatibleDataSchemas(
        const DataSchemaDescriptor& existing,
        const DataSchemaDescriptor& candidate
    ) noexcept
    {
        if (existing.id == candidate.id && existing.canonical_name != candidate.canonical_name)
        {
            return cxx::unexpected(RenderError{kIdentityCollision});
        }
        for (const auto* schema : {&existing, &candidate})
        {
            auto result = validateDataSchema(*schema);
            if (!result)
            {
                return result;
            }
        }
        const bool is_common_mismatch =
            existing.id != candidate.id || existing.schema_version != candidate.schema_version ||
            existing.scope != candidate.scope || existing.authority != candidate.authority ||
            existing.cpu != candidate.cpu || existing.wire != candidate.wire ||
            !std::ranges::equal(existing.fields, candidate.fields) ||
            existing.gpu.has_value() != candidate.gpu.has_value();
        if (is_common_mismatch)
        {
            return cxx::unexpected(RenderError{kDefinitionMismatch});
        }
        if (existing.gpu)
        {
            const auto& a = *existing.gpu;
            const auto& b = *candidate.gpu;
            const bool is_gpu_mismatch = a.storage != b.storage || a.size != b.size || a.alignment != b.alignment ||
                                         a.fingerprint != b.fingerprint ||
                                         a.structural_identity != b.structural_identity ||
                                         a.declarations != b.declarations || !std::ranges::equal(a.fields, b.fields);
            if (is_gpu_mismatch)
            {
                return cxx::unexpected(RenderError{kDefinitionMismatch});
            }
        }
        return {};
    }
} // namespace lux::render
