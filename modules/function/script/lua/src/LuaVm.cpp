#include <lux/engine/function/script/lua/detail/Lua55Operations.hpp>

#include <lua.hpp>


namespace lux::script::lua::detail
{
    int bootstrapLuaOperation(lua_State* state, int (*operation)(lua_State*), void* context) noexcept
    {
        if (state == nullptr || operation == nullptr) return LUA_ERRRUN;
        if (!lua_checkstack(state, 2)) return LUA_ERRMEM;
        const auto base = lua_gettop(state);
        // The standard VM represents a zero-upvalue C function without allocating a closure.
        lua_pushcfunction(state, operation);
        lua_pushlightuserdata(state, context);
        const auto status = lua_pcall(state, 1, 0, 0);
        lua_settop(state, base);
        return status;
    }

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

    bool configureLuaVm(
        lua_State* state,
        LuaRuntimeInfo& result
    ) noexcept
    {
        if (state == nullptr) return false;
        if (lua_version(state) != LUA_VERSION_NUM)
            return false;
        result = {"Lua55", LUA_RELEASE};
        return true;
    }

    LuaResumeResult resumeLuaVm(
        lua_State* thread,
        lua_State* caller,
        int argument_count
    ) noexcept
    {
        if (thread == nullptr || argument_count < 0)
            return {LUA_ERRRUN, 0};
        int result_count{};
        const auto status = lua_resume(thread, caller, argument_count, &result_count);
        return {status, result_count};
    }

    int yieldLuaInvocation(lua_State* state, int result_count) noexcept
    {
        if (state == nullptr || result_count < 0 || result_count > lua_gettop(state))
            return luaL_error(state, "invalid Lux Lua yield result count");
        const auto preserved_count = lua_gettop(state) - result_count;
        return lua_yieldk(
            state,
            result_count,
            static_cast<lua_KContext>(preserved_count),
            &finishYieldedAbility
        );
    }

    bool setLuaChunkEnvironment(
        lua_State* state,
        int chunk_index,
        int environment_index
    ) noexcept
    {
        if (state == nullptr)
            return false;
        const auto top = lua_gettop(state);
        const auto absolute_chunk = chunk_index < 0 ? top + chunk_index + 1 : chunk_index;
        const auto absolute_environment = environment_index < 0 ? top + environment_index + 1 : environment_index;
        if (absolute_chunk <= 0 || absolute_environment <= 0 || absolute_chunk > top || absolute_environment > top)
            return false;
        lua_pushvalue(state, absolute_environment);
        const auto* name = lua_setupvalue(state, absolute_chunk, 1);
        return name != nullptr && std::string_view{name} == "_ENV";
    }
} // namespace lux::script::lua::detail
