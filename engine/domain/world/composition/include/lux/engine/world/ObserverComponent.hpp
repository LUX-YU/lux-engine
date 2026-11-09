#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::world
{
    // A loading demand around this entity's Transform; independent of any camera or Render View.
    struct LUX_COMPONENT(
        schema = "lux.world.ObserverComponent", version = 1,
        snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT, create = DEFAULT
    ) ObserverComponent final
    {
        double range{};
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/world/ObserverComponent.type_static_info.hpp>
#endif
