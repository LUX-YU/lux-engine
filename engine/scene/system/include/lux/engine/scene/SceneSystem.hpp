#pragma once

#include <lux/engine/system/SystemTypeDescription.hpp>

#include <type_traits>

namespace lux::scene
{
    template <class Type>
    concept SceneSystem = requires {
        requires system::validSystemTypeDescription(Type::Description);
        requires std::is_nothrow_destructible_v<Type>;
    };
} // namespace lux::scene
