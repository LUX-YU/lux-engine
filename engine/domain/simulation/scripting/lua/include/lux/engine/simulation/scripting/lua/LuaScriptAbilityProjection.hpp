#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/lua/visibility.h>

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

struct lua_State;

namespace lux::simulation::script::detail
{
    template <class Value>
    inline constexpr bool LuaAbilityScalar =
        std::is_same_v<std::remove_cvref_t<Value>, bool> ||
        std::is_same_v<std::remove_cvref_t<Value>, std::int32_t> ||
        std::is_same_v<std::remove_cvref_t<Value>, std::uint32_t> ||
        std::is_same_v<std::remove_cvref_t<Value>, float> ||
        std::is_same_v<std::remove_cvref_t<Value>, double>;

    struct LuaPreparedAbilityAccess final
    {
        void* context{};
        const void* dispatch{};
        ScriptStepContext* step{};
        std::uint32_t local_slot{};
    };

    struct LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC LuaAbilityProjectionAccess final
    {
        [[nodiscard]] static bool current(lua_State* state, LuaPreparedAbilityAccess& result) noexcept;
        [[nodiscard]] static int fail(lua_State* state, std::int32_t status, const char* message) noexcept;
        [[nodiscard]] static int succeed(lua_State* state, int results) noexcept;
        [[nodiscard]] static int suspend(
            lua_State* state,
            ScriptStepResult result,
            std::uint32_t local_slot
        ) noexcept;
        [[nodiscard]] static int argumentCount(lua_State* state) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, bool& value) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, std::int32_t& value) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, std::uint32_t& value) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, float& value) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, double& value) noexcept;
        static void push(lua_State* state, bool value) noexcept;
        static void push(lua_State* state, std::int32_t value) noexcept;
        static void push(lua_State* state, std::uint32_t value) noexcept;
        static void push(lua_State* state, float value) noexcept;
        static void push(lua_State* state, double value) noexcept;
    };

    template <class... Arguments, std::size_t... Index>
    [[nodiscard]] bool readLuaAbilityArguments(
        lua_State* state,
        std::tuple<std::remove_cvref_t<Arguments>...>& values,
        std::index_sequence<Index...>
    ) noexcept
    {
        if constexpr ((LuaAbilityScalar<Arguments> && ...))
        {
            return (
                LuaAbilityProjectionAccess::read(
                    state,
                    static_cast<int>(Index + 1U),
                    std::get<Index>(values)
                ) && ...
            );
        }
        else
        {
            return false;
        }
    }

    template <class Result, class... Arguments, class Invoke>
    [[nodiscard]] int invokeLuaAbility(lua_State* state, Invoke invoke) noexcept
    {
        LuaPreparedAbilityAccess access;
        if (!LuaAbilityProjectionAccess::current(state, access))
            return LuaAbilityProjectionAccess::fail(state, -1, "invalid prepared Script Ability");
        if (LuaAbilityProjectionAccess::argumentCount(state) != static_cast<int>(sizeof...(Arguments)))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument count mismatch");
        std::tuple<std::remove_cvref_t<Arguments>...> values;
        if (!readLuaAbilityArguments<Arguments...>(state, values, std::index_sequence_for<Arguments...>{}))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument type mismatch");
        if constexpr (std::is_void_v<Result>)
        {
            std::apply([&](auto&... arguments) noexcept { invoke(access, arguments...); }, values);
            return LuaAbilityProjectionAccess::succeed(state, 0);
        }
        else
        {
            using Value = std::remove_cvref_t<Result>;
            static_assert(std::is_trivially_copyable_v<Value>);
            const Value result = std::apply(
                [&](auto&... arguments) noexcept -> Result { return invoke(access, arguments...); },
                values
            );
            if constexpr (LuaAbilityScalar<Value>)
            {
                LuaAbilityProjectionAccess::push(state, result);
                return LuaAbilityProjectionAccess::succeed(state, 1);
            }
            else
            {
                return LuaAbilityProjectionAccess::fail(state, -5, "unsupported Script Ability result");
            }
        }
    }

    template <class Result, class... Arguments, class Start>
    [[nodiscard]] int startLuaAbility(lua_State* state, Start start) noexcept
    {
        LuaPreparedAbilityAccess access;
        if (!LuaAbilityProjectionAccess::current(state, access) || access.step == nullptr)
            return LuaAbilityProjectionAccess::fail(state, -1, "async Script Ability requires coroutine execution");
        if (LuaAbilityProjectionAccess::argumentCount(state) != static_cast<int>(sizeof...(Arguments)))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument count mismatch");
        std::tuple<std::remove_cvref_t<Arguments>...> values;
        if (!readLuaAbilityArguments<Arguments...>(state, values, std::index_sequence_for<Arguments...>{}))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument type mismatch");
        const auto result = invokeScriptAbilityAsync<Result>(
            *access.step,
            [&](lux::script::ScriptAbilityCompletion<Result> completion) noexcept {
                return std::apply(
                    [&](auto&... arguments) noexcept {
                        return start(access, arguments..., std::move(completion));
                    },
                    values
                );
            }
        );
        return LuaAbilityProjectionAccess::suspend(state, result, access.local_slot);
    }
}
