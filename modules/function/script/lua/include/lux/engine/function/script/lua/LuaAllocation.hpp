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
        // Completely idle backing pages only. Live-page slack is not covered by this budget.
        std::size_t idle_page_budget_bytes{16U * 1024U * 1024U};
        unsigned seed{1592598566U};
        ELuaGcMode gc_mode{ELuaGcMode::INCREMENTAL};
        // Indexed by ELuaGcParameter; -1 preserves the upstream value.
        std::array<int, 6U> gc_parameters{-1, -1, -1, -1, -1, -1};
        bool track_allocations{};
    };
    struct LuaPageClassStats final
    {
        std::size_t payload{}, stride{}, capacity{}, tail{}, active_pages{}, idle_pages{};
        std::uint64_t requests{}, frees{}, requested_bytes{}, supplied{}, same_reuses{}, cross_reuses{};
        std::uint64_t idle_limit_releases{}, trim_releases{}, shutdown_releases{}, fallback{}, header_writes{};
    };
    struct LuaAllocationStats final
    {
        bool enabled{};
        std::uint64_t allocations{}, reallocations{}, frees{}, failures{};
        std::uint64_t requested_bytes{}, released_bytes{};
        std::uint64_t system_allocations{}, system_frees{}, in_place{};
        std::uint64_t page_allocations{}, page_frees{}, page_reuses{}, direct_allocations{}, direct_frees{};
        std::uint64_t page_fallbacks{}, slot_reuses{};
        std::size_t live_bytes{}, peak_live_bytes{};
        // Active backing = small requested live + rounding + pinned free slots + active metadata.
        std::size_t active_page_backing_bytes{}, idle_page_backing_bytes{}, peak_idle_page_backing_bytes{};
        std::size_t pinned_free_slot_bytes{}, class_rounding_bytes{}, metadata_and_header_bytes{};
        std::size_t large_block_backing_bytes{}, large_requested_live_bytes{};
        // Fixed diagnostic storage; callbacks compiled without Track do not update it.
        std::array<LuaPageClassStats, 9U> classes{};
        std::size_t page_header_bytes{}, block_header_bytes{};
        // Read only in owner-side diagnostic snapshots, never from lua_Alloc.
        std::array<int, 6U> gc_parameters{-1, -1, -1, -1, -1, -1};
    };
}
