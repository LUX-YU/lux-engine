#pragma once

#include <lux/engine/editor/sessions/SessionCommands.hpp>

namespace lux::editor::sessions::detail
{
    // Shared by synchronous role consumers and the declared Process-facing opening owner.
    [[nodiscard]] std::unique_ptr<commands::CommandBinding> makeHistoryBinding(
        SessionStore&, std::shared_ptr<HistoryActionLookup>, bool forward
    );
} // namespace lux::editor::sessions::detail
