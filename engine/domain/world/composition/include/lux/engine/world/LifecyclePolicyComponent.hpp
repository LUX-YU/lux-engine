#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::world
{
    enum class ELifecyclePolicy
    {
        AUTOMATIC,
        MANUAL
    };

    // Controls automatic unloading; neither mode prevents final Scene destruction.
    struct LUX_COMPONENT(
        schema = "lux.world.LifecyclePolicyComponent", version = 1,
        snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT, create = DEFAULT
    ) LifecyclePolicyComponent final
    {
        ELifecyclePolicy policy{ELifecyclePolicy::AUTOMATIC};
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/world/LifecyclePolicyComponent.type_static_info.hpp>
#endif
