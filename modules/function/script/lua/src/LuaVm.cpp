#include <lux/engine/function/script/lua/detail/LuaVmCompatibility.hpp>

#include <lua.hpp>

#if defined(LUX_SCRIPT_LUA_VM_LUAJIT)
#    include <luajit.h>
#endif

namespace lux::script::lua::detail
{
#if defined(LUX_SCRIPT_LUA_VM_LUA54)
    namespace
    {
        int finishYieldedAbility(lua_State* state, int, lua_KContext context)
        {
            const auto base = static_cast<int>(context);
            const auto top = lua_gettop(state);
            if (top < base)
                return luaL_error(state, "invalid Lux Lua resume stack");
            return top - base;
        }
    } // namespace
#endif

    bool configureLuaVm(
        lua_State* state,
        ELuaExecutionPolicy policy,
        LuaRuntimeInfo& result
    ) noexcept
    {
        const bool valid_policy = policy == ELuaExecutionPolicy::DEFAULT ||
            policy == ELuaExecutionPolicy::INTERPRETER_ONLY;
        if (state == nullptr || !valid_policy)
            return false;
#if defined(LUX_SCRIPT_LUA_VM_LUAJIT)
        const int mode = policy == ELuaExecutionPolicy::INTERPRETER_ONLY
            ? LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF
            : LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON;
        if (luaJIT_setmode(state, 0, mode) == 0)
            return false;
        if (policy == ELuaExecutionPolicy::INTERPRETER_ONLY &&
            luaJIT_setmode(state, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_FLUSH) == 0)
        {
            return false;
        }
        result = {
            "LuaJIT",
            LUAJIT_VERSION,
            true,
            policy != ELuaExecutionPolicy::INTERPRETER_ONLY
        };
        return true;
#elif defined(LUX_SCRIPT_LUA_VM_LUA54)
        result = {"Lua54", LUA_RELEASE, false, false};
        return true;
#else
#    error "Lux Lua VM implementation was not selected"
#endif
    }

    LuaResumeResult resumeLuaVm(
        lua_State* thread,
        lua_State* caller,
        int argument_count
    ) noexcept
    {
        if (thread == nullptr || argument_count < 0)
            return {LUA_ERRRUN, 0};
#if defined(LUX_SCRIPT_LUA_VM_LUAJIT)
        (void)caller;
        const auto status = lua_resume(thread, argument_count);
        return {status, lua_gettop(thread)};
#elif defined(LUX_SCRIPT_LUA_VM_LUA54)
        int result_count{};
        const auto status = lua_resume(thread, caller, argument_count, &result_count);
        return {status, result_count};
#else
#    error "Lux Lua VM implementation was not selected"
#endif
    }

    int yieldLuaInvocation(lua_State* state, int result_count) noexcept
    {
        if (state == nullptr || result_count < 0 || result_count > lua_gettop(state))
            return luaL_error(state, "invalid Lux Lua yield result count");
#if defined(LUX_SCRIPT_LUA_VM_LUAJIT)
        return lua_yield(state, result_count);
#elif defined(LUX_SCRIPT_LUA_VM_LUA54)
        const auto preserved_count = lua_gettop(state) - result_count;
        return lua_yieldk(
            state,
            result_count,
            static_cast<lua_KContext>(preserved_count),
            &finishYieldedAbility
        );
#else
#    error "Lux Lua VM implementation was not selected"
#endif
    }
} // namespace lux::script::lua::detail
