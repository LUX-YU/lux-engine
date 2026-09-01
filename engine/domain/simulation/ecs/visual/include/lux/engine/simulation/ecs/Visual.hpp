#pragma once

#include <lux/engine/description/Visual.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

namespace lux::simulation::ecs
{
    struct LUX_COMPONENT(schema = "lux.ecs.Mesh3D", version = 2, snapshot = COPY, semantic = DOMAIN_CONTRACT, editor = true) Mesh3D final
    {
        rdesc::MeshVisualDescription value{};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.Light3D", version = 2, snapshot = COPY, semantic = DOMAIN_CONTRACT, editor = true) Light3D final
    {
        rdesc::LightDescription value{};
    };
} // namespace lux::simulation::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/ecs/Visual.type_static_info.hpp>
#endif
