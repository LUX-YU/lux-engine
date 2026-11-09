#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    // Declares a Mesh rendering representation, independently of its source asset association.
    struct LUX_COMPONENT(
        schema = "lux.ecs.RenderableMeshComponent", version = 1,
        snapshot = COPY, persistent = true, semantic = DOMAIN_CONTRACT, create = DEFAULT
    ) RenderableMeshComponent final
    {
        bool keep_resources_when_hidden = false;
        bool cast_shadow = true;
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/ecs/RenderableMeshComponent.type_static_info.hpp>
#endif
