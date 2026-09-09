#include <lux/engine/function/script/lua/LuaPageAllocator.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>
#include <lux/engine/function/script/lua/Lua.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace lux::script::lua;

static void randomized(bool tracked, std::size_t budget)
{
    LuaPageAllocator allocator({.idle_page_budget_bytes = budget, .track_allocations = tracked});
    auto allocate = allocator.callback();
    struct Live { void* pointer{}; std::size_t size{}; unsigned char value{}; };
    std::array<Live, 127> slots{};
    std::mt19937 random(1592598566U);
    for (unsigned iteration{}; iteration < 20000U; ++iteration)
    {
        auto& slot = slots[random() % slots.size()];
        const auto size = random() % 8193U;
        auto* next = allocate(&allocator, slot.pointer, slot.pointer ? slot.size : LUA_TTHREAD, size);
        if (size)
        {
            assert(next && reinterpret_cast<std::uintptr_t>(next) % alignof(std::max_align_t) == 0U);
            for (std::size_t i{}; i < (std::min)(slot.size, std::size_t(size)); ++i)
                assert(static_cast<unsigned char*>(next)[i] == slot.value);
            slot.value = static_cast<unsigned char>(random());
            std::memset(next, slot.value, size);
        }
        slot.pointer = next;
        slot.size = size;
    }
    for (auto slot : slots) allocate(&allocator, slot.pointer, slot.size, 0U);
    auto stats = allocator.stats();
    assert(stats.enabled == tracked && stats.live_bytes == 0U && stats.idle_page_backing_bytes <= budget);
    allocator.clear();
    assert(!allocator.hasLiveAllocations());
    stats = allocator.stats();
    assert(stats.system_allocations == stats.system_frees);
    std::printf("PAGE_RANDOM,tracked=%d,budget=%zu,operations=20000,live=0,balanced=1\n", tracked, budget);
}

static void pagesAndFailure()
{
    LuaPageAllocator allocator({.idle_page_budget_bytes = 65536U, .track_allocations = true});
    auto allocate = allocator.callback();
    std::vector<void*> blocks;
    for (unsigned i{}; i < 2048U; ++i) blocks.push_back(allocate(&allocator, nullptr, LUA_TTHREAD, 32U));
    assert(allocator.stats().page_allocations >= 2U);
    for (std::size_t i = 1U; i < blocks.size(); ++i) allocate(&allocator, blocks[i], 32U, 0U);
    auto stats = allocator.stats();
    assert(stats.active_page_backing_bytes == 65536U && stats.pinned_free_slot_bytes > 32000U);
    assert(stats.active_page_backing_bytes == stats.live_bytes + stats.class_rounding_bytes +
        stats.pinned_free_slot_bytes + stats.metadata_and_header_bytes);
    allocator.clear();
    assert(allocator.hasLiveAllocations() && allocator.stats().idle_page_backing_bytes == 0U);
    allocate(&allocator, blocks.front(), 32U, 0U);
    const auto heap_calls = allocator.stats().system_allocations;
    auto* changed = allocate(&allocator, nullptr, LUA_TTABLE, 4096U);
    assert(changed && allocator.stats().system_allocations == heap_calls);
    std::memset(changed, 0x71, 4096U);
    allocator.failSystemAfter(0U);
    assert(allocate(&allocator, changed, 4096U, 9000U) == nullptr);
    for (unsigned i{}; i < 4096U; ++i) assert(static_cast<unsigned char*>(changed)[i] == 0x71U);
    assert(allocate(&allocator, changed, 4096U, 16U) == changed);
    assert(allocate(&allocator, nullptr, 0U, (std::numeric_limits<std::size_t>::max)()) == nullptr);
    allocate(&allocator, changed, 16U, 0U);
    allocator.clear();
    assert(!allocator.hasLiveAllocations());

    LuaPageAllocator fallback({.idle_page_budget_bytes = 0U, .track_allocations = true});
    fallback.failNextPage();
    auto* direct = fallback.callback()(&fallback, nullptr, LUA_TTHREAD, 32U);
    assert(direct && fallback.stats().page_fallbacks == 1U && fallback.stats().direct_allocations == 1U);
    fallback.callback()(&fallback, direct, 32U, 0U);
    assert(!fallback.hasLiveAllocations());
    assert(fallback.stats().system_allocations == fallback.stats().system_frees);
    std::puts("PAGE_PROTOCOL,pinned=1,reclassify=1,growth_failure_preserves=1,precise_fallback=1,overflow=1");
}

int main()
{
    for (bool tracked : {false, true})
        for (std::size_t budget : {0U, 16U * 1024U * 1024U}) randomized(tracked, budget);
    pagesAndFailure();
    for (bool tracked : {false, true})
    {
        LuaPageAllocator allocator({.idle_page_budget_bytes = 65536U, .track_allocations = tracked});
        auto* vm = lua_newstate(allocator.callback(), &allocator, 1592598566U);
        assert(vm);
        lua_pushcfunction(vm, &luxLuaBootstrap);
        assert(lua_pcall(vm, 0, 0, 0) == LUA_OK);
        assert(luaL_dostring(vm,
            "local t={} for i=1,1000 do t[i]=coroutine.create(function() return i end) end "
            "for i=1,1000 do local ok,n=coroutine.resume(t[i]);assert(ok and n==i) end") == LUA_OK);
        lua_close(vm);
        assert(allocator.stats().live_bytes == 0U);
        allocator.clear();
        assert(!allocator.hasLiveAllocations());
        assert(allocator.stats().system_allocations == allocator.stats().system_frees);
    }
    LuaPageAllocator failing({});
    failing.failSystemAfter(0U);
    assert(lua_newstate(failing.callback(), &failing, 0U) == nullptr);
    for (auto mode : {ELuaGcMode::INCREMENTAL, ELuaGcMode::GENERATIONAL})
    {
        ScriptEngine engine({.gc_mode = mode, .gc_parameters = {-1, -1, -1, 175, -1, -1}});
        assert(engine.state());
        assert(lua_gc(engine.state(), LUA_GCPARAM, LUA_GCPPAUSE, -1) == 175);
        assert(luaL_dostring(engine.state(), "assert(coroutine and debug and io and os and package and utf8)") == LUA_OK);
    }
    std::puts("ALLOCATOR_PASS,alignment=1,close_live=0,idle_pages_bounded=1,gc_modes=2,diagnostic_oracles=2");
}
