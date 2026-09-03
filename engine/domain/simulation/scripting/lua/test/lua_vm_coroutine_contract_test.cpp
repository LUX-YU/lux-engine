#include <lux/engine/function/script/lua/LuaVm.hpp>
#include <lux/engine/function/script/lua/detail/LuaVmCompatibility.hpp>

#include <lua.hpp>

#include <cassert>
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
    lua_close(state);
    return 0;
}
