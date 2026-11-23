#pragma once

#include <cstdint>

namespace lux::render
{
    enum class ETransformGraphProjection : std::uint8_t
    {
        RECORDS,
        PAGE_ADDRESSES
    };

    inline constexpr std::uint32_t kTransformInitialPageAddresses = 8;

    struct TransformBinding final
    {
        bool operator==(const TransformBinding&) const noexcept = default;
    };
} // namespace lux::render
