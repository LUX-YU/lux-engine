#pragma once
#include <lux/engine/core/semantic/SemanticType.hpp>

namespace lux::toolchain::lua::test
{
    struct CollisionEvent final
    {
        std::int32_t body{};
        float impulse{};
    };
}
namespace lux::semantic
{
    template <> struct TypeTraits<lux::toolchain::lua::test::CollisionEvent> final
    {
        inline static constexpr std::string_view CanonicalName{"lux.test.CollisionEvent"};
        inline static constexpr std::uint8_t AbiKind{static_cast<std::uint8_t>(EAbiKind::STRUCT_REF)};
        inline static constexpr std::uint32_t Size{sizeof(lux::toolchain::lua::test::CollisionEvent)};
        inline static constexpr std::uint32_t Alignment{alignof(lux::toolchain::lua::test::CollisionEvent)};
    };
}
