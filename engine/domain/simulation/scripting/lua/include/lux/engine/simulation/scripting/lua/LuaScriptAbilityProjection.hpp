#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>
#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/lua/visibility.h>

#include <cstdint>
#include <charconv>
#include <cmath>
#include <limits>
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
        PreparedLocalAsyncStart local_async;
        std::uint32_t local_slot{};
        int argument_count{};
        const void* execution{};
        const ScriptBehavior* behavior{};
        ScriptInvocationValidity validity;
        bool has_core_authority{};
    };

    template <class Type>
    [[nodiscard]] bool checkedLuaNumber(double value, Type& result) noexcept
    {
        if (!std::isfinite(value)) return false;
        if constexpr (std::is_integral_v<Type>)
        {
            const bool invalid = std::trunc(value) != value ||
                value < static_cast<double>((std::numeric_limits<Type>::lowest)()) ||
                value > static_cast<double>((std::numeric_limits<Type>::max)());
            if (invalid) return false;
        }
        else
        {
            const bool invalid = value < -static_cast<double>((std::numeric_limits<Type>::max)()) ||
                value > static_cast<double>((std::numeric_limits<Type>::max)());
            if (invalid) return false;
        }
        result = static_cast<Type>(value);
        return true;
    }

    struct LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC LuaAbilityProjectionAccess final
    {
        [[nodiscard]] static bool current(lua_State* state, LuaPreparedAbilityAccess& result) noexcept;
        [[nodiscard]] static bool revalidate(lua_State* state, const LuaPreparedAbilityAccess& original) noexcept;
        [[nodiscard]] static LuxLuaBoundaryOutcome
        fail(lua_State* state, std::int32_t status, const char* message) noexcept;
        [[nodiscard]] static LuxLuaBoundaryOutcome succeed(lua_State* state, int results) noexcept;
        [[nodiscard]] static LuxLuaBoundaryOutcome suspend(
            lua_State* state,
            ScriptStepResult result,
            std::uint32_t local_slot
        ) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, bool& value) noexcept;
        [[nodiscard]] static bool read(lua_State* state, int index, std::int32_t& value) noexcept
        { return readNumeric(state, index, value); }
        [[nodiscard]] static bool read(lua_State* state, int index, std::uint32_t& value) noexcept
        { return readNumeric(state, index, value); }
        [[nodiscard]] static bool read(lua_State* state, int index, float& value) noexcept
        { return readNumeric(state, index, value); }
        [[nodiscard]] static bool read(lua_State* state, int index, double& value) noexcept
        { return readNumeric(state, index, value); }
        static void push(lua_State* state, bool value) noexcept;
        static void push(lua_State* state, std::int32_t value) noexcept;
        static void push(lua_State* state, std::uint32_t value) noexcept;
        static void push(lua_State* state, float value) noexcept;
        static void push(lua_State* state, double value) noexcept;
    private:
        [[nodiscard]] static bool number(lua_State* state, int index, double& value) noexcept;
        template <class Type>
        [[nodiscard]] static bool readNumeric(lua_State* state, int index, Type& result) noexcept
        {
            double value{};
            return number(state, index, value) && checkedLuaNumber(value, result);
        }
    };


    template <class Policy, class... Arguments, std::size_t... Index>
    [[nodiscard]] bool readLuaAbilityArguments(lua_State* state,
        lux::script::lua::LuaValueSlots<std::remove_cvref_t<Arguments>...>& values,
        std::index_sequence<Index...>, std::optional<lux::script::lua::LuaValueFailure>* failure = nullptr) noexcept
    {
        bool success = true;
        (([&]() noexcept {
            if (!success) return;
            using T = std::remove_cvref_t<Arguments>;
            lux::script::lua::LuaValueReader input{state, static_cast<int>(Index + 1)};
            auto value = lux::script::lua::LuaValueCodec<T, Policy>::read(input);
            if (value) values.template put<Index>(std::move(*value));
            else
            {
                success = false;
                if (failure)
                {
                    failure->emplace(value.error());
                    std::array<char, 32> name{'a', 'r', 'g', '['};
                    const auto converted = std::to_chars(name.data() + 4, name.data() + name.size() - 1, Index + 1);
                    *converted.ptr = ']';
                    failure->value().prepend({name.data(), static_cast<std::size_t>(converted.ptr - name.data() + 1)});
                }
            }
        }()), ...);
        return success;
    }

    template <class Policy, class Result, class... Arguments, class Invoke>
    [[nodiscard]] LuxLuaBoundaryOutcome invokeLuaValueAbility(lua_State* state, Invoke invoke) noexcept
    {
        using namespace lux::script::lua;
        using ValueAccess = lux::script::lua::detail::LuaValueAccess;
        LuaPreparedAbilityAccess access;
        if (!LuaAbilityProjectionAccess::current(state, access))
            return LuaAbilityProjectionAccess::fail(state, -1, "invalid prepared Script Ability");
        if (access.argument_count != static_cast<int>(sizeof...(Arguments)))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument count mismatch");
        constexpr bool can_reenter = !((LuaValueScalar<std::remove_cvref_t<Arguments>> &&
            !LuaValueCodec<std::remove_cvref_t<Arguments>, Policy>::custom) && ...);
        // current() admits every entry. Parameter reentry only determines whether that admission must be rechecked.
        std::optional<LuaValueFailure> failure;
        // This inner frame is gone before error formatting and the C boundary performs error/yield.
        const auto status = [&]() noexcept -> int {
            LuaValueSlots<std::remove_cvref_t<Arguments>...> values;
            if (!readLuaAbilityArguments<Policy, Arguments...>(
                state, values, std::index_sequence_for<Arguments...>{}, &failure
            ))
                return -3;
            if constexpr (can_reenter)
                if (!LuaAbilityProjectionAccess::revalidate(state, access)) return -1;
            if constexpr (std::is_void_v<Result>)
            {
                values.apply([&](auto&... arguments) noexcept { invoke(access, arguments...); });
                return 0;
            }
            else
            {
                using Value = std::remove_cvref_t<Result>;
                static_assert(std::is_trivially_copyable_v<Value>);
                const Value result = values.apply([&](auto&... arguments) noexcept -> Result {
                    return invoke(access, arguments...);
                });
                LuaValueWriter output{state};
                const auto base = ValueAccess::top(state);
                const auto written = LuaValueCodec<Value, Policy>::push(output, result);
                if (!written || ValueAccess::top(state) != base + 1)
                {
                    failure = written ? LuaValueFailure{ELuaValueError::CONSTRUCTION} : written.error();
                    failure->prepend("result");
                    ValueAccess::restoreScratch(state, base);
                    return -5;
                }
                return 1;
            }
        }();
        if (status < 0)
        {
            if (failure)
            {
                if (failure->path.back() != '\0') failure->truncated = true;
                failure->path.back() = '\0';
            }
            const bool has_path = failure && failure->path[0];
            const auto* message = has_path ? failure->path.data() : "Script Ability conversion or authority failure";
            return LuaAbilityProjectionAccess::fail(state, status, message);
        }
        return LuaAbilityProjectionAccess::succeed(state, status);
    }

    template <class Result, class... Arguments, class Invoke>
    [[nodiscard]] LuxLuaBoundaryOutcome invokeLuaAbility(lua_State* state, Invoke invoke) noexcept
    { return invokeLuaValueAbility<lux::script::lua::LuaValuePolicy, Result, Arguments...>(state, invoke); }

    template <class Policy, class Result, class... Arguments, class Start>
    [[nodiscard]] LuxLuaBoundaryOutcome startLuaValueAbility(lua_State* state, Start start) noexcept
    {
        using namespace lux::script::lua;
        using ValueAccess = lux::script::lua::detail::LuaValueAccess;
        LuaPreparedAbilityAccess access;
        if (!LuaAbilityProjectionAccess::current(state, access) || access.step == nullptr)
            return LuaAbilityProjectionAccess::fail(state, -1, "async Script Ability requires coroutine execution");
        if (access.argument_count != static_cast<int>(sizeof...(Arguments)))
            return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument count mismatch");
        // New record/enum async protocols are not part of SR-5's first batch.
        if constexpr (!((LuaValueScalar<std::remove_cvref_t<Arguments>> &&
            !LuaValueCodec<std::remove_cvref_t<Arguments>, Policy>::custom) && ...))
            return LuaAbilityProjectionAccess::fail(state, -5, "unsupported async Ability value");
        else
        {
            ScriptStepResult result;
            const auto converted = [&]() noexcept {
                LuaValueSlots<std::remove_cvref_t<Arguments>...> values;
                const bool read = readLuaAbilityArguments<Policy, Arguments...>(
                    state, values, std::index_sequence_for<Arguments...>{}
                );
                if (!read)
                    return false;
                result = values.apply([&](auto&... arguments) noexcept {
                    return invokePreparedScriptAbilityAsync<Result>(*access.step, access.local_async,
                        [&](lux::script::ScriptAbilityCompletion<Result> completion) noexcept {
                            return start(access, arguments..., std::move(completion));
                        }, arguments...);
                });
                return true;
            }();
            if (!converted) return LuaAbilityProjectionAccess::fail(state, -3, "Script Ability argument type mismatch");
            return LuaAbilityProjectionAccess::suspend(state, result, access.local_slot);
        }
    }
    template <class Result, class... Arguments, class Start>
    [[nodiscard]] LuxLuaBoundaryOutcome startLuaAbility(lua_State* state, Start start) noexcept
    { return startLuaValueAbility<lux::script::lua::LuaValuePolicy, Result, Arguments...>(state, start); }
}
