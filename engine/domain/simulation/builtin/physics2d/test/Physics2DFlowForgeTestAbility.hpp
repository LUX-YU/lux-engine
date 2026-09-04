#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

namespace lux::physics2d::test
{
    struct LUX_SCRIPT_ABILITY(id = lux.physics2d.test.capture,
                              name = PhysicsCapture,
                              display = Physics Capture,
                              version = 1,
                              receiver = provider_instance) Physics2DCaptureAbility
    {
        LUX_SCRIPT_COMMAND(id = lux.physics2d.test.capture.value, display = Capture)
        void capture(LUX_SCRIPT_PARAM(lifetime = owned_value) bool value) noexcept;
    };
}
