#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/function/script/lua/LuaValue.hpp>

#include <memory>
#include <span>

struct lua_State;

namespace lux::script::lua
{
    struct LuaValueOperation final
    {
        std::uint64_t semantic_type{};
        std::string_view canonical_name;
        std::size_t size{};
        std::size_t alignment{};
        std::uint64_t representation{};
        std::uint64_t policy{};
        std::size_t frame_bytes{};
        bool readable{};
        bool writable{};
        bool (*push)(lua_State *, const void *) noexcept {};
        bool native_scalar{};
    };
    template <class T, class Policy = LuaValuePolicy>
    [[nodiscard]] consteval LuaValueOperation makeLuaValueOperation() noexcept
    {
        using V = std::remove_cvref_t<T>;
        using Traits = lux::semantic::TypeTraits<V>;
        using Codec = LuaValueCodec<V, Policy>;
        return {lux::semantic::typeId(Traits::CanonicalName),
                Traits::CanonicalName,
                sizeof(V),
                alignof(V),
                Codec::representation(),
                lux::semantic::typeId(Policy::name) ^ Policy::version,
                Codec::storage,
                Codec::can_read && Codec::bounded,
                Codec::can_push && Codec::bounded,
                [](lua_State *state, const void *value) noexcept {
                    const auto top = detail::LuaValueAccess::top(state);
                    LuaValueWriter output{state};
                    const auto result = Codec::push(output, *static_cast<const V *>(value));
                    const bool valid = result && detail::LuaValueAccess::top(state) == top + 1;
                    if (!valid)
                        detail::LuaValueAccess::restoreScratch(state, top);
                    return valid;
                },
                LuaValueScalar<V> && !Codec::custom};
    }
    template <class Ability> struct ScriptAbilityLuaPolicy
    {
        using Type = LuaValuePolicy;
    };
    template <class Policy, class Result, class... Args> struct LuaAbilityValueSignature final
    {
        inline static constexpr std::array<LuaValueOperation, sizeof...(Args)> Parameters{
            makeLuaValueOperation<Args, Policy>()...};
        inline static constexpr auto Results = [] {
            if constexpr (std::is_void_v<Result>)
                return std::array<LuaValueOperation, 0>{};
            else
                return std::array{makeLuaValueOperation<Result, Policy>()};
        }();
    };
    struct ScriptAbilityLuaMethodProjection final
    {
        ScriptApiMethodIdView method;
        int (*entry)(lua_State *) noexcept {};
        std::span<const LuaValueOperation> parameters;
        std::span<const LuaValueOperation> results;
    };

    struct ScriptAbilityLuaContribution final
    {
        const ScriptAbilityDescription *description{};
        std::span<const ScriptAbilityLuaMethodProjection> methods;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return description != nullptr && description->id.isValid() &&
                   methods.size() == description->methods.size() && scriptAbilityCodeNameValid(description->name) &&
                   description->schema_version != 0U && description->schema_hash != 0U;
        }
    };

    template <class Ability> struct ScriptAbilityLuaTraits;

    template <class Ability>
    [[nodiscard]] constexpr ScriptAbilityLuaContribution makeScriptAbilityLuaContribution() noexcept
    {
        return {std::addressof(ScriptAbilityTraits<Ability>::Description), ScriptAbilityLuaTraits<Ability>::Methods};
    }
} // namespace lux::script::lua
