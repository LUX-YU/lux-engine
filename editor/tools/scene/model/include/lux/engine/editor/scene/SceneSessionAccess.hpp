#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>

namespace lux::editor::scene
{
    using SceneSessionAccess = sessions::TSessionAccess<SceneSession>;
}
