#pragma once

namespace lux::editor::scene::detail
{
    // Presence marks an editor-only entity. It never enters author capture or a Run.
    struct EditorEntity final
    {
        bool deletable{false};
    };
} // namespace lux::editor::scene::detail
