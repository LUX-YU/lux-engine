#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#if LUA_VERSION_RELEASE_NUM != 50501
#error Expected Lua 5.5.1
#endif
int main(void)
{
    lua_State* state = luaL_newstate();
    lua_State* thread;
    int reference, results = 0;
    if (!state || lua_version(state) != 505) return 1;
    luaL_openlibs(state);
    if (luaL_loadstring(state,
        "close_count=0; return function() "
        "local value <close> = setmetatable({}, {__close=function() "
        "close_count=close_count+1; error('expected close failure') end}); coroutine.yield() end") != LUA_OK) return 2;
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) return 3;
    thread = lua_newthread(state);
    reference = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_xmove(state, thread, 1);
    if (lua_resume(thread, state, 0, &results) != LUA_YIELD || results != 0) return 4;
    if (lua_closethread(thread, state) != LUA_ERRRUN) return 5;
    if (!lua_isstring(thread, -1) || !strstr(lua_tostring(thread, -1), "expected close failure")) return 6;
    lua_getglobal(state, "close_count");
    if (lua_tointeger(state, -1) != 1) return 7;
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    if (lua_gettop(state) != 0) return 8;
    lua_close(state);
    puts("LUA55_CLOSE_ERROR yield=1 status=LUA_ERRRUN close_count=1 parent_stack=0 PASS");
    return 0;
}
