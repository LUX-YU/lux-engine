#pragma once

#include <lux/engine/description/DataLayout.hpp>
#include <bit>
#include <cstddef>
#include <string>
#include <vector>

namespace lux::rdesc
{
    enum class EShaderMatrixOrder : std::uint8_t
    {
        UNSPECIFIED,
        NOT_MATRIX,
        COLUMN_MAJOR,
        ROW_MAJOR
    };

    struct ShaderArrayDimension final
    {
        std::uint32_t count, stride;
        bool operator==(const ShaderArrayDimension&) const noexcept = default;
    };

    // Owning counterpart of DataGpuField. Cooked artifacts cannot retain the source's borrowed paths.
    struct ShaderMember final
    {
        std::string path;
        EDataScalar scalar;
        std::uint32_t offset, rows, columns, array_stride, matrix_stride;
        EShaderMatrixOrder matrix_order = EShaderMatrixOrder::UNSPECIFIED;
        // Outer to inner, including dimensions inherited from arrays of structures.
        std::vector<ShaderArrayDimension> array_dimensions;
        bool operator==(const ShaderMember&) const noexcept = default;
    };

    struct ShaderBlock final
    {
        std::uint32_t size = 0;
        std::uint32_t alignment = 1;
        EDataLayout storage = EDataLayout::STD430;
        std::vector<ShaderMember> fields;
        bool runtime_records = false; // One zero-offset runtime record array; size is its reflected ArrayStride.
        bool operator==(const ShaderBlock&) const noexcept = default;

        // Structural qualification. Matching actual binary offsets remains a backend reflection check.
        [[nodiscard]] bool hasStructuredLayout() const noexcept
        {
            const bool has_storage_layout = storage == EDataLayout::STD140 || storage == EDataLayout::STD430 ||
                storage == EDataLayout::TARGET_REFLECTED;
            return size != 0 && std::has_single_bit(alignment) && !fields.empty() && has_storage_layout;
        }

        // Cooked target layout evidence. The backend must independently verify the binary and final native binding.
        [[nodiscard]] bool hasConstantLayoutEvidence() const noexcept
        {
            return hasStructuredLayout() && storage == EDataLayout::TARGET_REFLECTED && !runtime_records &&
                std::ranges::all_of(fields, [this](const ShaderMember& field) noexcept
                {
                    const bool is_matrix = field.matrix_order == EShaderMatrixOrder::COLUMN_MAJOR ||
                        field.matrix_order == EShaderMatrixOrder::ROW_MAJOR;
                    const bool is_scalar_or_vector = field.matrix_order == EShaderMatrixOrder::NOT_MATRIX &&
                        field.columns == 1 && field.matrix_stride == 0;
                    const bool has_valid_shape = field.rows > 0 && field.rows <= 4 &&
                        field.columns > 0 && field.columns <= 4 && field.scalar <= EDataScalar::UINT;
                    if (!has_valid_shape || (!is_matrix && !is_scalar_or_vector)) return false;
                    const bool is_row_major = field.matrix_order == EShaderMatrixOrder::ROW_MAJOR;
                    const auto major_count = is_row_major ? field.rows : field.columns;
                    const auto minor_bytes = 4u * (is_row_major ? field.columns : field.rows);
                    if (is_matrix && field.matrix_stride < minor_bytes) return false;
                    const auto bytes = std::uint64_t(major_count - 1) * field.matrix_stride + minor_bytes;
                    const auto array_stride = field.array_dimensions.empty() ? 0 : field.array_dimensions.back().stride;
                    return field.offset <= size && bytes <= size - field.offset && field.array_stride == array_stride &&
                        std::ranges::all_of(field.array_dimensions, [this](const ShaderArrayDimension& dim) noexcept
                        {
                            return dim.count != 0 && dim.stride != 0 &&
                                std::uint64_t(dim.count - 1) * dim.stride < size;
                        });
                });
        }

        static std::vector<std::byte> serialize(const ShaderBlock&);
        static bool deserialize(std::span<const std::byte>, ShaderBlock&);

        // A cold copy, not a second Schema registry or validator. Native reflection checks the ABI boundary.
        [[nodiscard]] static ShaderBlock from(const DataGpuLayout& layout, bool records = false) noexcept
        {
            ShaderBlock result{layout.size, layout.alignment, layout.storage, {}, records};
            result.fields.reserve(layout.fields.size());
            for (const auto& field : layout.fields)
            {
                result.fields.push_back(
                    {std::string(field.path),
                     field.scalar,
                     field.offset,
                     field.rows,
                     field.columns,
                     field.array_stride,
                     field.matrix_stride,
                     EShaderMatrixOrder::UNSPECIFIED,
                     {}}
                );
            }
            return result;
        }
    };
} // namespace lux::rdesc
