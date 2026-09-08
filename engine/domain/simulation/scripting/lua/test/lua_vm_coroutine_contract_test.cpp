#include <lux/engine/function/script/lua/LuaVm.hpp>
#include <lux/engine/function/script/lua/detail/LuaVmCompatibility.hpp>

#include <lua.hpp>

#include <cassert>
#include <cstdio>
#include <string_view>

namespace
{
    int yieldForResume(lua_State* state)
    {
        assert(lua_gettop(state) == 0);
        return lux::script::lua::detail::yieldLuaInvocation(state, 0);
    }
}

int main(int argc, char** argv)
{
    const bool interpreter_only = argc == 2 && std::string_view{argv[1]} == "--interpreter-only";
    lua_State* state = luaL_newstate();
    assert(state != nullptr);
    luaL_openlibs(state);

    lux::script::lua::LuaRuntimeInfo runtime;
    assert(lux::script::lua::detail::configureLuaVm(
        state,
        interpreter_only
            ? lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY
            : lux::script::lua::ELuaExecutionPolicy::DEFAULT,
        runtime
    ));
    assert(!runtime.vm.empty() && !runtime.version.empty());
    assert(runtime.jit_available || !runtime.jit_enabled);
    assert(!interpreter_only || !runtime.jit_enabled);

    lua_pushcfunction(state, &yieldForResume);
    lua_setglobal(state, "engine_wait");
    assert(luaL_loadstring(
        state,
        "return function() local value = engine_wait(); return value + 1 end"
    ) == LUA_OK);
    assert(lua_pcall(state, 0, 1, 0) == LUA_OK);
    assert(lua_isfunction(state, -1));

    lua_State* thread = lua_newthread(state);
    assert(thread != nullptr);
    const int thread_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    assert(thread_ref != LUA_NOREF && thread_ref != LUA_REFNIL);
    lua_xmove(state, thread, 1);

    lua_gc(state, LUA_GCCOLLECT, 0);
    const auto suspended = lux::script::lua::detail::resumeLuaVm(thread, nullptr, 0);
    assert(suspended.status == LUA_YIELD && suspended.result_count == 0);
    assert(lua_status(thread) == LUA_YIELD);

    lua_pushnumber(thread, 41.0);
    const auto completed = lux::script::lua::detail::resumeLuaVm(thread, nullptr, 1);
    assert(completed.status == LUA_OK && completed.result_count == 1);
    assert(lua_tonumber(thread, -1) == 42.0);

    lua_settop(thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, thread_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    // A script can retain coroutine.running() both directly and through a closure. Retiring the
    // backend registry reference does not make that thread available for another invocation.
    assert(luaL_loadstring(state, R"lua(
        retained = {}
        function remember()
            local self = coroutine.running()
            retained[#retained + 1] = self
            retained_closure = function() return self end
            coroutine.yield()
            return 7
        end
    )lua") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
    auto* retained_thread = lua_newthread(state);
    const auto retained_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_getglobal(retained_thread, "remember");
    assert(lux::script::lua::detail::resumeLuaVm(retained_thread, nullptr, 0).status == LUA_YIELD);
    assert(lux::script::lua::detail::resumeLuaVm(retained_thread, nullptr, 0).status == LUA_OK);
    lua_settop(retained_thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, retained_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    assert(luaL_loadstring(state,
        "assert(coroutine.status(retained[1]) == 'dead'); assert(retained_closure() == retained[1])") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);

    auto* fresh_thread = lua_newthread(state);
    const auto fresh_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_getglobal(fresh_thread, "remember");
    assert(lux::script::lua::detail::resumeLuaVm(fresh_thread, nullptr, 0).status == LUA_YIELD);
    assert(luaL_loadstring(state,
        "assert(retained[1] ~= retained[2]); assert(coroutine.status(retained[1]) == 'dead'); "
        "assert(coroutine.status(retained[2]) == 'suspended')") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
    // Cancellation releases the backend reference, while Lua reachability still retains the thread.
    lua_settop(fresh_thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, fresh_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    assert(luaL_loadstring(state,
        "assert(type(retained[2]) == 'thread'); assert(retained_closure() == retained[2]); "
        "local t=coroutine.create(function() error('expected') end); "
        "assert(not coroutine.resume(t)); assert(coroutine.status(t)=='dead')") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
#if LUA_VERSION_NUM >= 504
    assert(luaL_loadstring(state,
        "closed=0; local t=coroutine.create(function() "
        "local x <close> = setmetatable({}, {__close=function() closed=closed+1 end}); "
        "coroutine.yield() end); assert(coroutine.resume(t)); assert(closed==0); "
        "assert(coroutine.close(t)); assert(closed==1)") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
#endif
    std::puts("THREAD_REUSE_REJECTED,retained_after_unref=1,closure=1,identity=1,dead=1,cancel=1,error=1");
    lua_close(state);
    return 0;
}
