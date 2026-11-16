#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::rdesc
{
    enum class EDataLayout
    {
        STD140,
        STD430,
        TARGET_REFLECTED
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
        std::uint32_t offset;
        std::uint32_t rows;
        std::uint32_t columns;
        std::uint32_t array_stride;
        std::uint32_t matrix_stride;

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

        bool operator==(const DataGpuLayout& other) const noexcept
        {
            return storage == other.storage && size == other.size && alignment == other.alignment &&
                   declarations == other.declarations && structural_identity == other.structural_identity &&
                   fingerprint == other.fingerprint && std::ranges::equal(fields, other.fields);
        }
    };
} // namespace lux::rdesc
