#pragma once

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/inspector/InspectorUndoJournal.hpp>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::inspector
{
    struct InspectorContext final
    {
        EditorContext& editor;
        ui::Frame& frame;
        InspectorUndoJournal& undo;
        EditorSelectionValue target;
    };
} // namespace lux::editor::inspector
