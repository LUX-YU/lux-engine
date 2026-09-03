#include <lua.hpp>

#include <cassert>

namespace
{
    int yieldForResume(lua_State* state)
    {
        assert(lua_gettop(state) == 0);
        return lua_yield(state, 0);
    }
}

int main()
{
    lua_State* state = luaL_newstate();
    assert(state != nullptr);
    luaL_openlibs(state);

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
    assert(lua_resume(thread, 0) == LUA_YIELD);
    assert(lua_status(thread) == LUA_YIELD);
    assert(lua_gettop(thread) == 0);

    lua_pushinteger(thread, 41);
    assert(lua_resume(thread, 1) == LUA_OK);
    assert(lua_gettop(thread) == 1);
    assert(lua_tointeger(thread, -1) == 42);

    lua_settop(thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, thread_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    lua_close(state);
    return 0;
}
