#pragma once
#include "CollisionValue.hpp"
#include <lua.hpp>
#include <cstddef>

// Diagnostic only: no codec, user rule, owning local or noexcept boundary inside the protected C function.
struct ProtectedRecord final
{
    const CollisionEvent* value{};
    int fail_after{};
    inline static char key;
    inline static std::size_t calls{};
    static void count(lua_State*, lua_Debug* event)
    {
        if (event->event == LUA_HOOKCALL) ++calls;
    }
    static int write(lua_State* state)
    {
        const auto* input = static_cast<const ProtectedRecord*>(lua_touserdata(state, 1));
        lua_createtable(state, 0, 2);
        if (input->fail_after == 1) return luaL_error(state, "after table");
        lua_pushliteral(state, "body");
        lua_pushinteger(state, input->value->body);
        lua_rawset(state, -3);
        if (input->fail_after == 2) return luaL_error(state, "after body");
        lua_pushliteral(state, "impulse");
        lua_pushnumber(state, input->value->impulse);
        lua_rawset(state, -3);
        return 1;
    }
    static int install(lua_State* state)
    {
        lua_pushlightuserdata(state, &key);
        lua_pushcfunction(state, write);
        lua_rawset(state, LUA_REGISTRYINDEX);
        return 0;
    }
    static bool initialize(lua_State* state) noexcept
    {
        if (!lua_checkstack(state, 2)) return false;
        const int base = lua_gettop(state);
#if LUA_VERSION_NUM == 501
        const int status = lua_cpcall(state, install, nullptr);
#else
        lua_pushcfunction(state, install);
        const int status = lua_pcall(state, 0, 0, 0);
#endif
        lua_settop(state, base);
        return status == 0;
    }
    bool push(lua_State* state) const noexcept
    {
        if (!lua_checkstack(state, 3)) return false;
        const int base = lua_gettop(state);
        lua_pushlightuserdata(state, &key);
        lua_rawget(state, LUA_REGISTRYINDEX);
        lua_pushlightuserdata(state, const_cast<ProtectedRecord*>(this));
        const int status = lua_pcall(state, 1, 1, 0);
        if (status) lua_settop(state, base);
        return status == 0;
    }
};
