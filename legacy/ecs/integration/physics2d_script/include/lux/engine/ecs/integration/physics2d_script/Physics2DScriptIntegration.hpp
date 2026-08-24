#pragma once

#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>

namespace lux::ecs
{
    /// Install the script vocabulary shared by Physics2D and ScriptSystem.
    /// This is an explicit composition step: neither optional domain depends
    /// on the other and headless Physics2D does not acquire a script backend.
    [[nodiscard]] ScriptEventId registerPhysics2DScriptEvents();
}
