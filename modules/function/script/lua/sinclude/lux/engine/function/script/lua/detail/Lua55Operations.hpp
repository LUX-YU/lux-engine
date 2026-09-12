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

    // Cold C bootstrap, including any allocation needed to push its C closure. The operation
    // receives context at stack slot 1, returns no values and must contain only trivial locals.
    // Restores the caller's stack on success and failure; returns the VM's status code.
    [[nodiscard]] LUX_FUNCTION_PUBLIC int bootstrapLuaOperation(
        lua_State* state, int (*operation)(lua_State*), void* context
    ) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool configureLuaVm(
        lua_State* state,
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

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool setLuaChunkEnvironment(
        lua_State* state,
        int chunk_index,
        int environment_index
    ) noexcept;
} // namespace lux::script::lua::detail
