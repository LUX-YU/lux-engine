#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <string>
#include <vector>

namespace lux::editor
{
    enum class EWorkspaceAction : std::uint8_t
    {
        STATUS,
        SAVE,
        APPLY,
        RENAME,
        REMOVE,
        DEFAULT
    };
    // An editor command, never an asset VFS request. The application owns the retained I/O result.
    struct WorkspaceRequest final
    {
        EWorkspaceAction action{EWorkspaceAction::STATUS};
        std::string name;
        std::string new_name;
        std::uint64_t revision{};
        std::vector<std::string> layouts;
        std::string message;
        bool pending{};
        EditorResult<void> result;
    };
}
