#include <lux/engine/function/script/lua/LuaBoundary.h>
#include <lua.h>
#include <lauxlib.h>

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
        return lua_yieldk(state, 0, (lua_KContext)base, finishWait);
    case LUX_LUA_BOUNDARY_ERROR:
        if (outcome.result_count == 1) return lua_error(state);
        return luaL_error(state, "Lux Lua primitive failed (%d)", outcome.error_code);
    default:
        return luaL_error(state, "invalid Lux Lua primitive outcome");
    }
}
