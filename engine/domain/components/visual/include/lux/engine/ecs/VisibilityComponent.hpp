#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    // CPU preparation demand; CANDIDATE is not a GPU visibility or resource-readiness result.
    enum class EVisibilityRequirement
    {
        NONE,
        CANDIDATE,
        RETAIN
    };

    // Author permission and the current range result have separate writers.
    // Complete snapshots copy both; author scene assets persist only is_visible.
    struct LUX_COMPONENT(
        schema = "lux.ecs.VisibilityComponent", version = 1,
        snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT, create = DEFAULT
    ) VisibilityComponent final
    {
        bool is_visible = true;
        EVisibilityRequirement global_requirement = EVisibilityRequirement::NONE;
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/ecs/VisibilityComponent.type_static_info.hpp>
#endif
