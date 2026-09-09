#include <lux/engine/function/script/lua/LuaAllocationCache.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>
#include <lux/engine/function/script/lua/Lua.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace lux::script::lua;
int main()
{
    LuaAllocationCache cache({.cache_bytes = 4096U, .track_allocations = true});
    auto allocate = [&](void* pointer, std::size_t size) { return LuaAllocationCache::allocate(&cache, pointer, 8, size); };
    auto* block = allocate(nullptr, 216U);
    assert(block && reinterpret_cast<std::uintptr_t>(block) % alignof(std::max_align_t) == 0U);
    std::memset(block, 0x71, 216U);
    assert(allocate(block, 224U) == block);
    cache.failSystemAfter(0U);
    assert(allocate(block, 720U) == nullptr);
    for (std::size_t i{}; i < 216U; ++i) assert(static_cast<unsigned char*>(block)[i] == 0x71U);
    assert(cache.stats().live_bytes == 224U);
    allocate(block, 0U);
    auto* reused = allocate(nullptr, 216U);
    assert(reused == block && cache.stats().cache_hits == 1U);
    allocate(reused, 0U);
    cache.clear();
    assert(cache.stats().live_bytes == 0U && cache.stats().retained_bytes == 0U);
    assert(cache.stats().system_allocations == cache.stats().system_frees);
    LuaAllocationCache vm_cache({.cache_bytes = 4096U, .track_allocations = true});
    auto* vm = lua_newstate(&LuaAllocationCache::allocate, &vm_cache, 1592598566U);
    assert(vm);
    lua_pushcfunction(vm, &luxLuaBootstrap);
    assert(lua_pcall(vm, 0, 0, 0) == LUA_OK);
    assert(luaL_dostring(vm, "local t={} for i=1,1000 do t[i]=coroutine.create(function() return i end) end") == LUA_OK);
    lua_close(vm);
    assert(vm_cache.stats().live_bytes == 0U && vm_cache.stats().retained_bytes <= 4096U);
    vm_cache.clear();
    assert(vm_cache.stats().system_allocations == vm_cache.stats().system_frees);
    LuaAllocationCache failing({});
    failing.failSystemAfter(0U);
    assert(lua_newstate(&LuaAllocationCache::allocate, &failing, 0U) == nullptr);
    for (auto mode : {ELuaGcMode::INCREMENTAL, ELuaGcMode::GENERATIONAL})
    {
        ScriptEngine engine({.gc_mode = mode, .gc_parameters = {-1, -1, -1, 175, -1, -1}});
        assert(engine.state());
        assert(lua_gc(engine.state(), LUA_GCPARAM, LUA_GCPPAUSE, -1) == 175);
        assert(luaL_dostring(engine.state(), "assert(coroutine and debug and io and os and package and utf8)") == LUA_OK);
    }
    std::puts("ALLOCATOR_PASS,alignment=1,growth_failure_preserves=1,cache_hit=1,close_live=0,cache_bounded=1,gc_modes=2");
}
