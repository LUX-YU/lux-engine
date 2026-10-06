#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>

namespace lux::editor::commands::detail
{
    // Controlled collision fixture only, not installed. Preserve a fully validated entry and place
    // its numeric locator under another already valid identity's hash; real resolution/dispatch runs unchanged.
    struct CommandIndexTestAccess final
    {
        [[nodiscard]] static CommandResult<CommandRegistrySnapshot> withSingleHash(
            const CommandRegistrySnapshot&,
            std::uint64_t
        );
    };
} // namespace lux::editor::commands::detail
