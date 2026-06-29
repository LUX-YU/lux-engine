#pragma once
#include <cstdint>

namespace lux::input
{
    /// Runtime-allocated action identifier (small integer from SparseSet).
    /// 0 is reserved as the invalid / unassigned sentinel.
    using ActionId = uint32_t;

    inline constexpr ActionId InvalidActionId = 0;

} // namespace lux::input
