#pragma once
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>

namespace lux::editor::material
{
    using MaterialSessionAccess = sessions::TSessionAccess<MaterialSession>;
}
