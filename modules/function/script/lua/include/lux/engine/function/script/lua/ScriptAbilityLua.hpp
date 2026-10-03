#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>

#include <memory>
#include <cstring>
#include <span>

struct lua_State;

namespace lux::script::lua
{
    using LuaResumeValuePush = bool (*)(lua_State*, std::span<const std::byte>) noexcept;

    namespace detail
    {
        template <class T, class Policy> bool pushValue(lua_State* state, const T& value) noexcept
        {
            const auto top = LuaValueAccess::top(state);
            LuaValueWriter output{state};
            const auto result = TLuaValueCodec<T, Policy>::push(output, value);
            const bool valid = result && LuaValueAccess::top(state) == top + 1;
            if (!valid) LuaValueAccess::restoreScratch(state, top);
            return valid;
        }
    }

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
        bool (*push)(lua_State*, const void*) noexcept {};
        bool native_scalar{};
        bool (*prepare)(lua_State*) noexcept {};
        // Present only for a bounded trivially-copyable native value. Resume bytes are
        // reconstructed into an aligned T, then passed through the same protected codec.
        LuaResumeValuePush push_resume{};
    };
    template <class T, class Policy = LuaValuePolicy>
    [[nodiscard]] consteval LuaValueOperation makeLuaValueOperation() noexcept
    {
        using V = std::remove_cvref_t<T>;
        using Traits = lux::semantic::TTypeTraits<V>;
        using Codec = TLuaValueCodec<V, Policy>;
        return {
            lux::semantic::typeId(Traits::CanonicalName),
            Traits::CanonicalName,
            sizeof(V),
            alignof(V),
            Codec::representation(),
            lux::semantic::typeId(Policy::name) ^ Policy::version,
            Codec::storage,
            Codec::can_read && Codec::bounded,
            Codec::can_push && Codec::bounded,
            [](lua_State* state, const void* value) noexcept {
                return detail::pushValue<V, Policy>(state, *static_cast<const V*>(value));
            },
            LuaValueScalar<V> && !Codec::custom,
            &Codec::prepare,
            []() consteval -> LuaResumeValuePush {
                if constexpr (std::is_trivially_copyable_v<V> && Codec::can_push && Codec::bounded)
                    return [](lua_State* state, std::span<const std::byte> bytes) noexcept {
                        if (bytes.size() != sizeof(V)) return false;
                        std::array<std::byte, sizeof(V)> owned{};
                        std::memcpy(owned.data(), bytes.data(), owned.size());
                        const auto value = std::bit_cast<V>(owned);
                        return detail::pushValue<V, Policy>(state, value);
                    };
                else
                    return nullptr;
            }()
        };
    }
    template <class Ability> struct TScriptAbilityLuaPolicy
    {
        using Type = LuaValuePolicy;
    };
    template <class Policy, class Result, class... Args> struct TLuaAbilityValueSignature final
    {
        inline static constexpr std::array<LuaValueOperation, sizeof...(Args)> Parameters{
            makeLuaValueOperation<Args, Policy>()...
        };
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
        LuxLuaTypedWorker entry{};
        std::span<const LuaValueOperation> parameters;
        std::span<const LuaValueOperation> results;
    };

    struct ScriptAbilityLuaContribution final
    {
        const ScriptAbilityDescription* description{};
        std::span<const ScriptAbilityLuaMethodProjection> methods;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return description != nullptr && description->id.isValid() &&
                   methods.size() == description->methods.size() && scriptAbilityCodeNameValid(description->name) &&
                   description->schema_version != 0U && description->schema_hash != 0U;
        }
    };

    template <class Ability> struct TScriptAbilityLuaTraits;

    template <class Ability>
    [[nodiscard]] constexpr ScriptAbilityLuaContribution makeScriptAbilityLuaContribution() noexcept
    {
        return {std::addressof(TScriptAbilityTraits<Ability>::Description), TScriptAbilityLuaTraits<Ability>::Methods};
    }
} // namespace lux::script::lua
