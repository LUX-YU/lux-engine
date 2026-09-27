#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>
namespace lux::editor
{
    class EditorContext;
}
namespace lux::editor::scene
{
    class SceneEditor;
}
namespace lux::editor::ui
{
    [[nodiscard]] EditorResult<std::unique_ptr<lux::ui::Pane>>
    createSceneCreationPane(scene::SceneEditor&, EditorContext&);
}
