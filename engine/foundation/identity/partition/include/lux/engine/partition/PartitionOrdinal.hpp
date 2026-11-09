#pragma once

#include <cstdint>

namespace lux::partition
{
    /** Dense, build-product-local index. It has no cross-cook identity. */
    struct PartitionOrdinal final
    {
        std::uint32_t value{};

        friend bool operator==(const PartitionOrdinal&, const PartitionOrdinal&) noexcept = default;
    };
}
