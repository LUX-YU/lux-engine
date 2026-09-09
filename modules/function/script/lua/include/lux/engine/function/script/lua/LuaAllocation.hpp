#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::script::lua
{
    enum class ELuaGcMode : std::uint8_t { INCREMENTAL, GENERATIONAL };
    enum class ELuaGcParameter : std::uint8_t
    {
        MINOR_MULTIPLIER, MAJOR_TO_MINOR, MINOR_TO_MAJOR, PAUSE, STEP_MULTIPLIER, STEP_SIZE
    };
    struct LuaVmConfiguration final
    {
        std::size_t cache_bytes{16U * 1024U * 1024U};
        unsigned seed{1592598566U};
        ELuaGcMode gc_mode{ELuaGcMode::GENERATIONAL};
        // Indexed by ELuaGcParameter; -1 preserves the upstream value.
        std::array<int, 6U> gc_parameters{-1, -1, -1, -1, -1, -1};
        bool track_allocations{};
    };
    struct LuaAllocationStats final
    {
        bool enabled{};
        std::uint64_t allocations{}, reallocations{}, frees{}, failures{};
        std::uint64_t requested_bytes{}, released_bytes{};
        std::uint64_t system_allocations{}, system_frees{}, cache_hits{}, in_place{};
        std::size_t live_bytes{}, peak_live_bytes{}, retained_bytes{}, peak_retained_bytes{};
    };
}
