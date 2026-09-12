#pragma once
#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/editor/sessions/scene/SceneAuthorState.hpp>
namespace lux::editor::sessions
{
    struct SceneEditInput final
    {
        SceneOpenInfo source;
        // The source creator supplies stable logical identities and the actual initial author values.
        // On failure both the Scene owner and these values remain with the caller.
        std::vector<SceneAuthorObject> objects;
    };
}
