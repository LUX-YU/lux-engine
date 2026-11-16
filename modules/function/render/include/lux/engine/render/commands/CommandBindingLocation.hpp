#pragma once

#include <lux/engine/render/commands/Command.hpp>

namespace lux::render
{
    // Non-owning position in the unique CommandRegistry; binding positions remain stable.
    struct CommandBindingLocation final
    {
        CommandId command;
        std::uint32_t binding = 0;
        bool operator==(const CommandBindingLocation&) const noexcept = default;
    };
} // namespace lux::render
