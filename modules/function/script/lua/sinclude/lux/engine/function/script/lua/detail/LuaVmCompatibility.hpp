#pragma once

#include <lux/engine/function/script/lua/LuaVm.hpp>
#include <lux/engine/function/visibility.h>

struct lua_State;

namespace lux::script::lua::detail
{
    struct LuaResumeResult final
    {
        int status{};
        int result_count{};
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool configureLuaVm(
        lua_State* state,
        ELuaExecutionPolicy policy,
        LuaRuntimeInfo& result
    ) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC LuaResumeResult resumeLuaVm(
        lua_State* thread,
        lua_State* caller,
        int argument_count
    ) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC int yieldLuaInvocation(
        lua_State* state,
        int result_count
    ) noexcept;

    LUX_FUNCTION_PUBLIC void pushLuaGlobalEnvironment(lua_State* state) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool setLuaChunkEnvironment(
        lua_State* state,
        int chunk_index,
        int environment_index
    ) noexcept;
} // namespace lux::script::lua::detail
