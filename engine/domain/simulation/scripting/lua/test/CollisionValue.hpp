#pragma once
#include <lux/cxx/reflection/runtime/Marker.hpp>
#include <lux/engine/core/semantic/SemanticType.hpp>
#include <cstdint>

struct LUX_META(luxlua::value) CollisionEvent final
{
    std::int32_t body{};
    float impulse{};
};
namespace lux::semantic
{
    template <> struct TypeTraits<::CollisionEvent>
    {
        inline static constexpr std::string_view CanonicalName = "lux.physics.CollisionEvent";
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size = sizeof(::CollisionEvent);
        inline static constexpr std::uint32_t Alignment = alignof(::CollisionEvent);
    };
}
