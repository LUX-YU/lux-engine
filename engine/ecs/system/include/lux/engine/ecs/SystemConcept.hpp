#pragma once

#include <lux/engine/ecs/SystemAccessSpec.hpp>

#include <concepts>

namespace lux::ecs
{
    class SystemContext;

    template <class Type>
    concept System = requires(Type& system, SystemContext& context)
    {
        { Type::Access } -> std::convertible_to<SystemAccessSpec>;
        { system.update(context) } noexcept -> std::same_as<void>;
    };
}
