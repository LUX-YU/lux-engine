#pragma once
#include <lux/engine/ui/Ids.hpp>
#include <cstdint>

namespace lux::ui
{
    enum class ECommandPhase : std::uint8_t
    {
        QUERY,
        EXECUTE
    };
    enum class ECommandDispatchResult : std::uint8_t
    {
        NOT_FOUND,
        DISABLED,
        EXECUTED,
        FAILED
    };

    // A synchronous request, routed through the existing Object parent chain.
    // The first owner accepts it even when disabled. Labels belong to the menu.
    struct Command final
    {
        CommandIdView id;
        ECommandPhase phase{ECommandPhase::QUERY};
        bool enabled{};
        bool checked{};
        ECommandDispatchResult result{ECommandDispatchResult::NOT_FOUND};
    };
} // namespace lux::ui
