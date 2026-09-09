#include <lux/engine/function/script/lua/LuaBoundary.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int luxLuaBootstrap(lua_State* state)
{
    const int libraries = LUA_GLIBK | LUA_LOADLIBK | LUA_COLIBK | LUA_DBLIBK | LUA_IOLIBK |
        LUA_MATHLIBK | LUA_OSLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK;
    luaL_openselectedlibs(state, libraries, 0);
    return 0;
}

static int finishWait(lua_State* state, int status, lua_KContext context)
{
    int base = (int)context;
    int top = lua_gettop(state);
    (void)status;
    if (top < base) return luaL_error(state, "invalid Lux Lua resume stack");
    return top - base;
}

int luxLuaBoundaryEntry(lua_State* state)
{
    const LuxLuaTypedWorker* worker = (const LuxLuaTypedWorker*)lua_touserdata(state, lua_upvalueindex(4));
    LuxLuaBoundaryOutcome outcome;
    int base = lua_gettop(state);
    if (worker == NULL || *worker == NULL) return luaL_error(state, "invalid Lux Lua primitive");
    outcome = (*worker)(state);
    switch (outcome.kind) {
    case LUX_LUA_BOUNDARY_RETURN:
        return outcome.result_count;
    case LUX_LUA_BOUNDARY_SUSPEND:
#if defined(LUX_LUA55_LEAF_YIELD_REVISION)
        return luxlua_yieldleaf(state, (lua_KContext)base, finishWait);
#else
        return lua_yieldk(state, 0, (lua_KContext)base, finishWait);
#endif
    case LUX_LUA_BOUNDARY_ERROR:
        if (outcome.result_count == 1) return lua_error(state);
        return luaL_error(state, "Lux Lua primitive failed (%d)", outcome.error_code);
    default:
        return luaL_error(state, "invalid Lux Lua primitive outcome");
    }
}
