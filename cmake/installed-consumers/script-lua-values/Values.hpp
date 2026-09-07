#pragma once
#include "Field.hpp"
#include <lux/cxx/reflection/runtime/Marker.hpp>
#include <lux/engine/core/semantic/SemanticType.hpp>
struct LUX_META(luxlua::value) Item
{
    LUX_META(luxlua::field, name = key) ValueScalar id;
    double weight;
};
namespace lux::semantic
{
    template <> struct TypeTraits<Item>
    {
        inline static constexpr std::string_view CanonicalName = "consumer.item";
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size = sizeof(Item);
        inline static constexpr std::uint32_t Alignment = alignof(Item);
    };
}
