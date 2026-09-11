#pragma once
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
namespace lux::simulation::na1
{
struct LUX_SCRIPT_ABILITY(id = lux.na1.probe, name = TaskProbe, display = TaskProbe, version = 1,
                          receiver = provider_instance) TaskProbe
{
    LUX_SCRIPT_QUERY(id = lux.na1.probe.hit, display = Hit, result_lifetime = owned_value)
    std::int32_t hit(LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t code) noexcept;
};
} // namespace lux::simulation::na1
