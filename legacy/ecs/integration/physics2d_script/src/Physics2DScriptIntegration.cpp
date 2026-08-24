#include <lux/engine/ecs/integration/physics2d_script/Physics2DScriptIntegration.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <cstdint>

namespace lux::ecs
{
    ScriptEventId registerPhysics2DScriptEvents()
    {
        return scriptEventRegistry().registerEvent(
            "OnCollision2DEnter",
            {ScriptEventParam{
                &lux::meta::ref_type_of_v<std::uint32_t>,
                "other"}});
    }
}
