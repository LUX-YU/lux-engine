#pragma once
#include "LuaValueTestTypes.hpp"
#include <lux/engine/function/script/lua/LuaValue.hpp>
namespace lux::simulation::script::test
{
    inline void (*value_reentry)() noexcept{};
}
namespace lux::script::lua
{
    template <> struct LuaValueOverride<lux::simulation::script::test::ValuePushOnly, LuaValuePolicy>
    {
        inline static constexpr std::string_view name = "lux.test.push-only.degrees";
        inline static constexpr std::uint32_t version = 1;
        static LuaValueResult<void> push(LuaValueWriter& output,
            const lux::simulation::script::test::ValuePushOnly& value) noexcept
        { return output.number(value.radians * 57.29577951308232f); }
    };
    template <> struct LuaValueOverride<lux::simulation::script::test::ValueToken, LuaValuePolicy>
    {
        using Token = lux::simulation::script::test::ValueToken;
        inline static constexpr std::string_view name = "lux.test.token.factory";
        inline static constexpr std::uint32_t version = 1;
        static LuaValueResult<Token> read(LuaValueReader& input) noexcept
        {
            auto id = input.number<std::int32_t>();
            if (!id) return lux::cxx::unexpected(id.error());
            if (*id < 0) return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CONSTRUCTION});
            return Token::create(*id); // Pure C++ factory; no Lua operations while constructing.
        }
        static LuaValueResult<void> push(LuaValueWriter& output, const Token& token) noexcept
        { return output.number(token.value()); }
    };
    template <> struct LuaValueOverride<lux::simulation::script::test::ValueAngle, LuaValuePolicy>
    {
        using Angle = lux::simulation::script::test::ValueAngle;
        inline static constexpr std::string_view name = "lux.test.angle.degrees";
        inline static constexpr std::uint32_t version = 1;
        static LuaValueResult<Angle> read(LuaValueReader& input) noexcept
        {
            auto degrees = input.number<float>();
            if (!degrees) return lux::cxx::unexpected(degrees.error());
            if (auto reenter = lux::simulation::script::test::value_reentry) reenter();
            return Angle{*degrees * 0.017453292519943295f};
        }
        static LuaValueResult<void> push(LuaValueWriter& output, const Angle& value) noexcept
        { return output.number(value.radians * 57.29577951308232f); }
    };
}
