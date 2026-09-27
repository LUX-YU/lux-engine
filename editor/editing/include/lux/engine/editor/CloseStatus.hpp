#pragma once

#include <lux/engine/editor/EditorError.hpp>

namespace lux::editor
{
    enum class ECloseState : std::uint8_t
    {
        OPEN,
        CLOSING,
        CLOSED
    };

    struct CloseStatus final
    {
        ECloseState state{ECloseState::OPEN};
        std::string waiting_for;
        EditorResult<void> progress;
    };
}
