#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::rdesc
{
    enum class EDataLayout
    {
        STD140,
        STD430
    };
    enum class EDataScalar
    {
        FLOAT,
        SINT,
        UINT
    };

    // Expanded leaf path, including array indices. All offsets are GPU byte offsets.
    struct DataGpuField
    {
        std::string_view path;
        EDataScalar scalar;
        std::uint32_t offset, rows, columns, array_stride, matrix_stride;
        bool operator==(const DataGpuField&) const noexcept = default;
    };

    // Synchronous view; generated instances borrow static arrays/string literals.
    struct DataGpuLayout
    {
        EDataLayout storage;
        std::uint32_t size, alignment;
        std::span<const DataGpuField> fields;
        std::string_view declarations;
        std::string_view structural_identity;
        std::uint64_t fingerprint;
    };
} // namespace lux::rdesc
