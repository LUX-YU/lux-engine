#pragma once
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <cstdint>
namespace installed_consumer
{
struct LUX_SCRIPT_ABILITY(id = installed.na1.probe, name = Probe, display = Probe, version = 1,
    receiver = provider_instance) ProbeAbility
{
    LUX_SCRIPT_COMMAND(id = installed.na1.probe.hit, display = Hit)
    void hit(LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value) noexcept;
};
}
