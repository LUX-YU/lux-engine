#pragma once
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>

namespace lux::editor::scene::detail
{
    editing::EditOperationPtr makeSceneObjectEdit(SceneEditor &editor, SceneObjects &objects, editing::StateId base,
                                                  std::vector<ObjectContent> content, bool creation, std::string label);
}
