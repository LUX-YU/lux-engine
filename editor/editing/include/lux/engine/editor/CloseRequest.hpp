#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <optional>

namespace lux::ui
{
    class Pane;
}

namespace lux::editor
{
    enum class ECloseAction : std::uint8_t
    {
        REVIEW,
        CANCEL
    };
    enum class EClosePurpose : std::uint8_t
    {
        HIDE,
        DESTROY,
        EXIT
    };
    enum class ECloseDecision : std::uint8_t
    {
        READY,
        CANCELLED,
        FAILED
    };

    struct CloseRequest final
    {
        std::uint64_t id{};
        ECloseAction action{ECloseAction::REVIEW};
        EClosePurpose purpose{EClosePurpose::EXIT};
    };

    // Delivered synchronously to the owning Root. The request identity rejects an obsolete review reply.
    struct CloseDecision final
    {
        std::uint64_t request{};
        lux::ui::Pane* participant{};
        ECloseDecision decision{ECloseDecision::READY};
        std::optional<EditorFailure> failure;
    };

    // Intent only. The host starts the review after the current object traversal returns.
    struct PaneCloseRequest final
    {
        lux::ui::Pane* pane{};
        EClosePurpose purpose{EClosePurpose::DESTROY};
    };
}
