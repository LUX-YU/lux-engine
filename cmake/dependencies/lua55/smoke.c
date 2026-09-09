#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>

#if LUA_VERSION_RELEASE_NUM != 50501
#error This smoke must compile against Lua 5.5.1
#endif

int main(void)
{
    lua_State* state = luaL_newstate();
    lua_State* thread;
    int reference, results = 0, parameter;
    if (!state) return 1;
    if (lua_version(state) != LUA_VERSION_NUM)
    {
        lua_close(state);
        fputs("LUA55_SMOKE: header/runtime version mismatch\n", stderr);
        return 13;
    }
    if (sizeof(lua_Number) != sizeof(double) || sizeof(lua_Integer) != 8 || (lua_Integer)-1 >= 0) return 2;
    luaL_openlibs(state);
    /* Exercise the actual 5.5 vararg contracts without changing engine GC policy. */
    if (lua_gc(state, LUA_GCINC) < 0) return 8;
    for (parameter = 0; parameter < LUA_GCPN; ++parameter)
    {
        int original = lua_gc(state, LUA_GCPARAM, parameter, -1);
        if (original < 0 || lua_gc(state, LUA_GCPARAM, parameter, original) != original) return 9;
        if (lua_gc(state, LUA_GCPARAM, parameter, -1) != original) return 10;
    }
    if (lua_gc(state, LUA_GCGEN) < 0 || lua_gc(state, LUA_GCSTEP, (size_t)0) < 0) return 11;
    if (lua_gc(state, LUA_GCINC) < 0) return 12;
    if (luaL_loadstring(state, "return function() coroutine.yield(); return 42 end") != LUA_OK) return 3;
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) return 4;
    thread = lua_newthread(state);
    reference = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_xmove(state, thread, 1);
    if (lua_resume(thread, state, 0, &results) != LUA_YIELD || results != 0) return 5;
    if (lua_resume(thread, state, 0, &results) != LUA_OK || results != 1 || lua_tointeger(thread, -1) != 42) return 6;
    if (lua_closethread(thread, state) != LUA_OK) return 7;
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    lua_close(state);
    puts("LUA55_SMOKE,version=" LUA_RELEASE ",yield=1,result=42,close=1");
    return 0;
}
