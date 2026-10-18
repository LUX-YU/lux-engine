#pragma once

#include <lux/engine/description/DataLayout.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    // Owning counterpart of DataGpuField. Cooked artifacts cannot retain the source's borrowed paths.
    struct ShaderMember final
    {
        std::string path;
        rdesc::EDataScalar scalar;
        std::uint32_t offset, rows, columns, array_stride, matrix_stride;
        bool operator==(const ShaderMember&) const noexcept = default;
    };

    struct ShaderBlock final
    {
        std::uint32_t size = 0;
        std::uint32_t alignment = 1;
        rdesc::EDataLayout storage = rdesc::EDataLayout::STD430;
        std::vector<ShaderMember> fields;
        bool runtime_records = false; // one zero-offset runtime array of this record, stride == size
        bool operator==(const ShaderBlock&) const noexcept = default;

        // A cold copy, not a second Schema registry or validator. Native reflection checks the ABI boundary.
        [[nodiscard]] static ShaderBlock from(const rdesc::DataGpuLayout& layout, bool records = false) noexcept
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
                     field.matrix_stride}
                );
            }
            return result;
        }
    };
} // namespace lux::render
